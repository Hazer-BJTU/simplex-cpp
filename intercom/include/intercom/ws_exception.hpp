#pragma once

//
// ws_exception.hpp — the intercom module's own request-lifecycle exception
// ======================================================================
//
// intercom (internal WebSocket communication) carries its own exception
// type on purpose: it is NOT endpoint's HttpRequestException, even though
// the two look alike. The module is transport-only — it moves opaque byte
// strings (JSON in practice) between services over WebSocket, and its
// callers should not have to know that the connection underneath was once
// an HTTP upgrade or that the rest of the tree also happens to speak HTTP.
// An independent type keeps the module's failure surface decoupled from
// endpoint's and lets the security-review layer (the next phase) add its own
// verdicts without touching endpoint.
//
// The shape deliberately mirrors HttpRequestException so the mental model
// carries over: one Stage, an optional transport error_code, and the
// host/target context of the failing endpoint. what() is already the full
// one-line rendering (stage phrase + message + context), so a host that
// catches this type only as std::exception — the only dependable catch
// across a dlopen boundary — still sees the whole context.

#include <boost/system/error_code.hpp>

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace intercom {

/**
 * @brief A failure in one stage of a WebSocket request exchange.
 *
 * The module's lifecycle exception: every transport fault out of
 * connect_websocket / fetch_once surfaces as one of these, carrying the
 * failing stage, the transport error_code when one exists, and the
 * endpoint's host/target so the logs read like a request line.
 */
class WsException : public std::runtime_error {
public:
    /// Which phase of the exchange failed.
    enum class Stage {
        /// TCP/TLS connect + the WebSocket upgrade handshake (101).
        Connect,
        /// Sending the request message.
        Write,
        /// Reading the reply message.
        Read,
        /// An operation on an empty (unconnected) stream, or anything
        /// unclassifiable.
        Unknown
    };

    /**
     * @brief Construct the failure with its stage, message, and context.
     *
     * @param stage   The phase that failed.
     * @param message Human-readable description; rendered as-is.
     * @param ec      Transport error_code when the failure was code-reported,
     *                clear otherwise.
     * @param host    The endpoint host, for the rendered context.
     * @param target  The WebSocket request path, for the rendered context.
     */
    WsException(
        Stage stage,
        std::string message,
        boost::system::error_code ec = {},
        std::string host = {},
        std::string target = {})
        : std::runtime_error(render(stage, message, ec, host, target)),
          stage_(stage),
          ec_(ec),
          host_(std::move(host)),
          target_(std::move(target))
    {}

    [[nodiscard]] Stage stage() const noexcept { return stage_; }
    [[nodiscard]] const boost::system::error_code& error_code() const noexcept {
        return ec_;
    }
    [[nodiscard]] const std::string& host() const noexcept { return host_; }
    [[nodiscard]] const std::string& target() const noexcept { return target_; }

    /// The stage's prose phrase, as rendered by to_string()/what().
    [[nodiscard]] static constexpr std::string_view stage_phrase(
        Stage stage) noexcept
    {
        switch (stage) {
            case Stage::Connect: return "while establishing the connection";
            case Stage::Write:   return "while sending the message";
            case Stage::Read:    return "while reading the reply";
            case Stage::Unknown: return "at an unknown stage";
        }
        return "at an unknown stage";   // unreachable; quiets -Wreturn-type
    }

    /**
     * @brief The one-line, log-friendly rendering — identical to what().
     *
     * e.g. `Failed while reading the reply: websocket exchange failed: ...
     * (connection reset; /v1/ws to ws.internal)` — absent fields are omitted.
     */
    [[nodiscard]] std::string to_string() const { return what(); }

private:
    static std::string render(
        Stage stage,
        const std::string& message,
        const boost::system::error_code& ec,
        const std::string& host,
        const std::string& target)
    {
        std::string rendered = "Failed ";
        rendered += stage_phrase(stage);
        rendered += ": ";
        rendered += message;

        bool opened = false;
        auto append = [&](std::string_view piece) {
            rendered += opened ? "; " : " (";
            opened = true;
            rendered += piece;
        };
        if (ec) append(ec.message());
        if (!host.empty() || !target.empty()) {
            std::string where;
            if (!target.empty()) where += target;
            if (!host.empty()) {
                if (!where.empty()) where += ' ';
                where += "to " + host;
            }
            append(where);
        }
        if (opened) rendered += ')';
        return rendered;
    }

    Stage stage_;
    boost::system::error_code ec_;
    std::string host_;
    std::string target_;
};

/**
 * @brief The read-timeout flavour of WsException: the reply did not arrive
 *        within the caller-configured read deadline.
 *
 * The stage is always Read and error_code() carries the transport's timeout
 * code, so a slow backend is distinguishable from a dead connection — catch
 * it specifically, or as WsException like every other failure.
 */
class WsTimeoutException : public WsException {
public:
    WsTimeoutException(
        std::string message,
        boost::system::error_code ec = {},
        std::string host = {},
        std::string target = {})
        : WsException(
              Stage::Read,
              std::move(message),
              std::move(ec),
              std::move(host),
              std::move(target))
    {}
};

} // namespace intercom
