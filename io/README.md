# IO client

`io::Client` adds JSON routing to the reusable, text-only
`intercom::StableWebSocketClient`. It owns one WebSocket connection at a time,
one incoming payload queue, and one incoming signal queue. The queues live for
the entire client run, including reconnections.

An incoming WebSocket text message must be a JSON object with `type` and
`data` fields:

```json
{"type":"payload","data":{"text":"hello"}}
{"type":"signal","data":{"name":"cancel","request_id":"r1"}}
```

Other fields are allowed. The client routes `data` unchanged. A malformed
JSON document, a missing field, or an unknown `type` ends `run()` with a
protocol error. Outbound JSON can be sent with `co_await client.send(value)`;
the base intercom client still accepts plain text for other applications.

## Payloads

Call `subscribe_payload()` once, then repeatedly `co_await subscription.next()`
to remove requests in arrival order. The subscription is exclusive: the queue
distributes work to one consumer and does not broadcast copies. Only one
`next()` call may be outstanding on that subscription; a second concurrent
call fails. This package
does not invoke AgentLoop; the consumer decides when and how to do that.

Both incoming queues have configurable positive capacities. Routing uses a
non-blocking channel send so a full payload queue never holds up the WebSocket
reader or later signals. A rejected payload is logged and counted by
`rejected_payloads()`; there is no implicit replay or server acknowledgment.
Applications needing guaranteed admission must add a protocol-level request
ID and acknowledgment. A full signal queue is fatal because silently dropping
a cancellation signal would be unsafe.

## Signals

The default signal handler synchronously publishes `io::SignalEvent` on the
`eventbus::EventBus&` passed to the constructor. Subscribe on that same bus to
handle server signals. `register_signal_handler()` replaces the default
publisher with another synchronous `void(const nlohmann::json&)` function.
The signal channel has a dedicated coroutine and thread; slow synchronous bus
listeners cannot occupy the WebSocket reader's thread. Signal handlers still
run serially and should finish. A throwing handler ends the client run after
the transport and worker are joined.

Call `stop()` or request the run's stop token, then await `run()` before
destroying the client, its EventBus, or its executor. Stopping closes both
incoming queues. A message already removed by a consumer is that consumer's
responsibility; queued requests are not persisted by this package.
