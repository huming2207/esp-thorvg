# JS binding: the C side

Everything here is in `js/tvg_js.h`. The host (your firmware) owns the ThorVG engine and the
framebuffers; JS just draws into them.

## The minimum

```c
#include "quickjs-libc.h"
#include "qjs_esp.h"
#include "tvg_js.h"

tvg_engine_init(0);     // you start the engine, not JS

tvg_js_surface_t s = {
    .buf = fb, .stride = 800, .w = 800, .h = 480,
    .cs = TVG_COLORSPACE_RGB565,
};
tvg_js_add_surface("#lcd", &s);

JSRuntime *rt = qjs_esp_new_runtime();
js_std_init_handlers(rt);
JSContext *ctx = JS_NewContext(rt);
js_std_add_helpers(ctx, 0, NULL);
js_init_module_os(ctx, "qjs:os");
tvg_js_init_module(ctx, "thorvg");      // any name you like, that's what JS imports

// ... eval your script, run the loop ...

JS_FreeContext(ctx);
JS_FreeRuntime(rt);
tvg_js_remove_surface("#lcd");          // after the context: canvases hold the surface
tvg_engine_term();
```

And in JS:

```js
import { init } from 'thorvg';
const TVG = await init();
const canvas = new TVG.Canvas('#lcd');
```

Run all of it on one task. On ESP32-S31 with SIMD on, that task has to be on core 1.

## Surfaces

A surface is a named framebuffer JS can render into with `new Canvas(name)`.

```c
typedef struct {
    void *buf;                  // stride * h pixels, you own it, it must outlive the canvas
    uint32_t stride;            // in pixels, not bytes
    uint32_t w, h;
    Tvg_Colorspace cs;          // ABGR8888/ARGB8888(S), GRAYSCALE8 or RGB565
    tvg_js_present_t present;   // optional, see below
    void *arg;                  // passed to present
    bool aliased;               // no anti-aliasing by default, see e-paper.md
} tvg_js_surface_t;
```

- `tvg_js_add_surface(name, &s)`: name is 1..15 chars and the struct is copied. You get
  `ESP_ERR_NO_MEM` when all `THORVG_JS_MAX_SURFACES` slots are taken, `ESP_ERR_INVALID_STATE` if
  the name is already in use.
- `tvg_js_remove_surface(name)` fails with `ESP_ERR_INVALID_STATE` while a live `Canvas` still
  renders into it. Free the context first.
- One canvas per surface at a time.

### present()

```c
void present(void *arg, const Tvg_Region *regions, uint32_t cnt);
```

Called on the JS task right after `Canvas.render()`, with the same dirty regions `render()`
returns to JS. Not called when nothing changed. It runs on the JS task, so if your display is slow
(e-paper: 20 s), don't do the transfer in there. Hand it off to another task, or skip `present`
entirely and let JS decide when to push a frame (that's what inkframe does).

## One context at a time

The binding is process-global (ThorVG's fonts and loaders are global anyway). A second context
trying to import it gets an error until the first one is gone.

## Killed scripts: `tvg_js_abandon()`

Normal teardown (`JS_FreeContext` + `JS_FreeRuntime`) runs every finalizer, and every ThorVG/Yoga
object gets freed. Check with `tvg_js_live_objects()`: it should be 0 afterwards.

If you run untrusted or buggy scripts with esp-quickjs-ng's arena and limits, a stopped script's
runtime can't be freed normally: you drop the whole arena with `qjs_esp_arena_reset()`, and
no finalizers run. The ThorVG paints, canvases and Yoga nodes the script made live outside that
arena, so they'd leak, and the binding would still think it's bound to a dead context. Call
`tvg_js_abandon()` after the reset:

```c
qjs_esp_stop_t why = qjs_esp_loop_run(loop);
qjs_esp_loop_free(loop);
if (why == QJS_ESP_STOP_IDLE) {
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
} else {
    tvg_js_abandon();               // frees what JS held, unbinds, frees the surfaces
    qjs_esp_arena_reset(arena);
}
tvg_js_remove_surface("#lcd");
```

Never call it while the runtime is still alive: it frees things live JS objects point at. Loaded
fonts survive it.
