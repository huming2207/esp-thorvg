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
#define TVG_MALLOC(sz)       heap_caps_malloc(sz, MALLOC_CAP_SPIRAM)

#ifdef TVG_CALLOC
#  undef TVG_CALLOC
#endif
#define TVG_CALLOC(n, sz)    heap_caps_calloc(n, sz, MALLOC_CAP_SPIRAM)

#ifdef TVG_REALLOC
#  undef TVG_REALLOC
#endif
#define TVG_REALLOC(p, sz)   heap_caps_realloc(p, sz, MALLOC_CAP_SPIRAM)

#ifdef TVG_FREE
#  undef TVG_FREE
#endif
#define TVG_FREE(p)          heap_caps_free(p)

#endif // _TVG_PSRAM_ALLOCATOR_H_
