# ThorVG component

ESP-IDF component tracking the [ThorVG ESP fork](https://github.com/huming2207/thorvg/tree/jmh/esp)
`jmh/esp`, pinned to `5155c4e5e7a8ee23e738cd29fdb22640357ac794` (version 1.2.0).
Based on upstream `main` at `04eb68aa4c0e4f6d449bdf027983a664d93aadef`,
with custom allocator callbacks and class-scoped allocation.
This is not the [official Espressif port](https://github.com/espressif/idf-extra-components/tree/master/thorvg).
Do not use for production just yet.

Initialize the source with `git submodule update --init --recursive`.
The component builds the CPU renderer and C API using Meson and Ninja;
loaders, logging, and threading are configured through ESP-IDF menuconfig.

## Custom allocation

Pass a `tvg::HeapAllocator` to `tvg::Initializer::init(threads, allocator)` to
route ThorVG allocations through custom callbacks, such as ESP-IDF PSRAM heap
functions. The default callbacks use the standard C heap. The callback type
is now `tvg::HeapAllocator`, rather than `tvg::Initializer::HeapAllocator`;
calls using an inline callback initializer remain supported.

ThorVG objects use class-scoped allocation operators, leaving the application's
global `new`/`delete` operators unchanged. Public value types remain plain data.

## PIE SIMD (experimental)

`CONFIG_THORVG_SIMD_ENABLED` (menuconfig → ThorVG Support Options) builds ThorVG with
`-Dsimd=true`. ThorVG then enables its PIE kernels when ESP-IDF targets the PIE
extension (`-march=..._xespv` on ESP32-P4 rev >= 3 and ESP32-S31, `_xespv2p1` on
ESP32-P4 rev < 3). They speed up solid fills, translucent spans and the Rgb565 target fills.

On ESP32-S31 only core 1 has PIE, so create the rendering task pinned to core 1.
Do not render from an ISR or inside a critical section.
