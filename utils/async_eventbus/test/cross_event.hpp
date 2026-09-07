#pragma once

/**
 * @file cross_event.hpp
 * @brief Event type shared between the cross-DSO host and plugin.
 *
 * Both DSOs include this header, so both instantiate the SAME CrossEvent type.
 * The host executable's -rdynamic export keeps typeid(CrossEvent) identical on
 * both sides, which is what the type_index-routed default_async_bus() relies on
 * to route events across the dlopen boundary (type identity, not the libstdc++
 * name-comparison fallback).
 */

#include "eventbus/async_event_bus.hpp"

namespace async_eventbus_test {

struct CrossEvent : eventbus::AsyncEventBase {
    int value = 0;
};

} // namespace async_eventbus_test
