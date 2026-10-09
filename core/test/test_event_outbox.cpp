#define BOOST_TEST_MODULE CoreEventOutbox
#include <boost/test/unit_test.hpp>
#include "core/event_outbox.hpp"
using Json = nlohmann::json;
using Outbox = core::EventOutbox;
using Kind = Outbox::Kind;
using Admission = Outbox::Admission;
namespace {
Json event(std::string name, Json data = Json::object()) {
    return {{"event", std::move(name)}, {"request_id", "request"}, {"run_id", "run"}, {"data", std::move(data)}};
}
}

BOOST_AUTO_TEST_CASE(repeated_metadata_coalesces_without_spending_lifecycle_reserves_or_sequence) {
    Outbox queue(1);
    BOOST_CHECK(queue.admit(event("model_response"), Kind::Preview) == Admission::Added);
    BOOST_CHECK(queue.admit(event("persisted", {{"checkpoint", 0}}), Kind::Metadata) == Admission::Added);
    for (int i = 1; i < 1000; ++i) {
        BOOST_CHECK(queue.admit(event("persisted", {{"checkpoint", i}}), Kind::Metadata) == Admission::Replaced);
        BOOST_CHECK(queue.admit(event("tool_calls"), Kind::Preview) == Admission::Omitted);
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
    BOOST_CHECK(queue.admit(event("persisted", {{"checkpoint", 1000}}), Kind::Metadata) == Admission::Added);
    queue.complete_send();
    queue.begin_send(); queue.complete_send();
    BOOST_CHECK(queue.admit(event("run_finished"), Kind::Lifecycle) == Admission::Added);
    BOOST_TEST(queue.omitted_display() == 0u);
    auto terminal = queue.begin_send();
    BOOST_TEST(terminal.at("data").at("omitted_display_events") == 999);
    BOOST_TEST(terminal.at("data").at("coalesced_metadata_events") == 999);
    queue.complete_send();
    BOOST_TEST(queue.bytes() == 0u);
}

BOOST_AUTO_TEST_CASE(failed_admission_preserves_sequence_reservations_and_omission_settlement) {
    Outbox queue(1);
    queue.admit(event("model_response"), Kind::Preview);
    queue.admit(event("tool_calls"), Kind::Preview);
    const auto bytes = queue.bytes();
    const auto sequence = queue.sequence();
    BOOST_CHECK(queue.admit(event("run_finished"), Kind::Lifecycle,
        intercom::default_write_byte_capacity) == Admission::Omitted);
    BOOST_TEST(queue.bytes() == bytes);
    BOOST_TEST(queue.sequence() == sequence);
    BOOST_TEST(queue.omitted_display() == 1u);
    BOOST_CHECK_THROW(queue.admit(event("run_finished", {{"message", std::string(1024 * 1024, 'X')}}),
        Kind::Lifecycle), std::length_error);
    BOOST_TEST(queue.sequence() == sequence);
    BOOST_TEST(queue.omitted_display() == 1u);
    BOOST_CHECK(queue.admit(event("run_finished"), Kind::Lifecycle) == Admission::Added);
    queue.begin_send(); queue.complete_send();
    auto terminal = queue.begin_send();
    BOOST_TEST(terminal.at("data").at("omitted_display_events") == 1);
    queue.complete_send();
    queue.close();
    BOOST_CHECK(queue.admit(event("ready"), Kind::Lifecycle) == Admission::Omitted);
    BOOST_TEST(queue.sequence() == 2u);
    BOOST_TEST(queue.bytes() == 0u);
}

BOOST_AUTO_TEST_CASE(storage_failure_settles_omissions_and_transport_pressure_sheds_only_optional_work) {
    Outbox queue(1);
    BOOST_CHECK(queue.admit(event("model_response"), Kind::Preview, Outbox::soft_bytes) == Admission::Omitted);
    BOOST_CHECK(queue.admit(event("history"), Kind::Query, Outbox::soft_bytes) == Admission::Omitted);
    BOOST_CHECK(queue.admit(event("input_rejected"), Kind::Feedback, Outbox::soft_bytes) == Admission::Omitted);
    BOOST_CHECK(queue.admit(event("error", {{"durable", false}}), Kind::Lifecycle, Outbox::soft_bytes) == Admission::Added);
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
        BOOST_CHECK(queue.admit(event("run_started"), Kind::Lifecycle) == Admission::Added);
    }
    queue.admit(event("model_response"), Kind::Preview);
    queue.admit(event("tool_results"), Kind::Preview);
    const auto bytes = queue.bytes();
    const auto sequence = queue.sequence();
    BOOST_CHECK(queue.admit(event("run_finished"), Kind::Lifecycle) == Admission::Omitted);
    BOOST_TEST(queue.bytes() == bytes);
    BOOST_TEST(queue.sequence() == sequence);
    BOOST_TEST(queue.omitted_display() == 1u);
    queue.begin_send(); queue.complete_send();
    BOOST_CHECK(queue.admit(event("run_finished"), Kind::Lifecycle) == Admission::Added);
    BOOST_TEST(queue.omitted_display() == 0u);
}
