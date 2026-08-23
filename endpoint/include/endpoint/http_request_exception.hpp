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
        Connect,
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
        std::string host = {},
        unsigned status = 0)
        : std::runtime_error(std::move(message)),
          stage_(stage),
          ec_(ec),
          method_(std::move(method)),
          target_(std::move(target)),
          host_(std::move(host)),
          status_(status)
    {}

    [[nodiscard]] Stage stage() const noexcept { return stage_; }
    [[nodiscard]] const boost::system::error_code& error_code() const noexcept { return ec_; }
    [[nodiscard]] const std::string& method() const noexcept { return method_; }
    [[nodiscard]] const std::string& target() const noexcept { return target_; }
    [[nodiscard]] const std::string& host() const noexcept { return host_; }
    /** HTTP status of the response for failures that carry one (a non-200 SSE
     *  rejection); 0 when the failure has no status. */
    [[nodiscard]] unsigned status() const noexcept { return status_; }

    /** The stage's phrase as rendered by to_string(): "while sending the
     *  request", … */
    [[nodiscard]] static constexpr std::string_view stage_phrase(Stage stage) noexcept
    {
        switch (stage) {
            case Stage::CreateRequest:  return "while building the request";
            case Stage::Connect:        return "while establishing the connection";
            case Stage::Write:          return "while sending the request";
            case Stage::Read:           return "while reading the response";
            case Stage::HandleResponse: return "while handling the response";
            case Stage::Unknown:        return "at an unknown stage";
        }
        return "at an unknown stage"; // unreachable; keeps -Wreturn-type quiet
    }

    /**
     * One-line, log-friendly rendering of the failure in prose rather than
     * key=value tags: the stage phrase and the message lead, and whichever
     * context fields are set gather into one parenthetical, e.g.
     *   Failed while handling the response: SSE request rejected \
     * (HTTP status 401; POST /chat/completions to api.deepseek.com)
     * Absent fields (no error code, no status, no request context) are omitted.
     */
    [[nodiscard]] std::string to_string() const
    {
        std::string rendered = "Failed ";
        rendered += stage_phrase(stage_);
        rendered += ": ";
        rendered += what();

        // Context pieces join into a single parenthetical, "; "-separated.
        bool opened = false;
        auto append = [&](std::string_view piece) {
            rendered += opened ? "; " : " (";
            opened = true;
            rendered += piece;
        };
        if (ec_) append(ec_.message());
        if (status_ != 0) append("HTTP status " + std::to_string(status_));
        if (!method_.empty() || !target_.empty() || !host_.empty()) {
            std::string request_line = method_;
            if (!target_.empty()) {
                if (!request_line.empty()) request_line += ' ';
                request_line += target_;
            }
            if (!host_.empty()) {
                if (!request_line.empty()) request_line += ' ';
                request_line += "to " + host_;
            }
            append(request_line);
        }
        if (opened) rendered += ')';
        return rendered;
    }

private:
    Stage stage_;
    boost::system::error_code ec_;
    std::string method_;
    std::string target_;
    std::string host_;
    unsigned status_;
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
