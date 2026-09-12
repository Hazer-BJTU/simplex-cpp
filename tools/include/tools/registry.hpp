#pragma once

//
// registry.hpp — the tool registry: one routing table over many tool sets
// ======================================================================
//
// toolsets.hpp answers "what is one call" — a ToolSet settles a query and runs
// it. This header answers the two questions an agent loop actually has: WHICH
// set runs a call, and HOW a whole batch of them comes back.
//
// THE UNIT OF REGISTRATION IS THE SET, THE UNIT OF ROUTING IS THE NAME. add()
// takes a ToolSet and registers every name get_tools() advertises; a call is
// resolved by query.name, which is the only part of a call the model controls
// and the only part the table can be keyed on. Two sets may not provide the
// same name — a call that resolved to two tools would have no defined answer —
// so a duplicate is refused while registering (a startup-time configuration
// error, loud) rather than discovered at dispatch time (a per-call failure the
// model would be told about).
//
// A BATCH GOES THROUGH FOUR STEPS, and the order between them is the design:
//
//   1. ROUTE AND SETTLE   synchronous, no suspension. Every call of the batch
//                         is resolved to its set and settled in place by
//                         ToolSet::prepare() — dispatch, ensure_arguments,
//                         write_attributes — so query.type says whether the call
//                         may run alongside its neighbours and query.security
//                         says how much trust it needs. This happens for the
//                         WHOLE batch before any of it runs: one call's schedule
//                         must not depend on how far its neighbours got, and a
//                         batch is settled from a single pass of the caller's
//                         own coroutine (toolsets.hpp explains why the split is
//                         where it is).
//   2. SERIAL FIRST       every call whose settled type is SerialWrite is
//                         awaited one at a time, in batch order. It asked for
//                         the executor to itself, and it gets it: none of this
//                         batch is in flight while one of them runs.
//   3. PARALLEL, THEN     the rest are spawned together on the executor — no
//                         strand between them, which is what makes the channel
//                         below a CONCURRENT channel — and the batch WAITS for
//                         every one of them. A batch is a join, not a detach:
//                         the caller is the agent loop, which cannot assemble a
//                         model turn until every call it made has an answer.
//   4. ASSEMBLE           the records are returned in the ORDER OF THE CALLS,
//                         never in completion order, one record per call.
//
// What co_spawn does with that executor is worth knowing, because it shapes what
// step 3 costs: a branch runs INLINE on the collector's thread up to its first
// real suspension, and everything after that is dispatched on the executor it
// was spawned with. A batch of tools that never wait is therefore one
// single-threaded pass with no hops, a batch that waits overlaps — and a strand
// passed as the executor serialises a branch from its first suspension on only,
// since the synchronous prefix never left the caller's thread.
//
// Serial before parallel is not a scheduling detail, it is what SerialWrite
// MEANS: a writer that declared it must not run concurrently with anything, and
// the only way to honour that from a batch is to run it while the rest of the
// batch waits. ReadOnly and ParallWrite are the only types allowed to overlap,
// and they are named explicitly: a type a future enum adds defaults to the slow
// side, because a wrong ReadOnly is a data race while a wrong SerialWrite is
// merely slower.
//
// EVERY CALL IS ANSWERED, EXACTLY ONCE. That is a wire requirement, not
// politeness: a provider rejects an assistant message whose tool_calls are not
// each answered by exactly one tool message (see
// llm/src/chat_completions/interpreter.cpp::emit_tool_results). So no path here
// may DROP a call — not an unknown tool, not a set that breaks its contract, not
// a report that never arrives. Whatever fails, the call gets a record, and the
// record carries the call (query.id) so the conversation can correlate it. The
// assembly is positional over the batch for the same reason: results[i] answers
// the call the caller passed at index i.
//
// FAILURE COMES IN THREE SIZES, and the record's stage says which:
//
//   no such tool       the table has no entry for query.name: a Dispatch
//                      failure raised here, before any set is involved.
//   the SET is broken  a ToolSet method threw what its own contract says it
//                      cannot — prepare() promises it always throws
//                      InvokeException, execute() promises it never throws.
//                      Reported at Stage::Unknown, naming the set, and reported
//                      HERE rather than left to unwind: the other calls of the
//                      batch keep their answers.
//   the CALL failed    everything the tool layer reports on its own
//                      (ArgumentParse, SecurityCheck, Invoke, ResultCheck —
//                      toolsets.hpp). This layer only files those records and
//                      never rewrites one.
//
// IDENTITY AND FILING. A record is filed under the identity of the call the
// registry was GIVEN — InvokeQuery::mangled_name() of the query AS IT ARRIVED,
// before any hook has run — and the assembly looks every call up by that same
// key. The key is taken up front for two reasons: prepare() settles the query in
// place (ensure_arguments fills defaults, write_attributes rewrites the
// attributes), so an identity read off the settled object is an identity that
// object can change; and a tool that renamed the query it was handed must not be
// able to move its own record out of reach. mangled_name() is built from name
// and id precisely so this holds (dataclass/model_io.hpp), which also means two
// calls in one batch that agree on name and ID are ONE identity: they share a
// record — the most a provider could distinguish them by either.
//
// THREADING. add()/remove()/clear() are configuration: they are not synchronised
// against dispatch, so a host registers everything before it serves calls, the
// same rule the plugin layer follows. Dispatch itself is read-only and safe on a
// multi-threaded io_context: the batch reads the table only while ROUTING — the
// synchronous first step, before its first suspension — and from then on holds a
// shared_ptr per call, so a batch that has started no longer needs the registry
// it came from (the registry still has to outlive that first resume, as any
// `co_await registry.execute(...)` implies). `execute()` is const for the same
// reason: dispatching changes nothing.
//
// LOGGING. Only what a host cannot otherwise see is logged: a call the table does
// not know (debug — a model naming a tool that does not exist is ordinary, and
// the record already tells the model), and a set that broke its contract or a
// branch that never reported (error — those make a batch look like an ordinary
// set of tool results). Everything else travels back in the records, which is
// where the model reads it.
//
// WHAT THIS LAYER DELIBERATELY DOES NOT DO: own a tool's lifecycle (a set does:
// build()/release() are its business), retry, back off, prioritise, propagate
// cancellation into a branch, or impose a policy on a set. A host that needs any
// of those wraps this, and still gets one record per call out of it.
//

#include <algorithm>
#include <cstddef>
#include <exception>
#include <format>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

#include "dataclass/model_io.hpp"
#include "logging/logger.hpp"
#include "tools/invoke_exception.hpp"
#include "tools/toolsets.hpp"

namespace tools {

/**
 * The host's routing table over tool sets: which set provides a tool name, and
 * how one batch of calls is settled, run and collected.
 *
 * A host holds ONE of these and hands the model the catalogue it flattens
 * (get_tools()); a tool call from the model goes back in through execute(). See
 * the file header for the batch's four steps, the identity a record is filed
 * under, and the failure sizes.
 */
class ToolRegistry {
public:
    using ToolSetPtr = std::shared_ptr<ToolSet>;
    using ToolHandle = ToolSet::ToolHandle;
    /// What execute() answers with: one record per call, in the order of the
    /// calls (file header, step 4).
    using Results = std::vector<model_io::InvokeReturn>;
    /// The identity a call is filed under while a batch is in flight: the
    /// mangled_name() of the query as it arrived (file header).
    using CallKey = std::string;

    ToolRegistry() = default;
    ~ToolRegistry() = default;
    // Copying the table copies the POINTERS: both tables then route to the same
    // sets. That is what a host handing a registry to a component — a plugin
    // that wants to dispatch through the host's tools, say — needs, and it is
    // why the entries are shared_ptr in the first place. The table does not own
    // the sets' lifetimes any more than a catalogue does.
    ToolRegistry(const ToolRegistry&) = default;
    ToolRegistry& operator = (const ToolRegistry&) = default;
    ToolRegistry(ToolRegistry&&) = default;
    ToolRegistry& operator = (ToolRegistry&&) = default;

    // ===== configuration =====================================================

    /**
     * Register a set, and with it every tool name it advertises.
     *
     * The whole set is checked before anything is registered, so a set that
     * cannot be added leaves the table exactly as it was — a host that catches
     * the duplicate is not left with a half-registered set.
     *
     * @param tool_set the set to register; kept by shared_ptr, so the registry
     *        shares ownership with whoever made it rather than owning it.
     * @throws std::invalid_argument for a null set, or for one that offers a
     *         tool with an empty name: no call could ever route to that name,
     *         and an empty query.name would otherwise match it by accident
     *         instead of being reported as the dispatch failure it is.
     * @throws std::runtime_error for a name another set already provides. A
     *         registration conflict is a configuration bug, so it is loud and
     *         it happens at startup rather than per call.
     */
    void add(ToolSetPtr tool_set) {
        if (tool_set == nullptr) {
            throw std::invalid_argument(
                "ToolRegistry::add() was given a null toolset");
        }

        const std::vector<std::string> names = tool_set->supported_names();

        for (const std::string& name : names) {
            if (name.empty()) {
                throw std::invalid_argument(std::format(
                    "toolset \"{}\" offers a tool with no name, which no call "
                    "could route to", tool_set->name()));
            }
            const auto occupied = _lookup_table.find(name);
            if (occupied != _lookup_table.end()) {
                throw std::runtime_error(std::format(
                    "duplicated tool name: {}, occupied by: {}, offered by: {}",
                    name, occupied->second->name(), tool_set->name()));
            }
        }

        _toolset_list.push_back(std::move(tool_set));
        for (const std::string& name : names) {
            // Names are unique by now, so nothing is overwritten; the set is
            // taken from the list rather than from the moved-from argument.
            _lookup_table.emplace(name, _toolset_list.back());
        }
    }

    /**
     * Unregister the set that provides `name` — the whole set, and with it every
     * other name that set offered.
     *
     * The unit of registration is the set, so the unit of removal is too: taking
     * one name out of a set's catalogue would leave the set half-advertised —
     * its tool still reachable under its other names — and nothing here could
     * decide which half was intended.
     *
     * @return whether anything was removed: false when no set provides `name`.
     *         The sets themselves are only released, not destroyed, if something
     *         else still holds them.
     */
    bool remove(std::string_view name) {
        const auto found = _lookup_table.find(name);
        if (found == _lookup_table.end()) {
            return false;
        }
        const ToolSetPtr removed = found->second;

        _toolset_list.erase(
            std::remove(_toolset_list.begin(), _toolset_list.end(), removed),
            _toolset_list.end());

        for (auto entry = _lookup_table.begin(); entry != _lookup_table.end();) {
            if (entry->second == removed) {
                entry = _lookup_table.erase(entry);
            } else {
                ++entry;
            }
        }
        return true;
    }

    /** Unregister everything. A set still held elsewhere stays alive. */
    void clear() noexcept {
        _lookup_table.clear();
        _toolset_list.clear();
    }

    // ===== inspection ========================================================

    /** How many sets are registered (not how many tools). */
    [[nodiscard]] std::size_t size() const noexcept {
        return _toolset_list.size();
    }

    /** Whether no set is registered at all. */
    [[nodiscard]] bool empty() const noexcept {
        return _toolset_list.empty();
    }

    /** Whether any registered set provides `name`. */
    [[nodiscard]] bool contains(std::string_view name) const {
        return _lookup_table.contains(name);
    }

    /**
     * The set that provides `name`, or nullptr when none does — the same answer
     * execute() turns into a Dispatch failure, for a host that only wants to
     * ask.
     *
     * The name is looked up without being copied into a key (the table hashes
     * string views), so this is cheap enough to call where a call is routed.
     */
    [[nodiscard]] ToolSetPtr find(std::string_view name) const {
        const auto found = _lookup_table.find(name);
        if (found == _lookup_table.end()) {
            return nullptr;
        }
        return found->second;
    }

    /**
     * Every registered set, in registration order.
     *
     * Returned by reference: the caller is looking at the registry's own table
     * and must not hold it across a registration. A host that wants a safe copy
     * of the catalogue wants get_tools() instead.
     */
    [[nodiscard]] const std::vector<ToolSetPtr>& get_registered() const noexcept {
        return _toolset_list;
    }

    /**
     * The flattened catalogue — every tool of every set, set by set in
     * registration order — which is what a request builder hands the model.
     *
     * There is deliberately no lookup table here: the registry routes by name,
     * but the model is shown a list, and a set is free to present its tools in
     * the order it wants them seen.
     */
    [[nodiscard]] std::vector<model_io::Invocable> get_tools() const {
        std::vector<model_io::Invocable> catalogue;
        for (const ToolSetPtr& tool_set : _toolset_list) {
            std::vector<model_io::Invocable> offered = tool_set->get_tools();
            catalogue.insert(catalogue.end(),
                std::make_move_iterator(offered.begin()),
                std::make_move_iterator(offered.end()));
        }
        return catalogue;
    }

    /**
     * Every routable name, in the order get_tools() presents them — the same
     * list, for diagnostics and for a host that checks a name before routing it.
     */
    [[nodiscard]] std::vector<std::string> supported_names() const {
        std::vector<std::string> names;
        for (const ToolSetPtr& tool_set : _toolset_list) {
            std::vector<std::string> offered = tool_set->supported_names();
            names.insert(names.end(),
                std::make_move_iterator(offered.begin()),
                std::make_move_iterator(offered.end()));
        }
        return names;
    }

    // ===== dispatch ==========================================================

    /**
     * Run one batch of calls and answer with one record per call, in the order
     * of the calls. See the file header for the four steps and the failure
     * sizes this returns records for; nothing here throws except on a failure of
     * the machinery itself (an allocation, a stopped executor).
     *
     * @param queries the batch, TAKEN BY VALUE: a coroutine's arguments are
     *        copied into its frame when it is called, whereas a reference would
     *        only borrow the caller's vector — and this awaitable is lazy, so a
     *        borrowed batch would be read after the caller's full-expression
     *        ended (the rule stated at length in endpoint/include/endpoint/
     *        https_stream.hpp, and pinned by test_registry.cpp). The frame also
     *        SETTLES the queries in place, which a const reference could not
     *        express anyway. Move the batch in when the caller is done with it.
     * @param executor the executor the parallel calls run on and the channel
     *        they report through. Pass the host's own, or a strand when the
     *        tools behind it are not thread-safe; the calls this batch spawns are
     *        joined before it returns either way.
     * @return Results, sized and ordered like `queries`. An empty batch is a
     *         legal batch: it answers with an empty vector and spawns nothing.
     *
     * Never suspends while routing — the whole batch is settled first (step 1),
     * so a caller that passes a batch and then awaits it has already had every
     * query's type and security decided. After that it suspends freely: the
     * serial calls are awaited in turn, then the parallel ones are joined.
     */
    [[nodiscard]] boost::asio::awaitable<Results> execute(
        std::vector<model_io::InvokeQuery> queries,
        boost::asio::any_io_executor executor) const
    {
        std::unordered_map<CallKey, model_io::InvokeReturn> records;
        std::vector<CallKey> keys;
        std::vector<PreparedCall> serial_calls;
        std::vector<PreparedCall> parallel_calls;

        // ---- 1. route and settle, synchronously -----------------------------
        //
        // `this` is read while routing and NOWHERE below: the batch holds a
        // shared_ptr per call from here on, so the sets it routed to stay alive
        // for as long as their calls may still run, whatever happens to the
        // registry. (A lazy awaitable is not resumed until its caller awaits it,
        // so the registry does have to outlive that first resume — exactly what
        // `co_await registry.execute(...)` implies.)
        keys.reserve(queries.size());
        for (model_io::InvokeQuery& query : queries) {
            // The identity of the call as it ARRIVED, before any hook can
            // touch it (file header, "identity and filing"). It is kept even
            // though the query is copied into the group below, because a hook
            // may rename what it was handed.
            keys.push_back(query.mangled_name());
            const CallKey& key = keys.back();

            const ToolSetPtr tool_set = find(query.name);
            if (tool_set == nullptr) {
                // Ordinary: a model naming a tool that does not exist is a
                // hallucination, not a fault of this process, and the record
                // tells the model exactly that.
                logging::Logger::debug(
                    "registry: no toolset provides \"{}\"; call {} is answered "
                    "with a dispatch failure", query.name, query.id);
                records[key] = InvokeException(
                    InvokeException::Stage::Dispatch,
                    std::format("no toolset in the registry provides a tool "
                                "named \"{}\"", query.name),
                    query,
                    {}).to_invoke_return();
                continue;
            }

            try {
                ToolHandle tool = tool_set->prepare(query);

                // The SETTLED type picks the schedule, and only an explicit
                // read-only/parallel declaration may overlap its neighbours.
                std::vector<PreparedCall>& group =
                    may_run_in_parallel(query.type) ? parallel_calls : serial_calls;

                // A COPY of the query, not a move: the assembly below reads the
                // batch's own copy when it has to stand in for a call whose
                // record never arrived, and a moved-from query would make that
                // record a lie about which call failed.
                group.push_back(PreparedCall{tool_set, std::move(tool), query, key});
            } catch (const InvokeException& failure) {
                // The documented failure of the settling half: prepare() always
                // throws THIS type, and the exception already IS the record
                // (invoke_exception.hpp), correlated to the call it carries.
                records[key] = failure.to_invoke_return();
            } catch (const std::exception& unexpected) {
                records[key] = broken_set(
                    *tool_set, "while settling the call", unexpected.what(), query);
            } catch (...) {
                records[key] = broken_set(*tool_set, "while settling the call",
                    "an unknown error, not a std::exception", query);
            }
        }

        // ---- 2. serial calls: one at a time, in the order of the calls ------
        for (const PreparedCall& call : serial_calls) {
            // Whatever the set does, this call gets its record: run_settled()
            // turns a set that breaks its execute() contract into one instead of
            // unwinding through the rest of the batch.
            records[call.key] =
                co_await run_settled(call.tool_set, call.tool, call.query);
        }

        // ---- 3. parallel calls: spawned together, then joined ---------------
        if (!parallel_calls.empty()) {
            // Buffered for the whole group, so a branch's report lands without
            // waiting for the collector. With the default zero-capacity channel
            // a sender stays suspended until someone receives — and a batch that
            // was abandoned (a cancelled caller, a shutting-down host) would
            // leave those branches suspended in the send forever, holding the
            // channel, their captures and the toolset alive.
            auto channel = std::make_shared<ReportChannel>(
                executor, parallel_calls.size());

            for (PreparedCall& call : parallel_calls) {
                // The branch owns everything it reads: the call by value, the
                // channel by shared_ptr. These awaitables are lazy, so a
                // reference into this frame would be read after this frame has
                // gone (the rule endpoint/https_stream.hpp states for the tree).
                boost::asio::co_spawn(executor,
                    [call = std::move(call), channel]() -> boost::asio::awaitable<void> {
                        Report report;
                        report.key = call.key;
                        report.record = co_await run_settled(
                            call.tool_set, call.tool, call.query);
                        co_await channel->async_send(
                            boost::system::error_code{},
                            std::optional<Report>{std::move(report)},
                            boost::asio::use_awaitable);
                    },
                    // EXACTLY ONE report per branch, and that is what makes the
                    // collector's fixed number of receives safe. The branch has
                    // one statement that can throw once run_settled() has
                    // returned — the send — and nothing after a send that
                    // SUCCEEDED can throw, so an exception here means the report
                    // was never delivered. (An empty report says so: the
                    // collector stops waiting and the assembly reports the call
                    // it belongs to.)
                    [channel](std::exception_ptr failure) {
                        if (failure == nullptr) {
                            return; // the branch delivered its report
                        }
                        try {
                            std::rethrow_exception(failure);
                        } catch (const std::exception& e) {
                            logging::Logger::error(
                                "registry: a parallel call could not report its "
                                "result: {}", e.what());
                        } catch (...) {
                            logging::Logger::error(
                                "registry: a parallel call could not report its "
                                "result: an unknown error, not a std::exception");
                        }
                        channel->async_send(boost::system::error_code{},
                            std::optional<Report>{}, boost::asio::detached);
                    });
            }

            for (std::size_t arrival = 0; arrival < parallel_calls.size(); ++arrival) {
                std::optional<Report> report =
                    co_await channel->async_receive(boost::asio::use_awaitable);
                if (!report) {
                    // A branch that finished without a record. Which call it
                    // belonged to is exactly what this message no longer says —
                    // the assembly below reports the one that is missing.
                    continue;
                }
                records[std::move(report->key)] = std::move(report->record);
            }
        }

        // ---- 4. assemble, in the order of the calls --------------------------
        Results results;
        results.reserve(queries.size());
        for (std::size_t index = 0; index < queries.size(); ++index) {
            const auto record = records.find(keys[index]);
            if (record != records.end()) {
                // COPIED, not moved: two calls of one batch may share an
                // identity (mangled_name() is name and id), and both are
                // answered with the same record — moving it out on the first
                // lookup would answer the second with a gutted one.
                results.push_back(record->second);
                continue;
            }

            // Unreachable through every path above, and kept anyway because the
            // one thing this layer may never do is answer a call with nothing:
            // the batch would lose a tool message and the provider would reject
            // the turn. The query is the settled one the assembly still holds.
            logging::Logger::error(
                "registry: no record for call {} (tool {}); the batch reported "
                "one report short", queries[index].id, queries[index].name);
            results.push_back(InvokeException(
                InvokeException::Stage::Unknown,
                "the batch produced no record for this call",
                queries[index],
                {}).to_invoke_return());
        }

        co_return results;
    }

    /**
     * The convenience form: the same batch, on the executor of whoever awaits
     * it.
     *
     * A host dispatching from inside a coroutine is already on the executor it
     * wants the batch on — its own strand, its own context — and naming it again
     * is bookkeeping the caller should not have to do. This is the explicit form
     * called with `co_await this_coro::executor`, which is the executor Asio
     * attaches to an awaitable when the caller first resumes it (the
     * `this_coro::executor` form endpoint/create_connection_stream offers for
     * the same reason).
     *
     * Use the explicit form when the batch should run somewhere OTHER than where
     * it was started: off a UI thread, on a pool, through a strand built for the
     * tools rather than for the caller.
     *
     * @param queries the batch, taken by value — see the explicit form.
     * @return the records, exactly as the explicit form returns them.
     */
    [[nodiscard]] boost::asio::awaitable<Results> execute(
        std::vector<model_io::InvokeQuery> queries) const
    {
        const boost::asio::any_io_executor executor =
            co_await boost::asio::this_coro::executor;
        co_return co_await execute(std::move(queries), executor);
    }

private:
    /**
     * One settled call, ready for the schedule: the set it routed to, the tool
     * inside that set, the settled query, and the identity it will be filed
     * under.
     *
     * The pair is kept together on purpose — the tool handle belongs to the set
     * that resolved it, and holding the shared_ptr means the batch keeps the set
     * alive for as long as the call it routed may still run.
     */
    struct PreparedCall {
        ToolSetPtr tool_set;
        ToolHandle tool;
        model_io::InvokeQuery query;
        CallKey key;
    };

    /**
     * What one parallel branch reports back.
     *
     * The key travels WITH the record because the collector files what arrives:
     * a branch whose set answered for a call of its own making (a retry, a
     * nested invocation) must not be able to file that answer under a different
     * call's identity.
     */
    struct Report {
        CallKey key;
        model_io::InvokeReturn record;
    };

    /// The branches' report channel; empty optional = "no record to report".
    using ReportChannel = boost::asio::experimental::concurrent_channel<
        void(boost::system::error_code, std::optional<Report>)>;

    /**
     * The names a batch is keyed by, looked up without materialising a key —
     * the table hashes string views, so `find(query.name)` on a long tool name
     * allocates nothing.
     */
    struct StringHash {
        using is_transparent = void;
        [[nodiscard]] std::size_t operator()(std::string_view text) const noexcept {
            return std::hash<std::string_view>{}(text);
        }
    };

    /// Only these two may overlap their neighbours; anything else — including a
    /// type a later version of the enum adds — is treated as serial, which is
    /// the side that is merely slower rather than the one that races.
    [[nodiscard]] static constexpr bool may_run_in_parallel(
        model_io::InvokeType type) noexcept
    {
        return type == model_io::InvokeType::ReadOnly ||
               type == model_io::InvokeType::ParallWrite;
    }

    /**
     * Run one settled call through its set and return the record, whether the
     * set cooperates or not.
     *
     * ToolSet::execute() is documented never to throw, so anything escaping it
     * is the set breaking its contract — and that must not cost the batch its
     * other answers, nor the caller its await. The query is passed as an lvalue
     * on purpose: the failure path needs it intact to correlate the record, and
     * the record the successful path returns carries its own copy anyway.
     */
    [[nodiscard]] static boost::asio::awaitable<model_io::InvokeReturn> run_settled(
        ToolSetPtr tool_set, ToolHandle tool, model_io::InvokeQuery query)
    {
        try {
            co_return co_await tool_set->execute(std::move(tool), query);
        } catch (const std::exception& unexpected) {
            co_return broken_set(
                *tool_set, "out of execute()", unexpected.what(), query);
        } catch (...) {
            co_return broken_set(*tool_set, "out of execute()",
                "an unknown error, not a std::exception", query);
        }
    }

    /**
     * The record standing in for a call whose SET broke its own contract: an
     * InvokeException where its contract promised InvokeException-only
     * (prepare) or no throw at all (execute).
     *
     * Reported at Stage::Unknown because no checkpoint of the invocation
     * sequence owns a throw that escaped the sequence, and reported by THIS
     * layer — never left to unwind — so one broken set cannot take a batch's
     * other calls down with it.
     */
    [[nodiscard]] static model_io::InvokeReturn broken_set(
        const ToolSet& tool_set,
        std::string_view phase,
        std::string_view what,
        const model_io::InvokeQuery& query)
    {
        return InvokeException(
            InvokeException::Stage::Unknown,
            std::format("toolset \"{}\" broke its contract {}: {}",
                        tool_set.name(), phase, what),
            query,
            {}).to_invoke_return();
    }

    std::vector<ToolSetPtr> _toolset_list;
    std::unordered_map<std::string, ToolSetPtr, StringHash, std::equal_to<>>
        _lookup_table;
};

} // namespace tools
