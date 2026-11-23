/*
 * LinearGradient, RadialGradient, ConicGradient.
 *
 * ThorVG takes ownership of a gradient handed to a shape, which makes a JS
 * object pointing at it a use-after-free waiting to happen. So the JS object
 * keeps a template and fill()/stroke() get a duplicate. Unlike webcanvas a
 * gradient can be reused, and stops stick around after fill().
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include "tvg_js_priv.h"

static bool fill_cls(JSClassID cid)
{
    return cid && (cid == tjs.ids[TJS_LINEAR] || cid == tjs.ids[TJS_RADIAL] || cid == tjs.ids[TJS_CONIC]);
}

bool tjs_is_fill(JSValueConst v)
{
    JSClassID cid;
    JS_GetAnyOpaque(v, &cid);
    return fill_cls(cid);
}

static Tvg_Gradient fill_get(JSContext *ctx, JSValueConst v)
{
    JSClassID cid;
    Tvg_Gradient g = JS_GetAnyOpaque(v, &cid);
    if (!fill_cls(cid)) {
        JS_ThrowTypeError(ctx, "not a gradient");
        return NULL;
    }
    if (!g) {
        JS_ThrowTypeError(ctx, "Object has been disposed");
    }
    return g;
}

Tvg_Gradient tjs_fill_dup(JSContext *ctx, JSValueConst v)
{
    Tvg_Gradient g = fill_get(ctx, v);
    if (!g) {
        return NULL;
    }
    Tvg_Gradient d = tvg_gradient_duplicate(g);
    if (!d) {
        JS_ThrowOutOfMemory(ctx);
    }
    return d;
}

static void fill_finalizer(JSRuntime *rt, JSValueConst val)
{
    JSClassID cid;
    Tvg_Gradient g = JS_GetAnyOpaque(val, &cid);
    if (g) {
        tvg_gradient_del(g);
    }
}

/* magic: class. Args per webcanvas: (x1, y1, x2, y2), (cx, cy, r, fx?, fy?, fr?), (cx, cy, angle?) */
static JSValue fill_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv, int magic)
{
    float v[6] = { 0, 0, 0, 0, 0, 0 };
    if (tjs_nums(ctx, v, 6, argc, argv)) {
        return JS_EXCEPTION;
    }
    JSValue obj = tjs_new_obj(ctx, new_target, magic);
    if (JS_IsException(obj)) {
        return obj;
    }
    Tvg_Gradient g = magic == TJS_LINEAR ? tvg_linear_gradient_new()
                   : magic == TJS_RADIAL ? tvg_radial_gradient_new()
                   : tvg_conic_gradient_new();
    if (!g) {
        JS_FreeValue(ctx, obj);
        return JS_ThrowOutOfMemory(ctx);
    }
    JS_SetOpaque(obj, g);

    if (magic == TJS_LINEAR) {
        tvg_linear_gradient_set(g, v[0], v[1], v[2], v[3]);
    } else if (magic == TJS_RADIAL) {
        /* focal point defaults to the centre */
        if (argc < 4 || JS_IsUndefined(argv[3])) v[3] = v[0];
        if (argc < 5 || JS_IsUndefined(argv[4])) v[4] = v[1];
        tvg_radial_gradient_set(g, v[0], v[1], v[2], v[3], v[4], v[5]);
    } else {
        tvg_conic_gradient_set(g, v[0], v[1], v[2]);
    }
    return obj;
}

/*
 * Appends to (or with keep = false, replaces) the stops. ThorVG reallocs its
 * array on set, so the current stops are copied out first.
 */
static int push_stop(JSContext *ctx, Tvg_Gradient g, Tvg_Color_Stop *s, uint32_t *n,
                     JSValueConst off, JSValueConst color)
{
    float o = 0;
    int32_t c[4];
    if (*n >= TJS_MAX_STOPS) {
        JS_ThrowRangeError(ctx, "more than %d gradient stops", TJS_MAX_STOPS);
        return -1;
    }
    if (tjs_nums(ctx, &o, 1, 1, &off) || tjs_color(ctx, color, c)) {
        return -1;
    }
    s[*n] = (Tvg_Color_Stop){ o, tjs_u8(c[0]), tjs_u8(c[1]), tjs_u8(c[2]), tjs_u8(c[3]) };
    (*n)++;
    return 0;
}

static JSValue fill_add_stop(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    Tvg_Gradient g = fill_get(ctx, this_val);
    if (!g) {
        return JS_EXCEPTION;
    }
    Tvg_Color_Stop s[TJS_MAX_STOPS];
    const Tvg_Color_Stop *cur = NULL;
    uint32_t n = 0;
    tvg_gradient_get_color_stops(g, &cur, &n);
    n = n > TJS_MAX_STOPS ? TJS_MAX_STOPS : n;
    if (n) {
        memcpy(s, cur, n * sizeof(*s));
    }
    if (push_stop(ctx, g, s, &n, argc > 0 ? argv[0] : JS_UNDEFINED, argc > 1 ? argv[1] : JS_UNDEFINED)) {
        return JS_EXCEPTION;
    }
    TJS_TRY(ctx, tvg_gradient_set_color_stops(g, s, n), "addStop");
    return tjs_this(ctx, this_val);
}

/* setStops(...[offset, color]) */
static JSValue fill_set_stops(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    Tvg_Gradient g = fill_get(ctx, this_val);
    if (!g) {
        return JS_EXCEPTION;
    }
    Tvg_Color_Stop s[TJS_MAX_STOPS];
    uint32_t n = 0;
    for (int i = 0; i < argc; i++) {
        JSValue off = JS_GetPropertyUint32(ctx, argv[i], 0);
        JSValue col = JS_IsException(off) ? JS_UNDEFINED : JS_GetPropertyUint32(ctx, argv[i], 1);
        int bad = JS_IsException(off) || JS_IsException(col) || push_stop(ctx, g, s, &n, off, col);
        JS_FreeValue(ctx, off);
        JS_FreeValue(ctx, col);
        if (bad) {
            return JS_EXCEPTION;
        }
    }
    TJS_TRY(ctx, tvg_gradient_set_color_stops(g, n ? s : NULL, n), "setStops");
    return tjs_this(ctx, this_val);
}

static JSValue fill_clear_stops(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    Tvg_Gradient g = fill_get(ctx, this_val);
    if (!g) {
        return JS_EXCEPTION;
    }
    TJS_TRY(ctx, tvg_gradient_set_color_stops(g, NULL, 0), "clearStops");
    return tjs_this(ctx, this_val);
}

static JSValue fill_spread(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    Tvg_Gradient g = fill_get(ctx, this_val);
    int32_t s = 0;
    if (!g || tjs_ints(ctx, &s, 1, argc, argv)) {
        return JS_EXCEPTION;
    }
    TJS_TRY(ctx, tvg_gradient_set_spread(g, s), "spread");
    return tjs_this(ctx, this_val);
}

/* webcanvas applies queued stops here; ours are applied as they come */
static JSValue fill_build(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (!fill_get(ctx, this_val)) {
        return JS_EXCEPTION;
    }
    return tjs_this(ctx, this_val);
}

static JSValue fill_dispose(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSClassID cid;
    Tvg_Gradient g = JS_GetAnyOpaque(this_val, &cid);
    if (fill_cls(cid) && g) {
        tvg_gradient_del(g);
        JS_SetOpaque(this_val, NULL);
    }
    return JS_UNDEFINED;
}

static JSValue fill_disposed(JSContext *ctx, JSValueConst this_val)
{
    JSClassID cid;
    Tvg_Gradient g = JS_GetAnyOpaque(this_val, &cid);
    return JS_NewBool(ctx, !fill_cls(cid) || !g);
}

static const JSCFunctionListEntry fill_proto[] = {
    JS_CFUNC_DEF("addStop", 2, fill_add_stop),
    JS_CFUNC_DEF("clearStops", 0, fill_clear_stops),
    JS_CFUNC_DEF("setStops", 0, fill_set_stops),
    JS_CFUNC_DEF("spread", 1, fill_spread),
    JS_CFUNC_DEF("build", 0, fill_build),
    JS_CFUNC_DEF("dispose", 0, fill_dispose),
    JS_CGETSET_DEF("isDisposed", fill_disposed, NULL),
};

/* the gradients add nothing over Fill.prototype */
static const JSCFunctionListEntry no_fns[] = {};

int tjs_init_fills(JSContext *ctx, JSValueConst ns)
{
    static const tjs_class_t base = TJS_CLASS("Fill", -1, NULL, 0, fill_proto, NULL, NULL);
    static const tjs_class_t defs[] = {
        TJS_CLASS("LinearGradient", TJS_LINEAR, fill_ctor, 4, no_fns, fill_finalizer, NULL),
        TJS_CLASS("RadialGradient", TJS_RADIAL, fill_ctor, 6, no_fns, fill_finalizer, NULL),
        TJS_CLASS("ConicGradient", TJS_CONIC, fill_ctor, 3, no_fns, fill_finalizer, NULL),
    };
    JSValue fill = tjs_def_class(ctx, &base, JS_UNDEFINED);
    if (JS_IsException(fill)) {
        return -1;
    }
    for (size_t i = 0; i < countof(defs); i++) {
        JSValue c = tjs_def_class(ctx, &defs[i], fill);
        if (JS_IsException(c)) {
            JS_FreeValue(ctx, fill);
            return -1;
        }
        JS_SetPropertyStr(ctx, ns, defs[i].name, c);
    }
    JS_FreeValue(ctx, fill); /* webcanvas doesn't export Fill */
    return 0;
}
