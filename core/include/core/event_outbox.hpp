#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <stdexcept>
#include <string>
#include <nlohmann/json.hpp>
#include "core/protocol.hpp"
#include "intercom/message_limits.hpp"

namespace core {
/** Strand-owned admission ledger. Transport admission is not peer delivery.
 *
 * Ordinary previews/query feedback share a bounded quota. Six latest-value
 * metadata slots are separate from eight lifecycle slots. Metadata replacement
 * retains the admitted sequence and never replaces the in-flight front item.
 * New runs must wait for the previous backlog to drain, bounding lifecycle
 * production. While a request waits, new noncritical output must be paused so
 * polling cannot indefinitely extend that finite backlog. Automatic compact
 * notifications are latest-value metadata; manual results remain lifecycle.
 */
class EventOutbox {
public:
    enum class Kind {
        Preview, Query, Feedback, Metadata, Lifecycle
    };
    enum class Admission {
        Added, Replaced, Omitted
    };
    static constexpr std::size_t lifecycle_slots = 8;
    static constexpr std::size_t metadata_slots = 6;
    static_assert(intercom::default_write_byte_capacity > (lifecycle_slots + 1) * display_event_max_bytes,
        "transport default must leave ordinary headroom after lifecycle and active-send reserves");
    static constexpr std::size_t soft_bytes = intercom::default_write_byte_capacity
        - (lifecycle_slots + 1) * display_event_max_bytes;

    explicit EventOutbox(std::size_t capacity) : capacity_(capacity) {
        if (!capacity) {
            throw std::invalid_argument("event_capacity must be positive");
        }
    }

    /** Omit a query before projecting state if output capacity is unavailable.
     * Call on the owning strand before expensive history/answer construction.
     * A skipped query is counted exactly once without allocating a reply or
     * spending sequence/count/byte reservations. Byte preflight conservatively
     * requires headroom for the maximum history frame. Passing it does
     * not reserve capacity: admit() still checks the final encoded reply.
     */
    bool omit_query_when_congested(std::size_t transport_bytes = 0) {
        if (closed_) {
            return true;
        }
        if (noncritical_paused_ || ordinary_count_ >= capacity_
            || !fits(bytes_, transport_bytes, history_event_max_bytes, soft_bytes)) {
            note_omission(Kind::Query);
            return true;
        }
        return false;
    }

    /** Commit sequence, bytes and omission settlement only on successful admission.
     * Failed or closed admission leaves the ledger intact. The caller supplies
     * transport reservations, including its active write, for the shared bound.
     */
    Admission admit(nlohmann::json message, Kind kind, std::size_t transport_bytes = 0) {
        const bool settlement = message.at("event") == "run_finished"
            || (message.at("event") == "error" && message["data"].value("durable", true) == false);
        if (settlement) {
            if (omitted_display_) {
                message["data"]["omitted_display_events"] = omitted_display_;
            }
            if (omitted_queries_) {
                message["data"]["omitted_query_events"] = omitted_queries_;
            }
            if (coalesced_metadata_) {
                message["data"]["coalesced_metadata_events"] = coalesced_metadata_;
            }
            if (omitted_feedback_) {
                message["data"]["omitted_feedback_events"] = omitted_feedback_;
            }
        }
        message["sequence"] = sequence_ + 1;
        auto bytes = message.dump().size();
        if (bytes > display_event_max_bytes) {
            throw std::length_error("display event exceeds the transport budget");
        }
        if (message.at("event") == "history" && bytes > history_event_max_bytes) {
            throw std::length_error("history event exceeds the display budget");
        }
        if (closed_) {
            return Admission::Omitted;
        }
        if (noncritical_paused_ && kind != Kind::Lifecycle) {
            note_omission(kind);
            return Admission::Omitted;
        }
        if (kind == Kind::Metadata) {
            for (std::size_t i = in_flight_ ? 1 : 0; i < queue_.size(); ++i) {
                auto& old = queue_[i];
                if (old.kind != kind || old.message.at("event") != message.at("event")
                    || old.message.at("request_id") != message.at("request_id")
                    || old.message.at("run_id") != message.at("run_id")) {
                    continue;
                }
                message["sequence"] = old.message.at("sequence");
                bytes = message.dump().size();
                if (fits(bytes_ - old.bytes, transport_bytes, bytes, soft_bytes)) {
                    bytes_ = bytes_ - old.bytes + bytes;
                    old = {std::move(message), bytes, kind};
                    ++coalesced_metadata_;
                    return Admission::Replaced;
                }
                break;
            }
        }
        const auto limit = kind == Kind::Lifecycle ? intercom::default_write_byte_capacity : soft_bytes;
        const bool slot = kind == Kind::Lifecycle ? lifecycle_count_ < lifecycle_slots
            : kind == Kind::Metadata ? metadata_count_ < metadata_slots : ordinary_count_ < capacity_;
        if (!slot || !fits(bytes_, transport_bytes, bytes, limit)) {
            if (kind == Kind::Lifecycle) {
                return Admission::Omitted;
            }
            note_omission(kind);
            return Admission::Omitted;
        }
        queue_.push_back({std::move(message), bytes, kind});
        bytes_ += bytes;
        ++count(kind);
        ++sequence_;
        if (settlement) {
            omitted_display_ = 0;
            omitted_queries_ = 0;
            coalesced_metadata_ = 0;
            omitted_feedback_ = 0;
        }
        return Admission::Added;
    }

    /** Move the front message while retaining its byte/count reservation. */
    nlohmann::json begin_send() {
        if (in_flight_ || queue_.empty()) {
            throw std::logic_error("invalid outbox send");
        }
        in_flight_ = true;
        return std::move(queue_.front().message);
    }
    /** Release exactly once after transport admission succeeds or fails. */
    void complete_send() {
        if (!in_flight_) {
            throw std::logic_error("outbox has no active send");
        }
        bytes_ -= queue_.front().bytes;
        --count(queue_.front().kind);
        queue_.pop_front();
        in_flight_ = false;
    }
    void close() noexcept { closed_ = true; }
    /** Strand-owned admission pause. Existing entries drain unchanged; new
     * noncritical output is counted as omitted, preserving lifecycle reserves.
     * The caller must resume admission when its waiting request proceeds or exits.
     */
    void pause_noncritical(bool paused) noexcept { noncritical_paused_ = paused; }
    bool closed() const noexcept { return closed_; }
    bool empty() const noexcept { return queue_.empty(); }
    std::size_t bytes() const noexcept { return bytes_; }
    std::size_t size() const noexcept { return queue_.size(); }
    std::uint64_t sequence() const noexcept { return sequence_; }
    std::size_t omitted_display() const noexcept { return omitted_display_; }

private:
    struct Entry {
        nlohmann::json message;
        std::size_t bytes;
        Kind kind;
    };
    static bool fits(std::size_t own, std::size_t transport, std::size_t added, std::size_t limit) {
        return own <= limit && transport <= limit - own && added <= limit - own - transport;
    }
    std::size_t& count(Kind kind) {
        if (kind == Kind::Lifecycle) {
            return lifecycle_count_;
        }
        if (kind == Kind::Metadata) {
            return metadata_count_;
        }
        return ordinary_count_;
    }
    void note_omission(Kind kind) {
        if (kind == Kind::Preview) {
            ++omitted_display_;
        } else if (kind == Kind::Query) {
            ++omitted_queries_;
        } else if (kind == Kind::Metadata) {
            ++coalesced_metadata_;
        } else {
            ++omitted_feedback_;
        }
    }
    std::deque<Entry> queue_;
    std::size_t capacity_;
    std::size_t bytes_ = 0;
    std::size_t ordinary_count_ = 0;
    std::size_t metadata_count_ = 0;
    std::size_t lifecycle_count_ = 0;
    std::size_t omitted_display_ = 0;
    std::size_t omitted_queries_ = 0;
    std::size_t coalesced_metadata_ = 0;
    std::size_t omitted_feedback_ = 0;
    std::uint64_t sequence_ = 0;
    bool in_flight_ = false;
    bool noncritical_paused_ = false;
    bool closed_ = false;
};
} // namespace core
