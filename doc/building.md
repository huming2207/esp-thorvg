# Building

## Get the sources

Two submodules: `thorvg/` (our ThorVG fork) and `yoga/` (layout, only used by the JS binding).

```sh
git submodule update --init --recursive
```

ThorVG builds with Meson + Ninja through an ExternalProject. If `meson` isn't on your PATH, the
build tries `pip install meson` into the IDF Python env. If that fails, install it yourself.

## Kconfig

All under `menuconfig → ThorVG Support Options`.

| Option | Default | What it does |
|---|---|---|
| `THORVG_LOG_ENABLED` | n | ThorVG's own log output. Noisy, useful when a load fails for no visible reason. |
| `THORVG_THREAD_ENABLED` | y | Worker threads. **Turn it off** unless you actually want them, see below. |
| `THORVG_SIMD_ENABLED` | y | PIE SIMD kernels (experimental). |
| `THORVG_*_LOADER_SUPPORT` | svg, lottie, ttf on; png, jpg, webp off | Which file formats `Picture`/`Font` can load. Every loader costs flash. |
| `THORVG_QUICKJS` | n | The JS binding in `js/`. |
| `THORVG_QUICKJS_COMPONENT` | `esp-quickjs-ng` | Name of the component that provides `quickjs.h`. |
| `THORVG_JS_LAYOUT` | y | `Layout` (Yoga). About 33 KB of flash. |
| `THORVG_JS_MAX_SURFACES` | 2 | How many framebuffers the host can register for JS at once. |

## Threads: just turn them off

With `THORVG_THREAD_ENABLED=y` every render task carries a FreeRTOS mutex and condvar, and those
live in internal RAM. A ~250 shape SVG ran an ESP32-S3 out of internal heap. Unless you've
measured that threads help, set it to `n` and call `tvg_engine_init(0)`.

## Allocator

ThorVG takes allocator callbacks at init, so you can push all of its memory into PSRAM:

```cpp
tvg::Initializer::init(0, {
    .alloc = [](size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); },
    .calloc = [](size_t n, size_t s) { return heap_caps_calloc(n, s, MALLOC_CAP_SPIRAM); },
    .realloc = [](void *p, size_t n) { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); },
    .free = heap_caps_free,
});
```

The callback type is `tvg::HeapAllocator`. ThorVG classes have their own `operator new`, so your
app's global `new`/`delete` stay untouched.

## PIE SIMD

`THORVG_SIMD_ENABLED` builds ThorVG with `-Dsimd=true`. It picks up PIE from the `-march` IDF
passes (`..._xespv` on ESP32-P4 rev >= 3 and ESP32-S31, `..._xespv2p1` on older P4). Speeds up
solid fills, translucent spans and RGB565 fills.

**ESP32-S31: only core 1 has PIE.** Pin whatever task renders to core 1, or it faults. Never render
from an ISR or inside a critical section.

## JS binding requirements

Turning on `THORVG_QUICKJS` isn't enough by itself: your app component has to `REQUIRES` the
QuickJS component too (`esp-quickjs-ng` by default). IDF doesn't let a component's requirements
depend on Kconfig, so the build checks for it and stops with an error if it's missing.

Next: [js-host-setup.md](js-host-setup.md).
