#pragma once

//
// responses/interpreter.hpp — ModelRequestInterpreter for POST /responses
// ========================================================================
//
// The request half of the Responses-API compatibility layer: it flattens one
// model_io::AgentInputState (plus a ModelEndpoint and plain-JSON generation
// parameters) into a single Responses-API request whose streaming reply is
// decoded by ResponsesStreamHandler — the two halves share the round-trip
// conventions below.
//
// Body layout (builder-owned keys in bold; every other generation key passes
// through verbatim — temperature, max_output_tokens, reasoning, ...):
//
//   model          from generation (the interface contract's hard error #1)
//   instructions   system_prompt.render().markdown — only when non-empty
//   input          the conversation, flattened (see below)
//   tools          each Invocable -> {type:"function", name, description,
//                  parameters} — only when any are registered
//   stream         true, ALWAYS (builder-owned): this layer speaks SSE only;
//                  the transport has http_request for non-streaming JSON
//                  bodies, but this layer's decoder is the SSE handler
//   store          false by default; a generation "store" wins
//   include        when store resolves to false, "reasoning.encrypted_content"
//                  is ensured (appended if missing) — multi-turn reasoning
//                  round-trip depends on it precisely under store=false
//
// `input` flattening, per turn, in order:
//
//   user_input           -> {type:"message", role:"user", content:[parts...]}
//                           one provider part per ordered Content entry:
//                           text -> input_text, external_ref -> input_image,
//                           binary -> input_file; extras.type may explicitly
//                           select input_image/input_file and extras fields
//                           (detail, filename, file_id, ...) are preserved
//   model_response       -> up to three groups, in this order:
//     reasoning          extras.items (captured done items) re-emitted
//                       VERBATIM — ids, summaries and encrypted_content must
//                       survive; a bare reasoning Content synthesizes
//                       {type:"reasoning", summary:[{type:"summary_text"}]}
//     the message itself extras["output_items"] message items re-emitted
//                       verbatim (annotations/phase/status preserved);
//                       otherwise a synthesized {role:"assistant",
//                       content:[{type:"output_text", text:raw}]} when raw
//                       is non-empty
//     invokes            each -> {type:"function_call", call_id, name,
//                       arguments} — arguments as a JSON STRING
//                       (is_string() passthrough, else dump()); q.extras
//                       carrying a captured function_call item wins verbatim
//   invoke_returns       -> {type:"function_call_output", call_id, output}:
//     call_id            invoke_return->query.id, else positional alignment
//                       with the parent response's invokes[i].id, else the
//                       key is omitted (legal on the wire); a captured
//                       function_call_output item in extras wins verbatim
//     output             a single text content's raw string, or a provider
//                       content array for multiple/non-text entries; an empty
//                       content list falls back to output.raw
//
// action_status is deliberately NOT mapped (explicit placeholder; the wire
// `phase` annotation is future work). A MessageItem of type InvokeReturn in
// a user_input position still takes the function_call_output branch — the
// embedded record says what it is.
//
// Transport: resolve_endpoint() picks host/port/target and
// apply_transport_headers() applies auth/user-agent/extra headers. The
// endpoint's request_path is used VERBATIM — set it to "/v1/responses" (the
// dataclass default "/chat/completions" is chat-completions flavoured and
// would misroute this body; the interface contract allows no third hard
// error, so this stays the caller's responsibility). SSE-specific headers
// (Accept: text/event-stream, Content-Type: application/json) are set here,
// not in the shared helper.
//
// Implementation contract (model_request.hpp) applies in full: synchronous,
// pure, no I/O, stateless — one instance may serve concurrent calls; lenient
// on imperfect conversations; exactly two hard errors (missing non-empty
// "model"; hostless base_url), both HttpRequestException{CreateRequest}.
//

#include <nlohmann/json.hpp>
#include <utility>

#include "dataclass/model_io.hpp"
#include "endpoint/model_request.hpp"
#include "llm/responses/dialect.hpp"

namespace llm::responses {

/**
 * @brief Builds one complete POST /responses request from the conversation.
 *
 * See the header block for the body layout and the round-trip conventions
 * shared with ResponsesStreamHandler.
 */
class ResponsesInterpreter : public endpoint::ModelRequestInterpreter {
public:
    explicit ResponsesInterpreter(ResponsesDialectPtr dialect = default_dialect())
        : _dialect(dialect ? std::move(dialect) : default_dialect()) {}

    HttpRequest build_request(
        const model_io::AgentInputState& conversation,
        const model_io::ModelEndpoint& endpoint,
        const nlohmann::json& generation) override;

private:
    ResponsesDialectPtr _dialect;
};

} // namespace llm::responses
