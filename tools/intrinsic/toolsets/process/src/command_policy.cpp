#include "tools/intrinsic/process/command_policy.hpp"

// Keep the non-recursive matcher and its bounded work private to this TU. Boost
// Regex is header-only in the supported toolchain; no new shared library ships.
#define BOOST_REGEX_MAX_STATE_COUNT 10000
#define BOOST_REGEX_MAX_BLOCKS 16
#include <boost/regex.hpp>

#include <locale>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace tools::intrinsic {
namespace {

constexpr std::size_t kMaxCommandBytes = 4096;
constexpr std::size_t kMaxSegments = 16;
constexpr std::size_t kMaxRules = 32;
constexpr std::size_t kMaxPatternBytes = 1024;

/// Trim horizontal whitespace only. Newlines are shell syntax, not whitespace
/// that may be discarded to make an otherwise unsafe command match a rule.
std::string_view trim(std::string_view value)
{
    const auto first = value.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    return value.substr(first, value.find_last_not_of(" \t") - first + 1);
}

/// Partition a deliberately small shell subset without removing quotes. Reject
/// expansion even inside quotes (including harmless single-quoted dollars): a
/// false negative asks for approval; a false positive would bypass it. Operators
/// inside quotes remain literal. No partial classification survives a bad suffix.
std::vector<std::string_view> segments(std::string_view command)
{
    std::vector<std::string_view> result;
    char quote = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i < command.size(); ++i) {
        const unsigned char c = command[i];
        if ((c < 32 && c != '\t') || c == 127 || c == '$' || c == '`' || c == '\\') {
            return {};
        }
        if (quote != 0) {
            if (c == quote) {
                quote = 0;
            }
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            continue;
        }
        // Globs can expand into options; braces/tilde and comments are also
        // outside this subset. Assignments are excluded even after command names.
        if (std::string_view(";<>()[]{}*?~#!=").find(c) != std::string_view::npos) {
            return {};
        }
        if (c != '|' && c != '&') {
            continue;
        }
        const auto part = trim(command.substr(start, i - start));
        if (part.empty()) {
            return {};
        }
        result.push_back(part);
        if (result.size() >= kMaxSegments) {
            return {};
        }
        if (c == '&') {
            if (i + 1 == command.size() || command[i + 1] != '&') {
                return {};
            }
            ++i;
        } else if (i + 1 < command.size() && command[i + 1] == '|') {
            ++i;
        }
        start = i + 1;
    }
    const auto last = trim(command.substr(start));
    if (quote != 0 || last.empty()) {
        return {};
    }
    result.push_back(last);
    return result;
}

/// Compile POSIX extended regexes using the classic locale. Full matching is
/// imposed by classify(), independent of whether the operator supplied anchors.
std::vector<boost::regex> rules(const nlohmann::json& config, const char* key)
{
    const auto found = config.find(key);
    if (found == config.end()) {
        return {};
    }
    if (!found->is_array() || found->size() > kMaxRules) {
        throw std::invalid_argument(std::string(key) + " must be an array of at most 32 regexes");
    }
    std::vector<boost::regex> result;
    for (std::size_t i = 0; i < found->size(); ++i) {
        const auto& value = (*found)[i];
        const auto field = std::string(key) + "/" + std::to_string(i);
        if (!value.is_string()) {
            throw std::invalid_argument(field + " must be a regex string");
        }
        const auto& pattern = value.get_ref<const std::string&>();
        if (pattern.empty() || pattern.size() > kMaxPatternBytes || pattern.find('\0') != std::string::npos) {
            throw std::invalid_argument(field + " must contain 1..1024 bytes without NUL");
        }
        try {
            boost::regex expression;
            expression.imbue(std::locale::classic());
            expression.assign(pattern, boost::regex::extended | boost::regex::nosubs);
            result.push_back(std::move(expression));
        } catch (const boost::regex_error&) {
            throw std::invalid_argument(field + " is not a valid POSIX extended regex");
        }
    }
    return result;
}

bool matches(const std::vector<boost::regex>& expressions, std::string_view command)
{
    for (const auto& expression : expressions) {
        if (boost::regex_match(command.begin(), command.end(), expression)) {
            return true;
        }
    }
    return false;
}

} // namespace

struct CommandPolicy::Impl {
    enum class Mode { Strict, Readonly, Permissive };
    Mode mode = Mode::Strict;
    std::vector<boost::regex> readonly;
    std::vector<boost::regex> trusted;
};

CommandPolicy::CommandPolicy(const nlohmann::json& config)
{
    if (!config.is_object()) {
        throw std::invalid_argument("must be a mapping");
    }
    for (const auto& [key, value] : config.items()) {
        if (key != "mode" && key != "readonly_rules" && key != "trusted_rules") {
            throw std::invalid_argument("unknown bypass configuration field");
        }
    }
    auto policy = std::make_shared<Impl>();
    if (const auto mode = config.find("mode"); mode != config.end()) {
        if (*mode == "readonly") {
            policy->mode = Impl::Mode::Readonly;
        } else if (*mode == "permissive") {
            policy->mode = Impl::Mode::Permissive;
        } else if (*mode != "strict") {
            throw std::invalid_argument("mode must be strict, readonly or permissive");
        }
    }
    // Validate dormant rules too so switching mode cannot reveal a latent typo.
    policy->readonly = rules(config, "readonly_rules");
    policy->trusted = rules(config, "trusted_rules");
    impl_ = std::move(policy);
}

CommandPolicy::Decision CommandPolicy::classify(std::string_view command) const noexcept
{
    if (impl_->mode == Impl::Mode::Strict || command.empty() || command.size() > kMaxCommandBytes) {
        return Decision::Confirm;
    }
    try {
        const auto parts = segments(command);
        if (parts.empty()) {
            return Decision::Confirm;
        }
        auto decision = Decision::Readonly;
        for (const auto part : parts) {
            if (matches(impl_->readonly, part)) {
                continue;
            }
            if (impl_->mode != Impl::Mode::Permissive || !matches(impl_->trusted, part)) {
                return Decision::Confirm;
            }
            decision = Decision::TrustedWrite;
        }
        return decision;
    } catch (...) {
        // Complexity/stack limits, allocation failures and all matcher errors
        // fail closed for the WHOLE command, never just the offending segment.
        return Decision::Confirm;
    }
}

} // namespace tools::intrinsic
