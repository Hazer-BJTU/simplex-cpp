#pragma once

#include <boost/system/error_code.hpp>

#include <stdexcept>
#include <string>
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

private:
    Stage stage_;
    boost::system::error_code ec_;
    std::string method_;
    std::string target_;
    std::string host_;
};
