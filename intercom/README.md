# Intercom WebSocket client

`StableWebSocketClient` maintains one WebSocket session to a fixed
`endpoint::ResolvedEndpoint`. It uses the existing `connect_websocket` path for
plain `ws://` and verified `wss://` connections. A supervisor reconnects after
transport failures with capped exponential backoff. It also retries every
connection-establishment failure indefinitely, including permanent DNS errors,
TLS certificate verification failures, and rejected WebSocket upgrades (even
authentication failures). This is intentional: endpoint configuration or the
peer may be repaired without restarting the process. Unlike the finite,
recoverability-based `intercom::fetch` policy, a bad stable-client configuration
can leave `run()` alive until stopped. Each connected session has one reader
and one writer coroutine; both finish before the next session starts.

Derive from the client and implement `on_text(std::string)`. The callback runs
on the client's strand for each complete text message. It should finish
quickly; throwing ends `run()` after the session has been cleaned up.
Receiving a binary WebSocket message ends `run()` with `WsProtocolException`;
it is not retried as a transient connection fault.

```cpp
class MyClient : public intercom::StableWebSocketClient {
public:
    using StableWebSocketClient::StableWebSocketClient;

protected:
    void on_text(std::string message) override {
        // Handle one complete message.
    }
};

// Keep client and executor alive until run() returns.
MyClient client(executor, resolved_endpoint);
auto running = boost::asio::co_spawn(executor, client.run(),
                                     boost::asio::use_future);
// Elsewhere: co_await client.send(text);
// At shutdown: client.stop(); then wait for running to complete.
```

`send()` admits a string to a bounded channel and suspends if the channel is
full. Admission does not confirm delivery. Queued messages survive reconnects;
a message already taken by the writer is never replayed after a failed write,
because the peer may have received it. Stopping closes the channel and ends
pending operations. Once `stop()` returns, a new `send()` fails immediately.
The same guarantee holds after `request_stop()` returns on the token passed to
an active `run()`. Call either shutdown method, then await `run()` before
destroying the client or its TLS context.

The default queue capacity and retry delays are configured through
`StableWebSocketOptions`. The inactivity timeout defaults to zero, leaving a
healthy but silent session open indefinitely. Setting a positive timeout
enables Beast's automatic idle ping; the session is closed and reconnected if
the peer does not answer. TCP connect, TLS handshake and WebSocket upgrade
retain their separate 30-second deadlines; the shared DNS resolver has no
independent hard deadline. The client is single use: create another instance
for a new lifetime after stopping.

## Cancellable one-shot exchange

Link intercom_exchange and include intercom/cancellable_exchange.hpp for
cancellable_exchange(executor, endpoint, text, timeout, stop_token). This sends
one text request and accepts one text reply without retries. A positive overall
deadline includes DNS, TCP/TLS, upgrade, write, read and graceful close.

The operation owns a strand, cancellation slot and deadline. Stop callbacks
only post to that strand; established sockets are explicitly aborted there.
Completion joins the child exchange and deadline, so callers may safely release
their dependencies afterward. Keep the TLS context alive through completion.
Inherited Asio cancellation is shielded; the explicit stop token controls
shutdown. Cancellation reports operation_aborted; expiry reports timed_out.
A binary reply throws WsProtocolException with endpoint context.

This API is used by the core confirmation adapter. It differs from fetch_once's
inactivity timeout and from fetch's optional retry behavior: a human approval
request must not be repeated implicitly.

The deadline is an authorization cutoff, not a hard wall-clock bound on return.
A system DNS backend already inside `getaddrinfo` may not be interruptible.
Expiration or cancellation still invalidates the reply, but completion (and
worker shutdown waiting for it) can be delayed until that backend returns.
The operation retains and joins this work; it never detaches resolution or
allows a late result to revive approval. Numeric endpoint addresses avoid DNS
lookup when bounded resolver latency is required.
