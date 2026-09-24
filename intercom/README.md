# Intercom WebSocket client

`StableWebSocketClient` maintains one WebSocket session to a fixed
`endpoint::ResolvedEndpoint`. It uses the existing `connect_websocket` path for
plain `ws://` and verified `wss://` connections. A supervisor reconnects after
transport failures with capped exponential backoff. Each connected session has
one reader and one writer coroutine; both finish before the next session starts.

Derive from the client and implement `on_text(std::string)`. The callback runs
on the client's strand for each complete text message. It should finish
quickly; throwing ends `run()` after the session has been cleaned up.

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
pending operations. Call `stop()` from any thread, then await `run()` before
destroying the client or its TLS context. A `std::stop_token` can also be
passed to `run()`.

The default queue capacity and retry delays are configured through
`StableWebSocketOptions`. The inactivity timeout defaults to zero, leaving a
healthy but silent session open indefinitely. Setting a positive timeout
enables Beast's automatic idle ping; the session is closed and reconnected if
the peer does not answer. TCP connect, TLS handshake and WebSocket upgrade
retain their separate 30-second deadlines; the shared DNS resolver has no
independent hard deadline. The client is single use: create another instance
for a new lifetime after stopping.
