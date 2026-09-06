/**
 * @file default_async_bus.cpp
 * @brief The one definition of the process-wide default async bus.
 *
 * The whole AsyncEventBus class stays header-inline; only the singleton is
 * compiled — into the shared async_eventbus library — for the same reason
 * default_bus() lives in eventbus_lib: a function-local static in an inline
 * function would fork into a private copy inside every dlopened module and
 * events would vanish at the boundary. Living here, uniqueness is structural:
 * every executable and plugin that links `async_eventbus` binds the same
 * SONAME, hence the same static, hence the same bus. Linking errors (loud)
 * replace silent second-bus failures.
 */

#include "eventbus/async_event_bus.hpp"

namespace eventbus {

AsyncEventBus& default_async_bus() {
    static AsyncEventBus bus;
    return bus;
}

} // namespace eventbus
