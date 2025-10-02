/*
 * This is a custom allocator for ThorVG on ESP32.
 * It overrides the default memory allocation functions (malloc, calloc, etc.)
 * to use PSRAM (SPIRAM) via the ESP-IDF heap capabilities API.
 *
 * To use this file, pass its path to the meson build configuration:
 * meson build -Dcustom_allocator_header=/path/to/this/file/psram_allocator.h
 */

#ifndef _TVG_PSRAM_ALLOCATOR_H_
#define _TVG_PSRAM_ALLOCATOR_H_

#include <esp_heap_caps.h>

// Undefine any previous definitions to be safe, then define the custom allocators.
#ifdef TVG_MALLOC
#  undef TVG_MALLOC
#endif
#define TVG_MALLOC(sz)       heap_caps_malloc(sz, (MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA))

#ifdef TVG_CALLOC
#  undef TVG_CALLOC
#endif
#define TVG_CALLOC(n, sz)    heap_caps_calloc(n, sz, (MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA))

#ifdef TVG_REALLOC
#  undef TVG_REALLOC
#endif
#define TVG_REALLOC(p, sz)   heap_caps_realloc(p, sz, (MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA))

#ifdef TVG_FREE
#  undef TVG_FREE
#endif
#define TVG_FREE(p)          heap_caps_free(p)

// This is necessary, because somehow Espressif's toolchain has stricter type checks on std::min and std::max
#include <algorithm>
#include <type_traits>

namespace std {
    template <typename A, typename B>
    constexpr auto min(A a, B b) -> typename std::common_type<A, B>::type {
        using T = typename std::common_type<A, B>::type;
        return (a < b) ? static_cast<T>(a) : static_cast<T>(b);
    }

    template <typename A, typename B>
    constexpr auto max(A a, B b) -> typename std::common_type<A, B>::type {
        using T = typename std::common_type<A, B>::type;
        return (a > b) ? static_cast<T>(a) : static_cast<T>(b);
    }
}

#endif // _TVG_PSRAM_ALLOCATOR_H_
