# IO client

`io::Client` adds JSON routing to the reusable, text-only
`intercom::StableWebSocketClient`. It owns one WebSocket connection at a time,
with independent payload, signal, query and rejection-feedback queues. The queues live for
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

A payload whose `data.operation` is `history` or `answer` is a read-only query.
It bypasses the normal payload queue and is published as
`io::PayloadQueryEvent` on the executor supplied to the client. Queries have
an independent `query_capacity` quota (default 64). A query envelope above
16 KiB is rejected; this applies to protocol cursors, not conversation content.
The host should supply its state-owning strand and keep listeners short.

Payload overflow increments `rejected_payloads()`. Query overflow or excessive
query size increments `rejected_queries()` and offers a metadata-only
`PayloadQueryRejectedEvent` (`query_queue_full` or `query_too_large`). Payload
rejection offers `PayloadRejectedEvent`. The independent feedback mailbox also
has `query_capacity` slots, bounds malformed correlation metadata, and publishes
on the supplied executor. If it is full, `unreported_rejections()` increases;
there is no recursive rejection and no automatic resend. Listeners throwing
still fail `run()` after its supervised workers have been joined.

Signals retain a separate `signal_capacity` quota and their dedicated thread.
A full actual signal queue remains fatal; queries and rejection feedback never
consume its capacity. Queue admission and rejection feedback do not acknowledge
execution or delivery. Feedback may be omitted under congestion or shutdown;
hosts must expose counters and clients should time out read-only queries and
retry, rather than assume an unanswered request succeeded.

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
destroying the client, its EventBus, or its executor. Stopping closes all
incoming queues. A message already removed by a consumer is that consumer's
responsibility; queued requests are not persisted by this package.
