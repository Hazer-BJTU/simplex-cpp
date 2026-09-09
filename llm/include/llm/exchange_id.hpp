#pragma once

/**
 * @file llm/exchange_id.hpp
 * @brief The correlation id one model exchange is known by, end to end.
 *
 * Concurrent converse() calls on ONE model instance are indistinguishable by
 * anything the caller can see from outside: same provider, same model name,
 * interleaved streaming events on one process-wide bus. This is the join key
 * that makes them separable — minted once per exchange, broadcast on every
 * event that exchange emits, and reported back on the assembled
 * model_io::MessageItem (under `extras.exchange_id`), so a subscriber can
 * bind a live stream to the result it eventually produced.
 *
 * ## Uniqueness across DSOs
 *
 * A plain process-wide counter would have to live in exactly one shared
 * library to be trustworthy — the treatment utils/eventbus needed for its
 * singleton bus. That is a heavy dependency for an id, and this header is
 * consumed by both protocol adapters AND provider plugins, so the counter
 * could plausibly exist in several copies (one per DSO that inlines it).
 *
 * Rather than legislate the link topology, the id is made collision-free
 * *by construction*: a per-copy random salt drawn once, plus a per-copy
 * atomic counter. Two copies of the counter cannot collide because their
 * salts differ; one copy cannot collide with itself because the counter is
 * atomic. The result is process-unique — in fact machine-unique in practice
 * — with no link-order requirement and no new dependency.
 *
 * The rendered form is `xchg-<16 hex salt>-<decimal sequence>`; treat it as
 * an opaque string, never parse it.
 */

#include <atomic>
#include <cstdint>
#include <random>
#include <string>

namespace llm {

/**
 * @brief Mint the correlation id for one exchange. Thread-safe.
 *
 * Call ONCE per converse() (not per attempt): a retried exchange keeps its
 * id, which is what lets a subscriber recognise a replay as the same
 * exchange rather than a new one. The per-attempt disambiguator is the
 * attempt counter carried alongside on each event.
 */
inline std::string next_exchange_id() {
    // Drawn once per copy of this function in the process. random_device is
    // used for the salt only — no sequencing depends on it, so quality just
    // has to be good enough to keep independent copies apart.
    static const std::uint64_t salt = [] {
        std::random_device device;
        return (static_cast<std::uint64_t>(device()) << 32) ^
               static_cast<std::uint64_t>(device());
    }();
    // relaxed: uniqueness needs atomicity of the read-modify-write, not any
    // ordering against other memory. Nothing is published through this value.
    static std::atomic<std::uint64_t> sequence{0};
    const std::uint64_t ordinal = sequence.fetch_add(1, std::memory_order_relaxed);

    static constexpr char kHex[] = "0123456789abcdef";
    std::string id = "xchg-";
    id.reserve(5 + 16 + 1 + 20);
    for (int shift = 60; shift >= 0; shift -= 4) {
        id.push_back(kHex[(salt >> shift) & 0xF]);
    }
    id.push_back('-');
    id += std::to_string(ordinal);
    return id;
}

} // namespace llm