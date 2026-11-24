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

## QuickJS bindings (optional)

`js/` is a QuickJS module with the same classes and calls as
[@thorvg/webcanvas](https://github.com/thorvg/thorvg.web), on ThorVG's C API.
Turn it on with `CONFIG_THORVG_QUICKJS` (menuconfig → ThorVG Support Options).
It needs a QuickJS component in the build; its name is
`CONFIG_THORVG_QUICKJS_COMPONENT` (default `esp-quickjs-ng`). Your app has to
require that component itself; ESP-IDF won't let `REQUIRES` depend on Kconfig.

```c
tvg_engine_init(0);                         // the host owns the engine
tvg_js_surface_t s = { .buf = fb, .stride = 800, .w = 800, .h = 480,
                       .cs = TVG_COLORSPACE_RGB565, .present = flush, .arg = NULL };
tvg_js_add_surface("#lcd", &s);
tvg_js_init_module(ctx, "thorvg");
// ... run JS, JS_FreeContext(), then tvg_js_remove_surface() and tvg_engine_term()
```

```js
import { init } from 'thorvg';
const TVG = await init();
const canvas = new TVG.Canvas('#lcd');      // or new TVG.Canvas(uint32Array, { width, height })
canvas.add(new TVG.Shape().appendRect(0, 0, 100, 100, { rx: 10 }).fill(255, 0, 0));
canvas.render();                            // draws, then calls present() with the dirty regions
```

Differences from webcanvas:

- `Canvas` targets a surface the host registered, or an ArrayBuffer/typed array.
  `renderer`, `threads` and `locateFile` in `init()` are ignored; it's always the
  CPU renderer, and `term()` does nothing (the host terminates the engine).
- A surface registered with `.aliased = true` gives its canvases
  `EngineOption.SmartRender | EngineOption.Aliased` unless JS passes
  `engineOption`. Use it for palette displays (e-paper), where dithering
  anti-aliased edges leaves a comb of teeth along them.
- `render()` only clears the whole target on its first draw. With the default
  engine option ThorVG then redraws just the dirty regions. Clearing every
  frame, as webcanvas does, forces a full redraw (10x slower here).
- No `requestAnimationFrame`: playing animations advance at the start of each
  `Canvas.render()`, so your render loop is the clock. A playing animation
  keeps itself alive until paused, like the rAF closure in a browser. Frames
  advance by elapsed time, so rendering two canvases doesn't speed anything
  up, but `onFrame` fires on every `render()`.
- With `init({ onError })`, a failed ThorVG call is reported and the method
  stops there, returning `this`; nothing after the failed call happens.
- `clip()` only takes a `Shape` (ThorVG would crash on anything else).
  `clip(null)` and `mask(null, MaskMethod.None)` remove them. Anything that
  would make a paint its own child, clipper or mask throws a `RangeError`.
- Gradients are templates: `fill(gradient)` hands ThorVG a copy, so one
  gradient can fill many shapes and stops stay after `fill()`.
- Wrappers hold a ThorVG ref, so dropping the last JS reference to a `Scene`
  frees it and detaches its children. Keep a reference to parents you still need.
- `Scene.add(paint, before?)` and `Canvas.add(paint, before?)` insert in front of
  `before`, which must already be a child (a `RangeError` otherwise: ThorVG would
  leak a ref on `paint`).
- `Layout` (`CONFIG_THORVG_JS_LAYOUT`, on by default) is a [Yoga](https://yogalayout.dev)
  flexbox node from the `yoga/` submodule, with React Native's style names:
  ```js
  const root = new Layout({ width: 800, height: 480, flexDirection: 'row', padding: 16, gap: 8 });
  const label = new Layout({ flexGrow: 1 }, new Text().font('Sans').fontSize(24).text('Hi'));
  root.add(label);          // add(child, before?), remove(child?), style({...}), paint = p
  root.calculate();         // lays out, then translates every bound paint
  label.layout;             // { left, top, width, height }, relative to the parent node
  ```
  Positions are relative to the parent node, so nest the paints the same way (a
  `Scene` per container). A `Text` bound to a leaf is measured by Yoga through
  ThorVG (whole line boxes, `wrap()` honoured) and gets the node size as its
  `layout()` box, so `align()` works inside it; changing its text or font marks
  the node dirty. `calculate()` only touches paints whose layout changed, so an
  unchanged layout leaves no dirty regions. Yoga aborts on a broken invariant
  (no C++ exceptions), so `Layout` throws a JS error before any call that would.
- Extras: `ColorSpace.RGB565/XRGB8888/XBGR8888`, `EngineOption.Aliased`,
  `Picture.loadFile(path)` and `Font.loadFile(path)` (straight from the
  filesystem, no second copy in the JS heap).
- Missing: `Video`, Lottie audio (`volume`, `resolver`), `Picture.resolver()`
  (ThorVG may call it from a loader thread), and `FontsourceProvider`.
  `Font.load()` needs the bytes.
- One JS context at a time can import the module.

Caveats seen on hardware/emulator:

- Turn `CONFIG_THORVG_THREAD_ENABLED` off unless you really use worker
  threads. With it on, every render task carries a FreeRTOS mutex and condvar
  in internal RAM, and a ~250 shape SVG runs an S3 out of internal heap.
- Scene effects (`dropShadow`, `gaussianBlur`, ...) crash on an RGB565 target:
  `tvgSwPostEffect.cpp` assumes 32-bit pixels. They work on 32-bit targets.
  They also allocate square scratch buffers, `max(w, h)^2 * 4` bytes each
  (2.5 MB for 800x480), and ThorVG doesn't check those allocations.

`examples/quickjs` runs the whole API in esp-emu and dumps the final frame
(`QJS_ESP_DIR=/path/to/esp-quickjs-ng idf.py build`).
