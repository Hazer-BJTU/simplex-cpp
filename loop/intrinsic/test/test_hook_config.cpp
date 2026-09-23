#define BOOST_TEST_MODULE IntrinsicLoopHookConfig

#include "loop/events.hpp"
#include "loop/hook_registry.hpp"
#include "loop/intrinsic/hook_config.hpp"

#include <boost/test/unit_test.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <unistd.h>

namespace fs = std::filesystem;

namespace {

/** Isolated YAML and environment fixture for loader and registry checks. */
class Scratch {
public:
    Scratch()
        : root_(fs::temp_directory_path()
                / ("simplex_loop_hook_config_" + std::to_string(::getpid()))) {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
        fs::create_directories(root_);

        if (const char* value = std::getenv("SIMPLEX_LOOP_HOOK_SCHEMA_DIR")) {
            old_env_ = value;
        }
    }

    ~Scratch() {
        if (old_env_) {
            ::setenv("SIMPLEX_LOOP_HOOK_SCHEMA_DIR", old_env_->c_str(), 1);
        } else {
            ::unsetenv("SIMPLEX_LOOP_HOOK_SCHEMA_DIR");
        }
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;

    fs::path write(std::string_view hook, std::string_view yaml) const {
        const fs::path directory = root_ / hook;
        fs::create_directories(directory);
        const fs::path file = directory / "config.yaml";
        std::ofstream output(file, std::ios::binary | std::ios::trunc);
        output << yaml;
        BOOST_REQUIRE(output.good());
        return file;
    }

    void use_override() const {
        BOOST_REQUIRE(::setenv("SIMPLEX_LOOP_HOOK_SCHEMA_DIR",
                               root_.c_str(), 1) == 0);
    }

    const fs::path& root() const noexcept {
        return root_;
    }

private:
    fs::path root_;
    std::optional<std::string> old_env_;
};

/** Configured hook that rejects every option except its positive limit. */
class LimitedHook final : public loop::intrinsic::IntrinsicLoopHook {
public:
    LimitedHook(loop::intrinsic::HookConfig config, int& calls)
        : IntrinsicLoopHook(std::move(config)), calls_(calls) {
        const auto& options = this->config().config;
        if (options.size() != 1 || !options.contains("limit")
            || !options.at("limit").is_number_integer()
            || options.at("limit").get<int>() <= 0) {
            throw std::invalid_argument("limit must be a positive integer");
        }
        limit_ = options.at("limit").get<int>();
    }

protected:
    Subscriptions subscribe(eventbus::EventBus& bus) override {
        Subscriptions subscriptions;
        subscriptions.emplace_back(bus.subscribe<loop::BeforeInput>(
            [this](const loop::BeforeInput&) {
                if (calls_ < limit_) {
                    ++calls_;
                }
            }));
        return subscriptions;
    }

private:
    int& calls_;
    int limit_ = 0;
};

constexpr std::string_view kOneCall = R"(
name: limiter
description: Limit how many inputs this test hook counts.
config:
  limit: 1
)";

void publish_input(eventbus::EventBus& bus) {
    model_io::MessageItem input;
    bus.publish(loop::BeforeInput{input});
}

bool failure_mentions(const fs::path& file, std::string_view part) {
    try {
        (void)loop::intrinsic::load_hook_config(file, "limiter");
    } catch (const loop::intrinsic::HookConfigError& failure) {
        const std::string message = failure.what();
        return message.find(file.string()) != std::string::npos
            && message.find(part) != std::string::npos;
    }
    return false;
}

} // namespace

BOOST_AUTO_TEST_CASE(runtime_yaml_configures_hook_before_subscription) {
    Scratch scratch;
    scratch.use_override();
    scratch.write("limiter", kOneCall);
    const fs::path file = loop::intrinsic::hook_config_file("limiter");
    BOOST_TEST(file == scratch.root() / "limiter" / "config.yaml");

    eventbus::EventBus bus;
    loop::LoopHookRegistry registry(bus);
    int calls = 0;
    auto first = std::make_shared<LimitedHook>(
        loop::intrinsic::load_hook_config(file, "limiter"), calls);
    registry.add(first);
    publish_input(bus);
    publish_input(bus);
    BOOST_TEST(calls == 1);

    scratch.write("limiter", R"(
name: limiter
description: A changed limit takes effect for the next instance.
config:
  limit: 3
)");
    int new_calls = 0;
    auto next = std::make_shared<LimitedHook>(
        loop::intrinsic::load_hook_config(file, "limiter"), new_calls);
    BOOST_TEST(registry.set(next));
    publish_input(bus);
    publish_input(bus);
    publish_input(bus);
    BOOST_TEST(new_calls == 3);
    BOOST_TEST(bus.subscriber_count<loop::BeforeInput>() == 1U);
}

BOOST_AUTO_TEST_CASE(malformed_or_invalid_config_never_subscribes) {
    Scratch scratch;
    scratch.use_override();
    const fs::path file = scratch.write("limiter", R"(
name: limiter
description: Invalid runtime option.
config:
  limit: 0
)");
    eventbus::EventBus bus;
    loop::LoopHookRegistry registry(bus);
    int calls = 0;

    BOOST_CHECK_THROW(std::make_shared<LimitedHook>(
        loop::intrinsic::load_hook_config(file, "limiter"), calls),
        std::invalid_argument);
    auto skipped = loop::intrinsic::try_create_hook(
        file, "limiter", [&](loop::intrinsic::HookConfig config) {
            return std::make_shared<LimitedHook>(std::move(config), calls);
        });
    BOOST_TEST(skipped == nullptr);
    BOOST_TEST(registry.empty());
    BOOST_TEST(bus.subscriber_count<loop::BeforeInput>() == 0U);

    scratch.write("limiter", R"(
name: limiter
description: Broken top-level field.
config: {}
confg: {}
)");
    BOOST_TEST(failure_mentions(file, "/confg"));
    BOOST_TEST(!loop::intrinsic::try_load_hook_config(file, "limiter"));
    BOOST_TEST(registry.empty());
}

BOOST_AUTO_TEST_CASE(declaration_rejects_bad_identity_and_shape) {
    Scratch scratch;
    const fs::path file = scratch.write("limiter", R"(
name: another
description: Wrong package identity.
config: {}
)");
    BOOST_TEST(failure_mentions(file, "/name"));

    scratch.write("limiter", R"(
name: limiter
description: Missing config.
)");
    BOOST_TEST(failure_mentions(file, "/config"));

    scratch.write("limiter", R"(
name: limiter
description: Wrong config type.
config: []
)");
    BOOST_TEST(failure_mentions(file, "/config"));

    scratch.write("limiter", "name: limiter\ndescription: [wrong]\nconfig: {}\n");
    BOOST_TEST(failure_mentions(file, "/description"));

    scratch.write("limiter", "name: limiter\ndescription: okay\nconfig: [\n");
    BOOST_TEST(failure_mentions(file, "limiter/config.yaml"));
}

BOOST_AUTO_TEST_CASE(path_override_is_explicit_and_names_cannot_escape) {
    Scratch scratch;
    scratch.use_override();
    const fs::path expected = scratch.root() / "limiter" / "config.yaml";
    BOOST_TEST(loop::intrinsic::hook_config_file("limiter") == expected);
    BOOST_TEST(!fs::exists(expected));
    BOOST_CHECK_THROW(([&] {
        const fs::path file = loop::intrinsic::hook_config_file("../other");
    }()), std::invalid_argument);
    BOOST_CHECK_THROW(([&] {
        const fs::path file = loop::intrinsic::hook_config_file("");
    }()), std::invalid_argument);
}
