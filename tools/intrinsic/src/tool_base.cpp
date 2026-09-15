#include "tools/intrinsic/tool_base.hpp"

#include <format>
#include <optional>
#include <utility>

#include "tools/intrinsic/tool_declaration.hpp"
#include "tools/invoke_exception.hpp"

namespace tools::intrinsic {

IntrinsicTool::IntrinsicTool(eventbus::AsyncEventBus* bus) noexcept
    : _bus(bus)
{}

const model_io::Invocable& IntrinsicTool::get_details() const noexcept
{
    return _details;
}

// ---- the declared form ------------------------------------------------------

DeclaredTool::DeclaredTool(const std::filesystem::path& declaration_file,
                           eventbus::AsyncEventBus* bus)
    : IntrinsicTool(bus)
{
    // Reported, not thrown: the tool is left UNNAMED, and that is the whole
    // mechanism — IntrinsicToolSet::register_tools() skips a tool with no name,
    // so a declaration nobody can load costs this one tool and not the host.
    std::optional<ToolDeclaration> declaration =
        try_load_tool_declaration(declaration_file);
    if (!declaration) {
        return;
    }

    // Copied into the tool's own storage: get_details() hands out a reference,
    // so what it points at has to live as long as the tool does.
    _details.name = std::move(declaration->name);
    _details.description = std::move(declaration->description);
    _details.argument_schema = std::move(declaration->argument_schema);
}

boost::asio::awaitable<std::tuple<bool, std::string>>
IntrinsicTool::security_check(const model_io::InvokeQuery& query)
{
    // The module's whole policy, unchanged — this override only chooses WHICH
    // bus the RequireConfirm question is asked on. Resolved here rather than in
    // the constructor: default_async_bus() is a shared-library singleton, and a
    // tool built in one module must still reach the one bus per process.
    co_return co_await default_security_check(
        query, _bus != nullptr ? *_bus : eventbus::default_async_bus());
}

// ---- argument reading -------------------------------------------------------

const nlohmann::json* IntrinsicTool::find_argument(
    const model_io::InvokeQuery& query, std::string_view key)
{
    if (!query.arguments.is_object()) return nullptr;
    const auto found = query.arguments.find(key);
    if (found == query.arguments.end() || found->is_null()) return nullptr;
    return &*found;
}

std::string IntrinsicTool::require_string(const model_io::InvokeQuery& query,
                                         std::string_view key,
                                         std::string_view what_it_is)
{
    const nlohmann::json* value = find_argument(query, key);
    if (value == nullptr) {
        bad_argument(std::format(
            "missing required property \"{}\": {}", key, what_it_is));
    }
    if (!value->is_string()) {
        bad_argument(std::format("property \"{}\" must be a string", key));
    }
    std::string text = value->get<std::string>();
    if (text.empty()) {
        bad_argument(std::format("property \"{}\" must not be empty", key));
    }
    return text;
}

std::string IntrinsicTool::optional_string(const model_io::InvokeQuery& query,
                                          std::string_view key,
                                          std::string_view fallback)
{
    const nlohmann::json* value = find_argument(query, key);
    if (value == nullptr) return std::string(fallback);
    if (!value->is_string()) {
        bad_argument(std::format("property \"{}\" must be a string", key));
    }
    return value->get<std::string>();
}

bool IntrinsicTool::optional_bool(const model_io::InvokeQuery& query,
                                  std::string_view key, bool fallback)
{
    const nlohmann::json* value = find_argument(query, key);
    if (value == nullptr) return fallback;
    if (!value->is_boolean()) {
        // Deliberately no coercion: a "false" string coerces to true under
        // every truthiness rule, and a model that gets the opposite of what it
        // asked for has no way to see why.
        bad_argument(std::format("property \"{}\" must be a boolean", key));
    }
    return value->get<bool>();
}

std::uint64_t IntrinsicTool::optional_uint(const model_io::InvokeQuery& query,
                                           std::string_view key,
                                           std::uint64_t fallback)
{
    const nlohmann::json* value = find_argument(query, key);
    if (value == nullptr) return fallback;
    // is_number_integer() covers BOTH of nlohmann's integer kinds, and the
    // sign is checked separately. is_number_unsigned() alone would not do: a
    // plain positive literal (5000, or anything a provider sends as a JSON
    // integer) is stored as a SIGNED integer, so it would be rejected —
    // rejecting every valid value while accepting none.
    if (!value->is_number_integer()) {
        // Floats and strings are refused rather than coerced: truncating 1.9
        // to 1, or reading "soon" as 0, would silently run a different call
        // than the one that was asked for.
        bad_argument(std::format(
            "property \"{}\" must be a non-negative integer", key));
    }
    if (!value->is_number_unsigned() && value->get<std::int64_t>() < 0) {
        // Negative would wrap into an enormous unsigned value — a "timeout" of
        // a few hundred million years reads as a hang.
        bad_argument(std::format("property \"{}\" must not be negative", key));
    }
    return value->get<std::uint64_t>();
}

std::vector<std::string> IntrinsicTool::optional_string_list(
    const model_io::InvokeQuery& query, std::string_view key)
{
    const nlohmann::json* value = find_argument(query, key);
    if (value == nullptr) return {};
    if (!value->is_array()) {
        bad_argument(std::format(
            "property \"{}\" must be an array of strings", key));
    }
    std::vector<std::string> items;
    items.reserve(value->size());
    for (std::size_t index = 0; index < value->size(); ++index) {
        // Element by element, so a single bad entry names its own index rather
        // than failing as a type error from inside the whole-array conversion.
        if (!(*value)[index].is_string()) {
            bad_argument(std::format(
                "property \"{}\"[{}] must be a string", key, index));
        }
        items.push_back((*value)[index].get<std::string>());
    }
    return items;
}

// ---- settling arguments -----------------------------------------------------
//
// Each of these is "validate like the optional_* twin, then make the answer
// part of the call". The order matters: the read validates FIRST, so a
// malformed value is refused before anything is written, and the write only
// happens when the property really was absent (find_argument answers that,
// with JSON null counting as absent — the module's one rule).

void IntrinsicTool::write_argument(model_io::InvokeQuery& query,
                                   std::string_view key,
                                   nlohmann::json value)
{
    if (query.arguments.is_null()) {
        // Null arguments and no arguments are the same call; the settled form
        // spells it as the empty object so the write below has somewhere to go.
        query.arguments = nlohmann::json::object();
    }
    if (!query.arguments.is_object()) {
        bad_argument(std::format(
            "the call's \"arguments\" must be a JSON object, got {}",
            query.arguments.type_name()));
    }
    query.arguments[std::string(key)] = std::move(value);
}

std::string IntrinsicTool::settle_string(model_io::InvokeQuery& query,
                                         std::string_view key,
                                         std::string_view fallback)
{
    const std::string value = optional_string(query, key, fallback);
    if (find_argument(query, key) == nullptr) {
        write_argument(query, key, value);
    }
    return value;
}

bool IntrinsicTool::settle_bool(model_io::InvokeQuery& query,
                                std::string_view key, bool fallback)
{
    const bool value = optional_bool(query, key, fallback);
    if (find_argument(query, key) == nullptr) {
        write_argument(query, key, value);
    }
    return value;
}

std::uint64_t IntrinsicTool::settle_uint(model_io::InvokeQuery& query,
                                         std::string_view key,
                                         std::uint64_t fallback)
{
    const std::uint64_t value = optional_uint(query, key, fallback);
    if (find_argument(query, key) == nullptr) {
        write_argument(query, key, value);
    }
    return value;
}

std::vector<std::string> IntrinsicTool::settle_string_list(
    model_io::InvokeQuery& query, std::string_view key)
{
    const std::vector<std::string> value = optional_string_list(query, key);
    if (find_argument(query, key) == nullptr) {
        // An empty list, not a missing property: every reader of the settled
        // query then answers "none of them" without a second question.
        write_argument(query, key, nlohmann::json::array());
    }
    return value;
}

// ---- failures ---------------------------------------------------------------

void IntrinsicTool::bad_argument(std::string message)
{
    // Stage::ArgumentParse is the one failure class a model can fix on its
    // own, so the message is the whole value of this path: it names the
    // property and says what was expected. The tool name comes from the query
    // the invocation layer correlates the record to.
    throw InvokeException(InvokeException::Stage::ArgumentParse,
                          std::move(message));
}

void IntrinsicTool::invoke_failed(std::string message)
{
    throw InvokeException(InvokeException::Stage::Invoke, std::move(message));
}

// ---- schema helpers ---------------------------------------------------------

nlohmann::json IntrinsicTool::string_property(std::string_view description)
{
    return nlohmann::json{{"type", "string"}, {"description", description}};
}

nlohmann::json IntrinsicTool::bool_property(std::string_view description,
                                            bool fallback)
{
    return nlohmann::json{{"type", "boolean"},
                          {"description", description},
                          {"default", fallback}};
}

nlohmann::json IntrinsicTool::uint_property(std::string_view description,
                                            std::uint64_t fallback)
{
    return nlohmann::json{{"type", "integer"},
                          {"minimum", 0},
                          {"description", description},
                          {"default", fallback}};
}

nlohmann::json IntrinsicTool::string_list_property(std::string_view description)
{
    return nlohmann::json{{"type", "array"},
                          {"items", nlohmann::json{{"type", "string"}}},
                          {"description", description}};
}

nlohmann::json IntrinsicTool::enum_property(std::string_view description,
                                            std::vector<std::string> values,
                                            std::string_view fallback)
{
    return nlohmann::json{{"type", "string"},
                          {"enum", std::move(values)},
                          {"description", description},
                          {"default", fallback}};
}

nlohmann::json IntrinsicTool::object_schema(nlohmann::json properties,
                                            std::vector<std::string> required)
{
    // "required" is always written, even empty: a schema that omits it reads
    // as "unspecified" to some consumers, and "nothing is required" is a
    // statement worth making explicitly.
    return nlohmann::json{
        {"type", "object"},
        {"properties", std::move(properties)},
        {"required", required.empty() ? nlohmann::json::array()
                                     : nlohmann::json(std::move(required))},
    };
}

} // namespace tools::intrinsic
