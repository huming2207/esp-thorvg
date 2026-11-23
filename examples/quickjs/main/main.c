/*
 * ThorVG from JS: registers an 800x480 RGB565 framebuffer as "#lcd", runs
 * main.js, then dumps the last frame (2x downscaled, base64) so a host script
 * can turn it into a PNG.
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "quickjs-libc.h"
#include "qjs_esp.h"
#include "tvg_js.h"

#define W 800
#define H 480

static const char *TAG = "tvg_qjs";
static uint16_t *fb;
static uint32_t presents;

extern const char main_js[] asm("_binary_main_js_start");
extern const char main_js_end[] asm("_binary_main_js_end");

#define ASSET(sym) \
    extern const uint8_t sym##_start[] asm("_binary_" #sym "_start"); \
    extern const uint8_t sym##_end[] asm("_binary_" #sym "_end")
ASSET(test0_svg);
ASSET(test_lot);
ASSET(segment_lot);
ASSET(slot_lot);
ASSET(PublicSans_Regular_ttf);

static const struct { const char *name; const uint8_t *start, *end; } assets[] = {
    { "svg", test0_svg_start, test0_svg_end },
    { "lottie", test_lot_start, test_lot_end },
    { "segment", segment_lot_start, segment_lot_end },
    { "slot", slot_lot_start, slot_lot_end },
    { "font", PublicSans_Regular_ttf_start, PublicSans_Regular_ttf_end },
};

/* asset(name) -> Uint8Array copy of an embedded file */
static JSValue js_asset(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) {
        return JS_EXCEPTION;
    }
    for (size_t i = 0; i < sizeof(assets) / sizeof(assets[0]); i++) {
        if (!strcmp(name, assets[i].name)) {
            JS_FreeCString(ctx, name);
            return JS_NewUint8ArrayCopy(ctx, assets[i].start, assets[i].end - assets[i].start);
        }
    }
    JS_FreeCString(ctx, name);
    return JS_ThrowReferenceError(ctx, "no asset");
}

static void present(void *arg, const Tvg_Region *r, uint32_t cnt)
{
    if (presents++ < 4) {
        ESP_LOGI(TAG, "present #%u: %u region(s), first %ld,%ld..%ld,%ld", (unsigned)presents, (unsigned)cnt,
                 (long)r[0].xMin, (long)r[0].yMin, (long)r[0].xMax, (long)r[0].yMax);
    }
}

/* every other pixel of every other row, as RGB565 little endian */
static void dump_fb(void)
{
    static uint16_t row[W / 2];
    static unsigned char out[W / 2 * 2 * 4 / 3 + 8];
    printf("-----BEGIN FB %d %d-----\n", W / 2, H / 2);
    for (int y = 0; y < H; y += 2) {
        for (int x = 0; x < W / 2; x++) {
            row[x] = fb[y * W + x * 2];
        }
        size_t n;
        mbedtls_base64_encode(out, sizeof(out), &n, (const unsigned char *)row, sizeof(row));
        printf("%.*s\n", (int)n, out);
    }
    printf("-----END FB-----\n");
}

static void js_task(void *arg)
{
    size_t heap0 = esp_get_free_heap_size();
    tvg_engine_init(0);
    tvg_js_surface_t s = {
        .buf = fb, .stride = W, .w = W, .h = H, .cs = TVG_COLORSPACE_RGB565, .present = present,
    };
    ESP_ERROR_CHECK(tvg_js_add_surface("#lcd", &s));

    JSRuntime *rt = qjs_esp_new_runtime();
    JS_SetMemoryLimit(rt, 4 * 1024 * 1024);
    js_std_init_handlers(rt);
    JSContext *ctx = JS_NewContext(rt);
    js_std_add_helpers(ctx, 0, NULL);
    js_init_module_std(ctx, "qjs:std");
    js_init_module_os(ctx, "qjs:os");
    tvg_js_init_module(ctx, "thorvg");
    qjs_esp_loop_t *loop = qjs_esp_loop_new(ctx);

    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "asset", JS_NewCFunction(ctx, js_asset, "asset", 1));
    JS_FreeValue(ctx, global);

    JSValue ret = JS_Eval(ctx, main_js, main_js_end - main_js - 1, "main.js", JS_EVAL_TYPE_MODULE);
    if (JS_IsException(ret)) {
        js_std_dump_error(ctx);
    } else {
        JS_FreeValue(ctx, ret);
        qjs_esp_loop_run(loop);
    }
    qjs_esp_loop_free(loop);
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    /* every canvas is gone with the context, so this must succeed */
    ESP_ERROR_CHECK(tvg_js_remove_surface("#lcd"));
    tvg_engine_term();
    /* fonts stay cached in ThorVG until term, so this is the whole lifetime */
    ESP_LOGI(TAG, "presents: %u, heap lost over the run: %d bytes", (unsigned)presents,
             (int)(heap0 - esp_get_free_heap_size()));
    dump_fb();
    printf("ALL DONE\n");
    vTaskDelete(NULL);
}

void app_main(void)
{
    fb = heap_caps_calloc(W * H, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    assert(fb);
#if CONFIG_FREERTOS_UNICORE
    const BaseType_t core = 0;
#else
    const BaseType_t core = 1; /* S31: PIE only on core 1 */
#endif
    xTaskCreatePinnedToCore(js_task, "js", 32 * 1024, NULL, 5, NULL, core);
}
