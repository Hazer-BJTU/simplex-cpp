/** Deterministic, opt-in backend comparison. Run each case in a separate process
 * so peak RSS is attributable to one backend/fixture. No network/model needed. */
#include "persistence/selection.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/resource.h>
#include <unistd.h>

using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;
namespace fs = std::filesystem;

/** Linux physical write counter; zero on platforms without procfs. */
std::uint64_t written_bytes() {
    std::ifstream input("/proc/self/io");
    std::string key;
    std::uint64_t value;
    while (input >> key >> value) if (key == "write_bytes:") return value;
    return 0;
}

/** Completed-call latency including durability, in milliseconds. */
template<class Function>
double elapsed(Function function) {
    const auto start = Clock::now();
    function();
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

Json percentiles(std::vector<double> samples) {
    std::sort(samples.begin(), samples.end());
    return {{"p50_ms", samples[samples.size() / 2]},
        {"p95_ms", samples[(samples.size() * 95 - 1) / 100]}};
}

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: benchmark_persistence json|sqlite messages repetitions\n";
        return 2;
    }
    const std::string backend = argv[1];
    const auto messages = std::stoul(argv[2]);
    const auto repetitions = std::stoul(argv[3]);
    if ((backend != "json" && backend != "sqlite") || messages < 2 || repetitions < 2) return 2;
    const auto root = fs::temp_directory_path() / ("simplex_benchmark_" + std::to_string(getpid()));
    fs::remove_all(root);
    struct Cleanup { fs::path root; ~Cleanup() { fs::remove_all(root); } } cleanup{root};
    model_io::AgentInputState original;
    original.meta.session_id = "benchmark";
    original.system_prompt.add_section("system", "System", "Deterministic benchmark.");
    for (std::size_t i = 0; i < messages / 2; ++i) {
        model_io::UserLoopStep turn;
        turn.user_input.role = "user";
        turn.user_input.type = model_io::MessageItemType::UserInput;
        model_io::Content content;
        content.raw = std::string(128, 'u') + std::to_string(i);
        turn.user_input.content.push_back(content);
        model_io::AgentLoopStep step;
        step.model_response.role = "assistant";
        step.model_response.type = model_io::MessageItemType::ModelResponse;
        content.raw = std::string(256, 'a');
        step.model_response.content.push_back(content);
        turn.agent_loop_step.push_back(step);
        original.turns.push_back(turn);
    }
    // A complete 1 MiB pending tool result exercises the same lossless graph.
    original.loop.emplace();
    model_io::InvokeReturn tool;
    tool.query.id = "large-result";
    tool.query.name = "fixture";
    tool.output.raw = std::string(1024 * 1024, 'r');
    original.loop->pending_results.push_back(tool);
    auto store = backend == "sqlite"
        ? persistence::make_sqlite_store("benchmark", root / "state")
        : persistence::make_json_store("benchmark", {root / "state", root / "memory"});
    auto summary = original;
    summary.turns.clear();
    summary.loop.reset();
    std::vector<double> save, restore, compact, reader;
    const auto writes_before = written_bytes();
    for (std::size_t i = 0; i < repetitions; ++i) {
        save.push_back(elapsed([&] { store->checkpoint(original, persistence::Boundary::StepFinished); }));
        std::optional<persistence::StoredState> restored;
        restore.push_back(elapsed([&] { restored = store->load_current(); }));
        if (!restored || Json(restored->state) != Json(original)) {
            throw std::runtime_error("round-trip mismatch");
        }
        reader.push_back(elapsed([&] {
            if (backend == "sqlite") (void)persistence::inspect_sqlite_current(root / "state");
            else (void)store->load_current();
        }));
        compact.push_back(elapsed([&] {
            const auto ref = store->reserve_archive({"11111111-1111-4111-8111-000000000001", true, "compact"});
            const auto prepared = store->begin_archive(ref, original);
            store->commit_compaction(ref, summary, prepared.revision);
            persistence::ArchivePruneRequest request;
            request.current = ref;
            request.policy.max_archives = 5;
            store->prune_archives(request);
        }));
    }
    std::uintmax_t bytes = 0;
    for (const auto& entry : fs::recursive_directory_iterator(root))
        if (entry.is_regular_file()) bytes += entry.file_size();
    const auto file_bytes = [&](const char* name) -> std::uintmax_t {
        const auto file = root / "state" / name;
        return fs::exists(file) ? fs::file_size(file) : 0;
    };
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    std::cout << Json{{"backend", backend}, {"messages", messages}, {"repetitions", repetitions},
        {"save", percentiles(save)}, {"restore", percentiles(restore)},
        {"compact", percentiles(compact)}, {"reader", percentiles(reader)},
        {"peak_rss_kib", usage.ru_maxrss}, {"storage_bytes", bytes},
        {"database_bytes", file_bytes("state.sqlite3")}, {"wal_bytes", file_bytes("state.sqlite3-wal")},
        {"physical_write_bytes", written_bytes() - writes_before}}.dump() << '\n';
}
