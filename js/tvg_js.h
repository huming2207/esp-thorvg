/*
 * QuickJS bindings for ThorVG, shaped after @thorvg/webcanvas (thorvg.web).
 *
 * The host owns the engine: call tvg_engine_init() / tvg::Initializer::init()
 * before the module is imported and terminate it after JS_FreeContext().
 * One JSContext at a time can load the module (ThorVG is process global anyway).
 *
 * Render from the task that runs JS. On ESP32-S31 pin that task to core 1 if
 * PIE SIMD is on (see the component README).
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>
#include <esp_err.h>
#include "quickjs.h"
#include "thorvg_capi.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Called on the JS task after Canvas.render() with the regions ThorVG touched.
 * Not called when nothing changed. Slow displays (e-paper) should hand the
 * buffer to their own task here instead of blocking JS for seconds.
 */
typedef void (*tvg_js_present_t)(void *arg, const Tvg_Region *regions, uint32_t cnt);

typedef struct {
    void *buf;                  /* stride * h pixels, caller owned, outlives the canvas */
    uint32_t stride;            /* in pixels */
    uint32_t w, h;
    Tvg_Colorspace cs;          /* ABGR8888/ARGB8888(S), GRAYSCALE8 or RGB565 */
    tvg_js_present_t present;   /* optional */
    void *arg;
} tvg_js_surface_t;

/*
 * Make a framebuffer reachable from JS as `new Canvas(name)`, e.g. "#epd".
 * name: 1..15 chars. The struct is copied. ESP_ERR_NO_MEM when the table
 * (CONFIG_THORVG_JS_MAX_SURFACES) is full, ESP_ERR_INVALID_STATE if taken.
 */
esp_err_t tvg_js_add_surface(const char *name, const tvg_js_surface_t *surf);

/* ESP_ERR_INVALID_STATE while a live Canvas still renders into it */
esp_err_t tvg_js_remove_surface(const char *name);

/* Register the C module, e.g. tvg_js_init_module(ctx, "thorvg") */
JSModuleDef *tvg_js_init_module(JSContext *ctx, const char *name);

#ifdef __cplusplus
}
#endif
