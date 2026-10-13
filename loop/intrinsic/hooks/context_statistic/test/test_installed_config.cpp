#include "loop/intrinsic/context_statistic/hook.hpp"

#include <cstdlib>
#include <filesystem>

int main(int, char* argv[]) {
    if (std::getenv("SIMPLEX_LOOP_HOOK_SCHEMA_DIR") != nullptr) {
        return 2;
    }
    if (std::filesystem::exists(
            "/nonexistent/simplex-loop-hook-schemas/context_statistic")) {
        return 3;
    }
    // The runner supplies this test's application path. Do not derive the
    // expected location from /proc/self/exe: it names musl under explicit loading.
    const auto executable = std::filesystem::canonical(argv[0]);
    const auto installed = executable.parent_path() / "schemas" / "loop"
        / "context_statistic" / "config.yaml";
    if (loop::intrinsic::hook_config_file("context_statistic") != installed) {
        return 4;
    }
    const auto hook = loop::intrinsic::ContextStatisticHook::from_config();
    return hook->config().config.at("context_window_tokens") == 777 ? 0 : 1;
}
