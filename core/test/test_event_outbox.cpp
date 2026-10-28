#define BOOST_TEST_MODULE CoreEventOutbox
#include <boost/test/unit_test.hpp>
#include "core/event_outbox.hpp"
#include "core/event_admission.hpp"
#include "protocol/wire.hpp"
using Json = nlohmann::json;
using Outbox = core::EventOutbox;
using Kind = Outbox::Kind;
using Admission = Outbox::Admission;
namespace {
/** One event envelope with the correlation fields every emitted event carries. */
protocol::EventEnvelope event(protocol::EventName name, Json data = Json::object()) {
    protocol::EventEnvelope envelope;
    envelope.event = name;
    envelope.session_id = "session";
    envelope.worker_id = "worker";
    envelope.request_id = "request";
    envelope.run_id = "run";
    envelope.data = std::move(data);
    return envelope;
}
}

BOOST_AUTO_TEST_CASE(repeated_metadata_coalesces_without_spending_lifecycle_reserves_or_sequence) {
    Outbox queue(1);
    BOOST_CHECK(queue.admit(event(protocol::EventName::model_response), Kind::Preview) == Admission::Added);
    BOOST_CHECK(queue.admit(event(protocol::EventName::persisted, {{"checkpoint", 0}}), Kind::Metadata) == Admission::Added);
    for (int i = 1; i < 1000; ++i) {
        BOOST_CHECK(queue.admit(event(protocol::EventName::persisted, {{"checkpoint", i}}), Kind::Metadata) == Admission::Replaced);
        BOOST_CHECK(queue.admit(event(protocol::EventName::tool_calls), Kind::Preview) == Admission::Omitted);
    }
    BOOST_TEST(queue.size() == 2u);
    BOOST_TEST(queue.sequence() == 2u);
    auto first = queue.begin_send();
    BOOST_TEST(first.at("sequence") == 1);
    queue.complete_send();
    auto latest = queue.begin_send();
    BOOST_TEST(latest.at("data").at("checkpoint") == 999);
    BOOST_TEST(latest.at("sequence") == 2);
    // Updating metadata cannot alter the sender's moved front message.
    BOOST_CHECK(queue.admit(event(protocol::EventName::persisted, {{"checkpoint", 1000}}), Kind::Metadata) == Admission::Added);
    queue.complete_send();
    queue.begin_send(); queue.complete_send();
    BOOST_CHECK(queue.admit(event(protocol::EventName::run_finished), Kind::Lifecycle) == Admission::Added);
    BOOST_TEST(queue.omitted_display() == 0u);
    auto terminal = queue.begin_send();
    BOOST_TEST(terminal.at("data").at("omitted_display_events") == 999);
    BOOST_TEST(terminal.at("data").at("coalesced_metadata_events") == 999);
    queue.complete_send();
    BOOST_TEST(queue.bytes() == 0u);
}

BOOST_AUTO_TEST_CASE(failed_admission_preserves_sequence_reservations_and_omission_settlement) {
    Outbox queue(1);
    queue.admit(event(protocol::EventName::model_response), Kind::Preview);
    queue.admit(event(protocol::EventName::tool_calls), Kind::Preview);
    const auto bytes = queue.bytes();
    const auto sequence = queue.sequence();
    BOOST_CHECK(queue.admit(event(protocol::EventName::run_finished), Kind::Lifecycle,
        intercom::default_write_byte_capacity) == Admission::Omitted);
    BOOST_TEST(queue.bytes() == bytes);
    BOOST_TEST(queue.sequence() == sequence);
    BOOST_TEST(queue.omitted_display() == 1u);
    BOOST_CHECK_THROW(queue.admit(event(protocol::EventName::run_finished, {{"message", std::string(1024 * 1024, 'X')}}),
        Kind::Lifecycle), std::length_error);
    BOOST_TEST(queue.sequence() == sequence);
    BOOST_TEST(queue.omitted_display() == 1u);
    BOOST_CHECK(queue.admit(event(protocol::EventName::run_finished), Kind::Lifecycle) == Admission::Added);
    queue.begin_send(); queue.complete_send();
    auto terminal = queue.begin_send();
    BOOST_TEST(terminal.at("data").at("omitted_display_events") == 1);
    queue.complete_send();
    queue.close();
    BOOST_CHECK(queue.admit(event(protocol::EventName::ready), Kind::Lifecycle) == Admission::Omitted);
    BOOST_TEST(queue.sequence() == 2u);
    BOOST_TEST(queue.bytes() == 0u);
}

BOOST_AUTO_TEST_CASE(storage_failure_settles_omissions_and_transport_pressure_sheds_only_optional_work) {
    Outbox queue(1);
    BOOST_CHECK(queue.admit(event(protocol::EventName::model_response), Kind::Preview, Outbox::soft_bytes) == Admission::Omitted);
    BOOST_CHECK(queue.admit(event(protocol::EventName::history), Kind::Query, Outbox::soft_bytes) == Admission::Omitted);
    BOOST_CHECK(queue.admit(event(protocol::EventName::input_rejected), Kind::Feedback, Outbox::soft_bytes) == Admission::Omitted);
    BOOST_CHECK(queue.admit(event(protocol::EventName::error, {{"durable", false}}), Kind::Lifecycle, Outbox::soft_bytes) == Admission::Added);
    auto error = queue.begin_send();
    BOOST_TEST(error.at("data").at("omitted_display_events") == 1);
    BOOST_TEST(error.at("data").at("omitted_query_events") == 1);
    BOOST_TEST(error.at("data").at("omitted_feedback_events") == 1);
    BOOST_TEST(queue.omitted_display() == 0u);
    queue.complete_send();
}

BOOST_AUTO_TEST_CASE(lifecycle_slot_exhaustion_does_not_spend_sequence_or_omission_receipt) {
    Outbox queue(1);
    for (std::size_t i = 0; i < Outbox::lifecycle_slots; ++i) {
        BOOST_CHECK(queue.admit(event(protocol::EventName::run_started), Kind::Lifecycle) == Admission::Added);
    }
    queue.admit(event(protocol::EventName::model_response), Kind::Preview);
    queue.admit(event(protocol::EventName::tool_results), Kind::Preview);
    const auto bytes = queue.bytes();
    const auto sequence = queue.sequence();
    BOOST_CHECK(queue.admit(event(protocol::EventName::run_finished), Kind::Lifecycle) == Admission::Omitted);
    BOOST_TEST(queue.bytes() == bytes);
    BOOST_TEST(queue.sequence() == sequence);
    BOOST_TEST(queue.omitted_display() == 1u);
    queue.begin_send(); queue.complete_send();
    BOOST_CHECK(queue.admit(event(protocol::EventName::run_finished), Kind::Lifecycle) == Admission::Added);
    BOOST_TEST(queue.omitted_display() == 0u);
}

BOOST_AUTO_TEST_CASE(pending_admission_pauses_new_polling_output_without_changing_existing_entries) {
    Outbox queue(1);
    queue.admit(event(protocol::EventName::history), Kind::Query);
    queue.admit(event(protocol::EventName::status, {{"active", false}}), Kind::Metadata);
    const auto bytes = queue.bytes();
    const auto sequence = queue.sequence();
    queue.pause_noncritical(true);
    for (int i = 0; i < 100; ++i) {
        BOOST_CHECK(queue.admit(event(protocol::EventName::history), Kind::Query) == Admission::Omitted);
        BOOST_CHECK(queue.admit(event(protocol::EventName::status, {{"active", true}}), Kind::Metadata) == Admission::Omitted);
    }
    BOOST_CHECK(queue.admit(event(protocol::EventName::input_rejected), Kind::Feedback) == Admission::Omitted);
    BOOST_TEST(queue.size() == 2u);
    BOOST_TEST(queue.bytes() == bytes);
    BOOST_TEST(queue.sequence() == sequence);
    queue.begin_send();
    queue.complete_send();
    auto status = queue.begin_send();
    BOOST_TEST(status.at("data").at("active") == false);
    queue.complete_send();
    BOOST_CHECK(queue.empty());
    // Lifecycle admission remains available throughout the pause.
    BOOST_CHECK(queue.admit(event(protocol::EventName::run_finished), Kind::Lifecycle) == Admission::Added);
    auto terminal = queue.begin_send();
    BOOST_TEST(terminal.at("data").at("omitted_query_events") == 100);
    BOOST_TEST(terminal.at("data").at("coalesced_metadata_events") == 100);
    BOOST_TEST(terminal.at("data").at("omitted_feedback_events") == 1);
    queue.complete_send();
    queue.pause_noncritical(false);
    BOOST_CHECK(queue.admit(event(protocol::EventName::history), Kind::Query) == Admission::Added);
    BOOST_CHECK(queue.admit(event(protocol::EventName::status), Kind::Metadata) == Admission::Added);
}

BOOST_AUTO_TEST_CASE(query_preflight_counts_only_skipped_work_without_spending_reservations) {
    Outbox queue(1);
    BOOST_CHECK(!queue.omit_query_when_congested());
    queue.admit(event(protocol::EventName::history), Kind::Query);
    const auto bytes = queue.bytes();
    BOOST_CHECK(queue.omit_query_when_congested());
    BOOST_TEST(queue.sequence() == 1u);
    BOOST_TEST(queue.size() == 1u);
    BOOST_TEST(queue.bytes() == bytes);
    queue.begin_send();
    queue.complete_send();
    BOOST_CHECK(!queue.omit_query_when_congested());
    queue.pause_noncritical(true);
    BOOST_CHECK(queue.omit_query_when_congested());
    queue.pause_noncritical(false);
    BOOST_CHECK(queue.omit_query_when_congested(Outbox::soft_bytes));
    BOOST_TEST(queue.sequence() == 1u);
    BOOST_TEST(queue.bytes() == 0u);
    queue.admit(event(protocol::EventName::run_finished), Kind::Lifecycle);
    auto terminal = queue.begin_send();
    BOOST_TEST(terminal.at("data").at("omitted_query_events") == 3);
    queue.complete_send();
    queue.close();
    BOOST_CHECK(queue.omit_query_when_congested());
    BOOST_TEST(queue.sequence() == 2u);
}

BOOST_AUTO_TEST_CASE(classification_keeps_ordinary_metadata_out_of_the_lifecycle_reserve) {
    using protocol::EventName;
    const auto kind = [](EventName name, const Json& data, bool failed) {
        return static_cast<int>(core::classify_event(name, data, failed));
    };
    const auto metadata = static_cast<int>(Kind::Metadata);
    const auto lifecycle = static_cast<int>(Kind::Lifecycle);

    // These four stay latest-value metadata in every storage state: a repeated
    // snapshot must not consume the reserve the terminal diagnostic needs.
    for (const bool failed : {false, true}) {
        for (const auto name : {EventName::persisted, EventName::status,
                                EventName::options, EventName::export_error}) {
            BOOST_TEST(kind(name, Json::object(), failed) == metadata);
        }
    }
    // Only a latched storage failure promotes the diagnostic itself.
    BOOST_TEST(kind(EventName::error, Json::object(), false) == metadata);
    BOOST_TEST(kind(EventName::error, Json::object(), true) == lifecycle);

    // The rest of the vocabulary does not depend on the storage state.
    for (const bool failed : {false, true}) {
        BOOST_TEST(kind(EventName::model_response, Json::object(), failed)
                   == static_cast<int>(Kind::Preview));
        BOOST_TEST(kind(EventName::tool_calls, Json::object(), failed)
                   == static_cast<int>(Kind::Preview));
        BOOST_TEST(kind(EventName::tool_results, Json::object(), failed)
                   == static_cast<int>(Kind::Preview));
        for (const auto name : {EventName::history, EventName::answer,
                                EventName::history_error, EventName::answer_error}) {
            BOOST_TEST(kind(name, Json::object(), failed) == static_cast<int>(Kind::Query));
        }
        for (const auto name : {EventName::ready, EventName::input_admitted,
                                EventName::run_started, EventName::input_committed,
                                EventName::run_finished}) {
            BOOST_TEST(kind(name, Json::object(), failed) == lifecycle);
        }
        BOOST_TEST(kind(EventName::input_rejected, Json::object(), failed)
                   == static_cast<int>(Kind::Feedback));
        BOOST_TEST(kind(EventName::compact_finished, Json{{"origin", "automatic"}}, failed)
                   == metadata);
        BOOST_TEST(kind(EventName::compact_finished, Json::object(), failed) == lifecycle);
    }
}

BOOST_AUTO_TEST_CASE(a_latched_storage_failure_leaves_metadata_coalescing_and_the_terminal_error_admissible) {
    // The regression this guards: classifying status like a lifecycle event once
    // storage failed consumed the eight reserved slots, so a polling client could
    // make the final diagnostic unadmittable.
    Outbox queue(1);
    constexpr bool failed = true;
    const auto admit = [&](protocol::EventName name, Json data) {
        return queue.admit(event(name, std::move(data)),
                           core::classify_event(name, data, failed));
    };

    for (int index = 0; index < 3; ++index) {
        BOOST_CHECK(admit(protocol::EventName::run_started, Json::object()) == Admission::Added);
    }
    // Twelve identical snapshots coalesce into the one queued metadata slot.
    BOOST_CHECK(admit(protocol::EventName::status, Json{{"active", true}}) == Admission::Added);
    for (int index = 0; index < 11; ++index) {
        BOOST_CHECK(admit(protocol::EventName::status, Json{{"active", index % 2 == 0}})
                    == Admission::Replaced);
    }
    // The terminal storage diagnostic is still admitted with no draining.
    BOOST_CHECK(admit(protocol::EventName::error, Json{{"durable", false}}) == Admission::Added);
    BOOST_TEST(queue.sequence() == 5u);
    BOOST_TEST(queue.size() == 5u);
}
