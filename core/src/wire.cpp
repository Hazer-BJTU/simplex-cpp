#include "core/wire.hpp"

#include <stdexcept>
#include <string>

namespace core {
namespace {

/** A stored value without a wire spelling is a programming error, never data. */
[[noreturn]] void no_wire_spelling(const char* what) {
    throw std::logic_error(std::string("no wire spelling for ") + what);
}

} // namespace

protocol::ContentType wire_type(model_io::ContentType type) {
    switch (type) {
        case model_io::ContentType::Text: return protocol::ContentType::text;
        case model_io::ContentType::Binary: return protocol::ContentType::binary;
        case model_io::ContentType::ExternalRef: return protocol::ContentType::external_ref;
    }
    no_wire_spelling("content type");
}

protocol::Modality wire_modality(model_io::Modality modality) {
    switch (modality) {
        case model_io::Modality::Text: return protocol::Modality::text;
        case model_io::Modality::Image: return protocol::Modality::image;
        case model_io::Modality::Audio: return protocol::Modality::audio;
        case model_io::Modality::Video: return protocol::Modality::video;
        case model_io::Modality::Document: return protocol::Modality::document;
    }
    no_wire_spelling("modality");
}

protocol::MessageType wire_type(model_io::MessageItemType type) {
    switch (type) {
        case model_io::MessageItemType::UserInput: return protocol::MessageType::user_input;
        case model_io::MessageItemType::ModelResponse: return protocol::MessageType::model_response;
        case model_io::MessageItemType::InvokeReturn: return protocol::MessageType::invoke_return;
    }
    no_wire_spelling("message type");
}

protocol::CallScheduling wire_scheduling(model_io::InvokeType type) {
    switch (type) {
        case model_io::InvokeType::ReadOnly: return protocol::CallScheduling::read_only;
        case model_io::InvokeType::ParallWrite: return protocol::CallScheduling::parall_write;
        case model_io::InvokeType::SerialWrite: return protocol::CallScheduling::serial_write;
    }
    no_wire_spelling("call scheduling");
}

protocol::CallSecurity wire_security(model_io::InvokeSecurity security) {
    switch (security) {
        case model_io::InvokeSecurity::DefaultDeny: return protocol::CallSecurity::default_deny;
        case model_io::InvokeSecurity::RequireConfirm:
            return protocol::CallSecurity::require_confirm;
        case model_io::InvokeSecurity::Trusted: return protocol::CallSecurity::trusted;
    }
    no_wire_spelling("call security");
}

protocol::Content wire_content(const model_io::Content& part) {
    protocol::Content content;
    content.type = wire_type(part.type);
    content.modality = wire_modality(part.modality);
    content.raw = part.raw;
    content.extras = part.extras;
    return content;
}

model_io::Content model_content(const protocol::RequestContent& part) {
    model_io::Content content;
    switch (part.type) {
        case protocol::ContentType::text: content.type = model_io::ContentType::Text; break;
        case protocol::ContentType::binary: content.type = model_io::ContentType::Binary; break;
        case protocol::ContentType::external_ref:
            content.type = model_io::ContentType::ExternalRef;
            break;
        default: no_wire_spelling("content type");
    }
    switch (part.modality) {
        case protocol::Modality::text: content.modality = model_io::Modality::Text; break;
        case protocol::Modality::image: content.modality = model_io::Modality::Image; break;
        case protocol::Modality::audio: content.modality = model_io::Modality::Audio; break;
        case protocol::Modality::video: content.modality = model_io::Modality::Video; break;
        case protocol::Modality::document: content.modality = model_io::Modality::Document; break;
        default: no_wire_spelling("modality");
    }
    content.raw = part.raw;
    content.extras = part.extras;
    return content;
}

protocol::Call wire_call(const model_io::InvokeQuery& query) {
    protocol::Call call;
    call.id = query.id;
    call.name = query.name;
    call.arguments = query.arguments;
    call.type = wire_scheduling(query.type);
    call.security = wire_security(query.security);
    call.extras = query.extras;
    return call;
}

} // namespace core
