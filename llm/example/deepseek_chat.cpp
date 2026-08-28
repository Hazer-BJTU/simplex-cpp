// deepseek_chat.cpp — the llm-stack edition of the DeepSeek chat example.
//
// endpoint/example/deepseek_chat.cpp is the hand-rolled prototype: it wires
// interpreter + reader + endpoint::complete directly. This one climbs one
// rung up the stack and exercises the provider machinery the llm package
// actually ships: LLMDispatcher loads the bundled provider plugins from
// <exe>/plugins/llm, create_model("deepseek", ...) mints a configured
// ChatCompletionsModel through the plugin factory, and the conversation runs
// entirely through the LLMModel contract — converse() for one exchange,
// integrate() to fold each result back into the growing AgentInputState.
// Everything DeepSeek-specific (endpoint defaults, thinking mode, effort
// vocabulary, reasoning replay, cache-hit accounting) stays inside the
// dialect; this file never mentions it.
//
// Live observation rides the process-wide event bus: converse() broadcasts
// every streamed reasoning increment as a ReasoningDeltaEvent (llm/
// chat_completions/events.hpp), and this executable subscribes once on
// eventbus::default_bus() to mirror the thinking to stderr as it arrives.
// Visible content still prints per exchange — converse() owns its SSE
// reader, so content-side live view is a later bus event. The bus singleton
// lives in the SHARED eventbus library, so this executable and the dlopened
// provider .so bind the same SONAME — one bus per process by construction,
// with no host-side symbol-export convention to remember.
//
// The registered tool is parameterised (calculate: a, b, operation) so the
// full schema → wire arguments → parse → execute → correlated-result cycle
// is exercised, unlike the endpoint example's zero-argument get_current_time.
//
// `--list-models` is the catalogue smoke entry: provider_info()'s GET(s)
// over the same endpoint/auth, printing the provider's live model list —
// and, because the DeepSeek dialect attaches the account-balance companion,
// the live balance beside it — then exiting before any conversation wiring.

#include "eventbus/event_bus.hpp"
#include "llm/chat_completions/events.hpp"
#include "llm/models.hpp"

#include <boost/asio.hpp>

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace asio = boost::asio;

namespace {

// ---- configuration -----------------------------------------------------------

// Default model. Override at runtime with the DEEPSEEK_MODEL environment
// variable.
const char* model_name() {
    if (const char* env = std::getenv("DEEPSEEK_MODEL")) return env;
    return "deepseek-v4-flash";
}

// ---- the one registered tool: parameterised arithmetic -----------------------

/// The tool registration (dataclass/model_io.hpp): the wire triple providers
/// expect — name, description, JSON-Schema for the arguments. Unlike the
/// endpoint example's zero-argument tool, this schema drives the whole
/// parameter cycle: the model must emit `a`, `b` and `operation` as wire
/// JSON, and the reader parses them back into InvokeQuery::arguments before
/// execution sees them.
model_io::Invocable calculate_tool() {
    model_io::Invocable tool;
    tool.name = "calculate";
    tool.description =
        "Evaluate one two-operand arithmetic expression exactly. Use this "
        "for any arithmetic instead of computing the answer yourself.";
    tool.argument_schema = {
        {"type", "object"},
        {"properties", {
            {"a", {
                {"type", "number"},
                {"description", "The left operand"},
            }},
            {"b", {
                {"type", "number"},
                {"description", "The right operand"},
            }},
            {"operation", {
                {"type", "string"},
                {"enum", nlohmann::json::array({"add", "subtract",
                                                "multiply", "divide"})},
                {"description", "The binary operation to apply"},
            }},
        }},
        {"required", nlohmann::json::array({"a", "b", "operation"})},
        {"additionalProperties", false},
    };
    return tool;
}

/// Read one operand leniently: a number as-is, a numeric string parsed, so a
/// model that quotes its arguments ("2") still gets an answer instead of a
/// schema complaint.
bool operand(const nlohmann::json& arguments, const char* key, double& out) {
    const auto it = arguments.find(key);
    if (it == arguments.end()) return false;
    if (it->is_number()) {
        out = it->get<double>();
        return true;
    }
    if (it->is_string()) {
        try {
            out = std::stod(it->get<std::string>());
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }
    return false;
}

/// Human-friendly formatting: whole values print without a decimal tail.
std::string format_number(double value) {
    if (value == static_cast<long long>(value)) {
        return std::to_string(static_cast<long long>(value));
    }
    return std::to_string(value);
}

/// Execute one tool call and shape the result as the contract wants it: the
/// payload in content (the canonical position, role "tool") PLUS the
/// originating call embedded as the InvokeReturn provenance record, whose
/// query.id is what the next request correlates the result by. Errors are
/// reported TO the model as the result text, so it can recover in the next
/// cycle instead of the loop dying.
model_io::MessageItem execute_tool(const model_io::InvokeQuery& query) {
    model_io::InvokeReturn record;
    record.query = query;
    record.output.type = model_io::ContentType::Text;

    double a = 0;
    double b = 0;
    std::string operation;
    if (query.arguments.is_object() && operand(query.arguments, "a", a) &&
        operand(query.arguments, "b", b) &&
        query.arguments.value("operation", std::string()) != "") {
        operation = query.arguments.value("operation", std::string());
        if (operation == "add") {
            record.output.raw = format_number(a + b);
        } else if (operation == "subtract") {
            record.output.raw = format_number(a - b);
        } else if (operation == "multiply") {
            record.output.raw = format_number(a * b);
        } else if (operation == "divide") {
            record.output.raw = b == 0
                ? "error: division by zero"
                : format_number(a / b);
        } else {
            record.output.raw = "error: unknown operation \"" + operation + "\"";
        }
    } else if (query.name != "calculate") {
        record.output.raw = "error: unknown tool \"" + query.name + "\"";
    } else {
        record.output.raw =
            "error: expected number arguments \"a\" and \"b\" and operation "
            "\"add\"|\"subtract\"|\"multiply\"|\"divide\"";
    }

    model_io::MessageItem item;
    item.type = model_io::MessageItemType::InvokeReturn;
    item.role = "tool";
    item.content.push_back(record.output);
    item.invoke_return = std::move(record);
    return item;
}

// ---- one user turn: the ReAct agent loop --------------------------------------

constexpr std::size_t kMaxAgentSteps = 8;

/// Run the agent loop for the last user turn through the LLMModel contract:
/// converse() for one exchange, integrate() to fold every item into the
/// growing AgentInputState (user turn, model responses, tool results — the
/// placement table in llm/models.hpp does the routing). A response with no
/// invokes is the final answer; otherwise execute the calls, integrate the
/// results, and exchange again.
asio::awaitable<void> run_turn(llm::LLMModel& model,
                               model_io::AgentInputState& state) {
    for (std::size_t step = 0; step < kMaxAgentSteps; ++step) {
        model_io::MessageItem item = co_await model.converse(state);
        model.integrate(state, item);

        // Reasoning already streamed live through the bus subscription in
        // main(); the visible answer prints here, per exchange.
        for (const model_io::Content& part : item.content) {
            if (!part.raw.empty()) std::cout << part.raw << "\n";
        }

        if (!item.invokes || item.invokes->empty()) {
            // Final answer. Report the exchange's token accounting (the
            // cache_hit field is DeepSeek's prompt_cache_hit_tokens bridged
            // by the dialect).
            if (item.cost) {
                std::cerr << "[cost] prompt=" << item.cost->prompt
                          << " generated=" << item.cost->generated
                          << " cache_hit=" << item.cost->cache_hit << "\n";
            }
            co_return;
        }

        // ReAct: execute every call, fold the results back as one cycle.
        for (const model_io::InvokeQuery& query : *item.invokes) {
            std::cout << "  [tool] id=\"" << query.id << "\" name=\""
                      << query.name << "\" arguments="
                      << query.arguments.dump();
            model_io::MessageItem result = execute_tool(query);
            model.integrate(state, result);
            std::cout << " -> "
                      << (result.content.empty()
                              ? std::string()
                              : result.content.front().raw)
                      << "\n";
        }
    }
    std::cerr << "agent loop exhausted its step budget before a final "
                 "answer\n";
}

} // namespace

int main(int argc, char* argv[]) {
    std::cout << "=== DeepSeek chat via the llm provider stack "
                 "(plugin + converse/integrate, one parameterised tool) ===\n";

    const bool list_models_only =
        argc > 1 && std::string_view(argv[1]) == "--list-models";

    std::string api_key;
    std::cout << "api_key [env DEEPSEEK_API_KEY]: ";
    if (!std::getline(std::cin, api_key) || api_key.empty()) {
        if (const char* env = std::getenv("DEEPSEEK_API_KEY")) {
            api_key = env;
        }
    }
    if (api_key.empty()) {
        std::cerr << "no api_key provided; exiting.\n";
        return 1;
    }

    std::string effort = "high";
    if (!list_models_only) {
        std::cout << "reasoning effort [high] (none|minimal|low|medium|high|"
                     "xhigh|max; none/minimal disable thinking): ";
        if (std::string line; std::getline(std::cin, line) && !line.empty()) {
            effort = line;
        }
    }

    // Load the provider plugins the build emits next to this executable.
    // The registry is then a concurrent-read router: provider name → factory.
    llm::LLMDispatcher dispatcher;
    const std::size_t loaded = dispatcher.load_default_models();
    if (loaded == 0) {
        std::cerr << "no provider plugins under <exe>/plugins/llm; build the "
                     "llm_deepseek target first.\n";
        return 1;
    }
    std::cout << "loaded " << loaded << " provider plugin(s)\n";

    asio::io_context io;
    const nlohmann::json config{
        {"model", model_name()},
        {"reasoning", {{"effort", effort}}},
        // The dialect's endpoint defaults apply underneath: base_url
        // https://api.deepseek.com, /chat/completions, Bearer — only the
        // credential needs supplying. Retry stays at the adapter defaults
        // (3 retries, 500ms..120s backoff).
        {"endpoint", {{"auth", {{"api_key", api_key}}}}},
    };
    auto model = dispatcher.create_model("deepseek", io.get_executor(), config);
    if (!model) {
        std::cerr << "provider \"deepseek\" did not load; is the plugin "
                     "built and ABI-matched?\n";
        return 1;
    }

    // The catalogue mode: the provider's live model list (plus the balance
    // companion the DeepSeek dialect attaches) over the same endpoint/auth,
    // then exit — no conversation wiring, no tool.
    if (list_models_only) {
        try {
            std::size_t count = 0;
            auto future = asio::co_spawn(
                io,
                [&]() -> asio::awaitable<void> {
                    nlohmann::json catalogue = co_await model->provider_info();
                    // Array = the bare models list; an object carries the
                    // models under "models" with provider extras (the
                    // balance companion) as siblings.
                    const nlohmann::json models =
                        catalogue.is_array()
                            ? catalogue
                            : catalogue.value(
                                  "models", nlohmann::json::array());
                    count = models.size();
                    for (const auto& entry : models) {
                        std::cout << "  " << entry.dump() << "\n";
                    }
                    if (catalogue.is_object() &&
                        catalogue.contains("balance")) {
                        std::cout << "balance:\n"
                                  << catalogue["balance"].dump(2) << "\n";
                    }
                },
                asio::use_future);
            io.run();
            future.get();
            std::cout << count << " model(s) offered by deepseek\n";
        } catch (const std::exception& error) {
            // Base catch only: the chain runs inside the provider .so (same
            // rationale as the turn loop below).
            std::cerr << "provider_info failed: " << error.what() << "\n";
            return 1;
        }
        return 0;
    }

    // The conversation half (dataclass/model_io.hpp): system prompt,
    // registered tools, and the turn list the agent loop integrates into.
    model_io::AgentInputState state;
    state.system_prompt.add_section(
        "persona", "", "You are a helpful assistant.",
        model_io::SectionStability::Immutable);
    state.tools.push_back(calculate_tool());

    std::cout << "model: " << model_name() << " (thinking "
              << (effort == "none" || effort == "minimal" ? "disabled"
                                                          : "enabled")
              << ", effort " << effort << ")\n";
    std::cout << "tool registered: calculate (a, b, operation) — try: "
                 "\"what is 1234 * 5678?\"\n";
    std::cout << "reasoning streams live to stderr via the process event "
                 "bus; content prints per exchange.\n";
    std::cout << "empty line to quit.\n";

    // The live view: every provider's reasoning increments, mirrored to
    // stderr as they stream. Synchronous bus — the slot runs inline on the
    // exchange's I/O thread in wire order (and must not throw).
    eventbus::EventBus::ScopedSubscription reasoning_view =
        eventbus::default_bus()
            .subscribe<llm::chat_completions::ReasoningDeltaEvent>(
                [](const llm::chat_completions::ReasoningDeltaEvent& event) {
                    std::cerr << event.reasoning << std::flush;
                });

    std::string line;
    while (std::cout << "\nyou> " && std::getline(std::cin, line)) {
        if (line.empty()) break;

        model_io::MessageItem input;
        input.type = model_io::MessageItemType::UserInput;
        input.role = "user";
        model_io::Content text;
        text.type = model_io::ContentType::Text;
        text.raw = line;
        input.content.push_back(std::move(text));
        model->integrate(state, input);

        try {
            auto future = asio::co_spawn(
                io, run_turn(*model, state), asio::use_future);
            io.restart();   // a prior turn's run() drained the context
            io.run();
            future.get();
        } catch (const std::exception& error) {
            // Deliberately the base catch only: converse() runs inside the
            // provider .so, and its adapter types (e.g. the API exception)
            // carry one typeinfo per module — exact-type catches across the
            // dlopen boundary are not dependable. what() already carries
            // the API's own diagnosis when the exchange reached the server.
            std::cerr << "turn failed: " << error.what() << "\n";
            state.turns.pop_back();   // drop the unanswered turn
        }
    }

    return 0;
}
