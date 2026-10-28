#include "tools/intrinsic/hub_remote_call/tools.hpp"
#include "tools/intrinsic/hub_remote_call/toolset.hpp"
#include "protocol/wire.hpp"
#include "tools/invoke_exception.hpp"
#include "tools/intrinsic/hub_remote_call/schemas.hpp"
#include "tools/intrinsic/hub_remote_call/subagents.hpp"

#include <boost/asio/this_coro.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <charconv>
#include <stdexcept>
#include <limits>

namespace tools::intrinsic {
namespace {
using Json = nlohmann::json;

/** Validate once at construction; no credentials appear in diagnostics. */
void validate_connection(
    const endpoint::ResolvedEndpoint& endpoint,
    std::chrono::milliseconds timeout)
{
    unsigned port = 0;
    const auto parsed = std::from_chars(
        endpoint.port.data(), endpoint.port.data() + endpoint.port.size(), port);
    if (endpoint.host.empty() || endpoint.host.find_first_of(" \t\r\n/?#@") != std::string::npos
        || endpoint.host.find('\0') != std::string::npos || endpoint.target.empty() || endpoint.target.front() != '/'
        || endpoint.target.find_first_of(" \t\r\n#") != std::string::npos
        || endpoint.target.find('\0') != std::string::npos
        || parsed.ec != std::errc{} || parsed.ptr != endpoint.port.data() + endpoint.port.size()
        || port == 0 || port > 65535 || timeout.count() <= 0
        || timeout.count() > std::numeric_limits<int>::max()) {
        throw std::invalid_argument("hub_remote_call requires a valid endpoint and positive timeout");
    }
}

bool nonblank(const std::string& value)
{
    return value.find_first_not_of(" \t\r\n\f\v") != std::string::npos;
}

/** Match the hub's lowercase slash-separated literal route grammar. */
bool valid_route(const std::string& route)
{
    bool first = true;
    for (char character : route) {
        const bool letter = character >= 'a' && character <= 'z';
        if (first) {
            if (!letter) return false;
            first = false;
        } else if (character == '/') {
            first = true;
        } else if (!letter && !(character >= '0' && character <= '9')
            && character != '_' && character != '-') {
            return false;
        }
    }
    return !first;
}

} // namespace

HubRemoteCallToolSet::HubRemoteCallToolSet(
    endpoint::ResolvedEndpoint endpoint,
    std::chrono::milliseconds timeout,
    HubRemoteCallIdentityProvider identity)
    : endpoint_(std::move(endpoint)), timeout_(timeout)
{
    validate_connection(endpoint_, timeout_);
    register_tools({
        std::make_shared<PlanTool>(endpoint_, timeout_, identity),
        std::make_shared<SubagentForkTool>(endpoint_, timeout_, identity),
        std::make_shared<SubagentSendTool>(endpoint_, timeout_, identity),
        std::make_shared<SubagentReceiveTool>(endpoint_, timeout_, identity)
    });
    declare_capability_group("plan", {"plan"});
    declare_capability_group("subagents", {
        "subagent_fork", "subagent_send", "subagent_receive"
    });
    load_skill(hub_remote_call::schema_directory() / "skill.yaml");
}

std::string_view HubRemoteCallToolSet::name() const noexcept
{
    return "hub_remote_call";
}

const endpoint::ResolvedEndpoint& HubRemoteCallToolSet::endpoint() const noexcept
{
    return endpoint_;
}

std::chrono::milliseconds HubRemoteCallToolSet::timeout() const noexcept
{
    return timeout_;
}

HubRemoteCallToolBase::HubRemoteCallToolBase(
    const std::filesystem::path& declaration_file,
    endpoint::ResolvedEndpoint endpoint,
    std::chrono::milliseconds timeout,
    eventbus::AsyncEventBus* bus)
    : DeclaredTool(declaration_file, bus),
      endpoint_(std::move(endpoint)), timeout_(timeout)
{
    validate_connection(endpoint_, timeout_);
}

HubRemoteCallToolBase::~HubRemoteCallToolBase() = default;

boost::asio::awaitable<Json> HubRemoteCallToolBase::request(
    model_io::InvokeQuery query,
    std::string route,
    std::string worker_id,
    std::string session_id,
    std::string run_id) const
{
    if (!valid_route(route) || !nonblank(worker_id) || !nonblank(session_id)
        || !nonblank(run_id) || !query.arguments.is_object()) {
        throw InvokeException(InvokeException::Stage::Invoke,
            "invalid hub remote call route, identity, or arguments", query);
    }
    auto target = endpoint_;
    const auto separator = target.target.find('?');
    auto path = target.target.substr(0, separator);
    const auto suffix = separator == std::string::npos
        ? std::string{} : target.target.substr(separator);
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    if (path == "/") path.clear();
    target.target = path + "/" + route + suffix;
    const auto request_id = boost::uuids::to_string(boost::uuids::random_generator()());
    protocol::ToolRequestEnvelope envelope;
    envelope.data.worker_id = worker_id;
    envelope.data.session_id = session_id;
    envelope.data.run_id = run_id;
    envelope.data.request_id = request_id;
    envelope.data.arguments = query.arguments;
    const auto encoded_request = protocol::to_json(envelope).dump();
    if (encoded_request.size() > 1024 * 1024) {
        throw InvokeException(InvokeException::Stage::Invoke,
            "remote request exceeds the 1 MiB transport budget", query);
    }
    std::string wire;
    try {
        wire = co_await intercom::cancellable_exchange(
            co_await boost::asio::this_coro::executor,
            std::move(target), encoded_request, timeout_, {},
            endpoint::get_global_ssl_context(), 256 * 1024);
    } catch (const boost::system::system_error& error) {
        throw InvokeException(InvokeException::Stage::Invoke,
            "hub remote call transport failed", query, error.code());
    } catch (const intercom::WsException& error) {
        throw InvokeException(InvokeException::Stage::Invoke,
            "hub remote call transport failed", query, error.error_code());
    } catch (const std::exception&) {
        throw InvokeException(InvokeException::Stage::Invoke,
            "hub remote call transport failed", query);
    }
    try {
        const auto parsed = Json::parse(wire);
        const auto reply = protocol::from_json<protocol::ToolResponseEnvelope>(parsed);
        const auto& data = reply.data;
        // Route arguments and results stay opaque; only the shared envelope and
        // the success/rejection alternative are checked here.
        const bool succeeded = data.status == protocol::ToolStatus::succeeded
            && data.result && data.result->is_object() && !data.error;
        const bool rejected = data.status == protocol::ToolStatus::rejected
            && !data.result && data.error && !data.error->code.empty()
            && !data.error->message.empty();
        if (data.worker_id != worker_id || data.session_id != session_id
            || data.run_id != run_id || data.request_id != request_id
            || data.route != route || !(succeeded || rejected)) {
            throw std::invalid_argument("invalid response");
        }
        co_return parsed.at("data");
    } catch (const std::exception&) {
        throw InvokeException(InvokeException::Stage::Invoke,
            "invalid or mismatched hub remote call response", query);
    }
}

} // namespace tools::intrinsic
