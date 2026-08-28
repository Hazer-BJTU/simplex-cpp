/**
 * @file default_bus.cpp
 * @brief The one definition of the process-wide default bus.
 *
 * The whole EventBus class stays header-inline; only the singleton is
 * compiled — into the shared eventbus library — because a function-local
 * static in an inline function would be duplicated into every dlopened
 * module (vague-linkage symbols unify only when the exporting side's symbols
 * are visible; for the host-executable side that visibility is now a
 * mechanism — every dlopening executable is built with ENABLE_EXPORTS, see
 * docs/abi-context.md — and for library-defined symbols the shared SONAME
 * binding is the loader's guarantee). Living here, uniqueness is structural:
 * every executable and plugin that links `eventbus` binds the same SONAME,
 * hence the same static, hence the same bus. Linking errors (loud) replace
 * silent second-bus failures.
 */

#include "eventbus/event_bus.hpp"

namespace eventbus {

EventBus& default_bus() {
    static EventBus bus;
    return bus;
}

} // namespace eventbus
