#pragma once

#include <boost/system/error_code.hpp>

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

/**
 * Describes a failure in one stage of an HTTP request and retains enough
 * request context for callers to log or classify the failure.
 */
class HttpRequestException : public std::runtime_error {
public:
    enum class Stage {
        CreateRequest,
        Write,
        Read,
        HandleResponse,
        Unknown
    };

    HttpRequestException(
        Stage stage,
        std::string message,
        boost::system::error_code ec = {},
        std::string method = {},
        std::string target = {},
        std::string host = {})
        : std::runtime_error(std::move(message)),
          stage_(stage),
          ec_(ec),
          method_(std::move(method)),
          target_(std::move(target)),
          host_(std::move(host))
    {}

    [[nodiscard]] Stage stage() const noexcept { return stage_; }
    [[nodiscard]] const boost::system::error_code& error_code() const noexcept { return ec_; }
    [[nodiscard]] const std::string& method() const noexcept { return method_; }
    [[nodiscard]] const std::string& target() const noexcept { return target_; }
    [[nodiscard]] const std::string& host() const noexcept { return host_; }

    /** The stage's short name as rendered by to_string(): "create", "write", … */
    [[nodiscard]] static constexpr std::string_view stage_name(Stage stage) noexcept
    {
        switch (stage) {
            case Stage::CreateRequest:  return "create";
            case Stage::Write:          return "write";
            case Stage::Read:           return "read";
            case Stage::HandleResponse: return "handle";
            case Stage::Unknown:        return "unknown";
        }
        return "unknown"; // unreachable; keeps -Wreturn-type quiet
    }

    /**
     * One-line, log-friendly rendering of the failure: the stage name and the
     * message, plus whichever context fields are set, e.g.
     *   stage=write what="request write failed" ec=Connection refused \
     * method=POST target=/v1/messages host=example.com
     * Absent fields (no error code, no request context) are omitted.
     */
    [[nodiscard]] std::string to_string() const
    {
        std::string rendered = "stage=";
        rendered += stage_name(stage_);
        rendered += " what=\"";
        rendered += what();
        rendered += '"';
        if (ec_) {
            rendered += " ec=";
            rendered += ec_.message();
        }
        if (!method_.empty()) rendered += " method=" + method_;
        if (!target_.empty()) rendered += " target=" + target_;
        if (!host_.empty())   rendered += " host=" + host_;
        return rendered;
    }

private:
    Stage stage_;
    boost::system::error_code ec_;
    std::string method_;
    std::string target_;
    std::string host_;
};

/**
 * The read-timeout flavour of HttpRequestException: http_request raises it
 * when a bounded response does not complete within the caller-configured read
 * deadline. The stage is always Read, and error_code() carries the transport's
 * timeout code. Catch it specifically to distinguish a slow backend from a
 * dead connection, or as HttpRequestException like every other failure.
 */
class HttpRequestTimeoutException : public HttpRequestException {
public:
    HttpRequestTimeoutException(
        std::string message,
        boost::system::error_code ec = {},
        std::string method = {},
        std::string target = {},
        std::string host = {})
        : HttpRequestException(
              Stage::Read,
              std::move(message),
              std::move(ec),
              std::move(method),
              std::move(target),
              std::move(host))
    {}
};
