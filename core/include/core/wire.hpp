#pragma once

/**
 * @file core/wire.hpp
 * @brief Conversions from the persisted model types to their wire records.
 *
 * The protocol package owns the shapes; core owns the state they project. This
 * header is the single place where a `model_io` enum becomes its wire spelling,
 * so no serialization site restates a vocabulary the generated manifest already
 * publishes. Application-only vocabularies (loop status, persistence boundary,
 * input operations) convert beside the code that owns them.
 */

#include "core/protocol.hpp"
#include "protocol/wire.hpp"

namespace core {

protocol::ContentType wire_type(model_io::ContentType type);
protocol::Modality wire_modality(model_io::Modality modality);
protocol::MessageType wire_type(model_io::MessageItemType type);
protocol::CallScheduling wire_scheduling(model_io::InvokeType type);
protocol::CallSecurity wire_security(model_io::InvokeSecurity security);

/** Plain projection of a stored part; budgets belong to the display algorithms. */
protocol::Content wire_content(const model_io::Content& part);

/** The stored form of an admitted part; the codec has already checked the labels. */
model_io::Content model_content(const protocol::RequestContent& part);

/** One full invocation for a confirmation request: arguments are never shortened. */
protocol::Call wire_call(const model_io::InvokeQuery& query);

} // namespace core
