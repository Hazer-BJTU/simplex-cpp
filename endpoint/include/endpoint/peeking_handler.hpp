#pragma once

//
// peeking_handler.hpp — read-only monitors over an SSE handler's products
// ======================================================================
//
// PeekingHandler wraps any SSEResponseHandler subclass and lets callers
// observe every decoded Product — at the decode point, before it enters the
// channel — without consuming or altering it. The canonical use is live
// monitoring of a model stream, e.g. printing the thinking process while the
// agent loop keeps draining the deltas into its own state:
//
//     PeekingHandler<ResponsesStreamHandler> handler(executor);
//     handler.add_peeker([](const ResponsesDelta& d) {
//         if (d.kind == DeltaKind::ReasoningText)
//             std::cerr << d.text << std::flush;   // watch it think
//     });
//     handler.add_peeker(metrics_recorder);        // a second monitor
//
// Type erasure, not templates: the peek callback is a std::function over the
// handler's product_type, so any callable (lambda, functor, function pointer)
// attaches to any handler at runtime — many peekers per handler, one peeker
// reused across handlers — without a concrete type per (handler, peeker)
// combination. The cost is one allocation at add_peeker() plus one indirect
// call per event, negligible next to network I/O.
//
// Semantics / contract
// --------------------
//  * The hook runs inside _handle_message, on the producer thread, after the
//    wrapped handler's own decode/accumulation has run and before the product
//    is queued. Peekers see each product exactly once, in arrival order,
//    regardless of how fast the consumer drains get().
//  * Read-only: each peek receives `const Product&`; its return value (if
//    any) is ignored and the product continues into the channel unchanged.
//  * A peek must not throw — an escaping exception propagates through
//    put()'s catch(...), drives the handler to ERROR and tears down both
//    sides (see SSEResponseHandler::put). Monitor code should swallow its
//    own errors.
//  * A peek must not block (it stalls the producer and thus the network
//    reads) and must not call back into the handler: producer-side state
//    (accumulators, line buffer) is mid-mutation.
//  * add_peeker() must run before the stream starts, or externally
//    serialized with put() — _peeks is not synchronized. Peekers are invoked
//    in registration order.
//  * The wrapper IS-A Handler (public inheritance): everything the wrapped
//    subclass exposes — e.g. ResponsesStreamHandler::response() / status() —
//    remains callable on the wrapper, subject to that subclass's own
//    concurrency discipline.
//

#include <functional>
#include <span>
#include <utility>
#include <vector>

#include "endpoint/request.hpp"

namespace endpoint {

/**
 * @brief An SSEResponseHandler subclass with read-only product monitors.
 *
 * @tparam Handler  A concrete SSEResponseHandler<Product> subclass. The
 *                  wrapper forwards construction arguments to it verbatim
 *                  and overrides only the decode hook.
 */
template<typename Handler>
class PeekingHandler final : public Handler {
public:
    /// The decoded event type the wrapped handler produces.
    using Product = typename Handler::product_type;
    /// Type-erased monitor: receives each product by const reference.
    using Peek = std::function<void(const Product&)>;

    template<typename... Args>
    explicit PeekingHandler(Args&&... args)
        : Handler(std::forward<Args>(args)...) {}

    /**
     * @brief Register a read-only monitor; callable more than once.
     *
     * Peekers run in registration order inside the decode hook. Register
     * before streaming starts (or externally serialized with put()).
     */
    void add_peeker(Peek peek) { _peeks.push_back(std::move(peek)); }

protected:
    Product _handle_message(
        std::span<const typename Handler::LineInfo> message) override {
        Product product = Handler::_handle_message(message);
        for (const auto& peek : _peeks) {
            peek(product);   // read-only: result ignored, product forwarded
        }
        return product;
    }

private:
    std::vector<Peek> _peeks;
};

} // namespace endpoint
