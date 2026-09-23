#include "loop/intrinsic/context_statistic/hook.hpp"

#include <cstdlib>
#include <filesystem>

int main() {
    if (std::getenv("SIMPLEX_LOOP_HOOK_SCHEMA_DIR") != nullptr) {
        return 2;
    }
    if (std::filesystem::exists(
            "/nonexistent/simplex-loop-hook-schemas/context_statistic")) {
        return 3;
    }
    const auto executable = std::filesystem::read_symlink("/proc/self/exe");
    const auto installed = executable.parent_path() / "schemas" / "loop"
        / "context_statistic" / "config.yaml";
    if (loop::intrinsic::hook_config_file("context_statistic") != installed) {
        return 4;
    }
    const auto hook = loop::intrinsic::ContextStatisticHook::from_config();
    return hook->config().config.at("context_window_tokens") == 777 ? 0 : 1;
}
