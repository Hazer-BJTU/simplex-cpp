/**
 * @file default_bus.cpp
 * @brief The one definition of the process-wide default bus.
 *
 * The whole EventBus class stays header-inline; only the singleton is
 * compiled — into the shared eventbus library — because a function-local
 * static in an inline function would be duplicated into every dlopened
 * module (vague-linkage symbols are only unified when the exporting side's
 * symbols are visible, which is a per-host convention, not a mechanism).
 * Living in the shared library, uniqueness is a loader guarantee: every
 * executable and plugin that links `eventbus` binds the same SONAME, hence
 * the same static, hence the same bus. Linking errors (loud) replace
 * silent second-bus failures.
 */

#include "eventbus/event_bus.hpp"

namespace eventbus {

EventBus& default_bus() {
    static EventBus bus;
    return bus;
}

} // namespace eventbus
