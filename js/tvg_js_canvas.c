/*
 * Canvas, Font, Accessor, and the surface table.
 *
 * webcanvas targets an HTML <canvas> by selector. Here the target is either a
 * surface name the host registered with tvg_js_add_surface() (the display's
 * framebuffer), or an ArrayBuffer / typed array for offscreen rendering.
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include <freertos/FreeRTOS.h>
#include "sdkconfig.h"
#include "tvg_js_priv.h"

/* ---- surfaces ---- */

typedef struct {
    char name[16];
    tvg_js_surface_t s;
    int users;          /* live canvases */
} surface_slot_t;

static surface_slot_t surfaces[CONFIG_THORVG_JS_MAX_SURFACES];
static portMUX_TYPE surf_lock = portMUX_INITIALIZER_UNLOCKED;

static int bpp(Tvg_Colorspace cs)
{
    return cs == TVG_COLORSPACE_GRAYSCALE8 ? 1 : cs == TVG_COLORSPACE_RGB565 ? 2 : 4;
}

esp_err_t tvg_js_add_surface(const char *name, const tvg_js_surface_t *surf)
{
    if (!name || !name[0] || !surf || !surf->buf || !surf->w || !surf->h || surf->stride < surf->w ||
        strlen(name) >= sizeof(surfaces[0].name)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ESP_OK;
    int slot = -1;
    portENTER_CRITICAL(&surf_lock);
    for (int i = 0; i < CONFIG_THORVG_JS_MAX_SURFACES; i++) {
        if (!strcmp(surfaces[i].name, name)) {
            ret = ESP_ERR_INVALID_STATE;
        } else if (!surfaces[i].name[0] && slot < 0) {
            slot = i;
        }
    }
    if (ret == ESP_OK && slot < 0) {
        ret = ESP_ERR_NO_MEM;
    }
    if (ret == ESP_OK) {
        strcpy(surfaces[slot].name, name);
        surfaces[slot].s = *surf;
        surfaces[slot].users = 0;
    }
    portEXIT_CRITICAL(&surf_lock);
    return ret;
}

esp_err_t tvg_js_remove_surface(const char *name)
{
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    portENTER_CRITICAL(&surf_lock);
    for (int i = 0; i < CONFIG_THORVG_JS_MAX_SURFACES; i++) {
        if (name && !strcmp(surfaces[i].name, name)) {
            ret = surfaces[i].users ? ESP_ERR_INVALID_STATE : ESP_OK;
            if (ret == ESP_OK) {
                surfaces[i].name[0] = 0;
            }
            break;
        }
    }
    portEXIT_CRITICAL(&surf_lock);
    return ret;
}

static int surface_take(const char *name)
{
    int ret = -1;
    portENTER_CRITICAL(&surf_lock);
    for (int i = 0; i < CONFIG_THORVG_JS_MAX_SURFACES; i++) {
        if (surfaces[i].name[0] && !strcmp(surfaces[i].name, name)) {
            surfaces[i].users++;
            ret = i;
            break;
        }
    }
    portEXIT_CRITICAL(&surf_lock);
    return ret;
}

static void surface_drop(int i)
{
    portENTER_CRITICAL(&surf_lock);
    surfaces[i].users--;
    portEXIT_CRITICAL(&surf_lock);
}

/* ---- Canvas ---- */

typedef struct {
    Tvg_Canvas c;       /* NULL once destroyed */
    int surf;           /* surface slot, or -1 for a buffer target */
    JSValue buf;        /* ArrayBuffer target, kept alive */
    size_t off;         /* byte offset of the view into it */
    size_t view_len;    /* bytes the view covers; SIZE_MAX for a bare ArrayBuffer */
    uint8_t *mem;       /* what ThorVG currently draws into */
    uint32_t w, h, stride;
    Tvg_Colorspace cs;
    bool smart;         /* ThorVG does partial redraws */
    bool fresh;         /* new target, next draw clears it all */
} canvas_t;

static canvas_t *canvas_get(JSContext *ctx, JSValueConst v)
{
    canvas_t *c = JS_GetOpaque2(ctx, v, tjs.ids[TJS_CANVAS]);
    if (c && !c->c) {
        JS_ThrowTypeError(ctx, "Canvas has been destroyed");
        return NULL;
    }
    return c;
}

#define CANVAS() \
    canvas_t *c = canvas_get(ctx, this_val); \
    if (!c) return JS_EXCEPTION

static void canvas_release(JSRuntime *rt, canvas_t *c)
{
    if (c->c) {
        tvg_canvas_destroy(c->c);
        c->c = NULL;
    }
    if (c->surf >= 0) {
        surface_drop(c->surf);
        c->surf = -1;
    }
    JS_FreeValueRT(rt, c->buf);
    c->buf = JS_UNDEFINED;
}

static void canvas_finalizer(JSRuntime *rt, JSValueConst val)
{
    JSClassID cid;
    canvas_t *c = JS_GetAnyOpaque(val, &cid);
    if (c) {
        canvas_release(rt, c);
        js_free_rt(rt, c);
    }
}

static void canvas_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark)
{
    JSClassID cid;
    canvas_t *c = JS_GetAnyOpaque(val, &cid);
    if (c) {
        JS_MarkValue(rt, c->buf, mark);
    }
}

/*
 * Point ThorVG at the right memory. Buffer targets get re-checked every time:
 * JS can detach (transfer) or resize an ArrayBuffer behind our back.
 * 0 ok, -1 thrown, 1 ThorVG refused and onError handled it.
 */
static int retarget(JSContext *ctx, canvas_t *c, bool force)
{
    uint8_t *mem;
    if (c->surf >= 0) {
        mem = surfaces[c->surf].s.buf;
    } else {
        size_t len;
        uint8_t *base = JS_GetArrayBuffer(ctx, &len, c->buf);
        if (JS_HasException(ctx)) {
            return -1;      /* detached */
        }
        /* what's left past the view's start, but no more than the view itself */
        size_t avail = len < c->off ? 0 : len - c->off;
        avail = avail < c->view_len ? avail : c->view_len;
        /* 64-bit: with a 32-bit size_t, 32768 x 32768 x 4 is 0 and would pass */
        uint64_t need = (uint64_t)c->stride * c->h * bpp(c->cs);
        if (!base || avail < need) {
            JS_ThrowRangeError(ctx, "Canvas: target buffer too small (%u < %llu bytes)",
                               (unsigned)avail, (unsigned long long)need);
            return -1;
        }
        mem = base + c->off;
        if ((uintptr_t)mem % bpp(c->cs)) {
            JS_ThrowRangeError(ctx, "Canvas: target buffer misaligned");
            return -1;
        }
    }
    if (!force && mem == c->mem) {
        return 0;
    }
    Tvg_Result r = tvg_swcanvas_set_target(c->c, (uint32_t *)mem, c->stride, c->w, c->h, c->cs);
    if (r != TVG_RESULT_SUCCESS) {
        c->mem = NULL;  /* next render tries again */
        return tjs_fail(ctx, r, "target") ? -1 : 1;
    }
    c->mem = mem;
    c->fresh = true;
    return 0;
}

/*
 * new Canvas(target, { width, height, engineOption, colorSpace })
 * target: registered surface name, or ArrayBuffer / typed array.
 * Surfaces default to their full size; buffers to 800x600 ABGR8888 like webcanvas.
 */
static JSValue canvas_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv, int magic)
{
    JSValueConst target = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValueConst opts = argc > 1 ? argv[1] : JS_UNDEFINED;
    int32_t w = 800, h = 600, eo = TVG_ENGINE_OPTION_DEFAULT, cs = TVG_COLORSPACE_ABGR8888;
    int surf = -1;

    if (JS_IsString(target)) {
        const char *name = JS_ToCString(ctx, target);
        if (!name) {
            return JS_EXCEPTION;
        }
        surf = surface_take(name);
        if (surf < 0) {
            JS_ThrowReferenceError(ctx, "Canvas: no surface called '%s'", name);
            JS_FreeCString(ctx, name);
            return JS_EXCEPTION;
        }
        JS_FreeCString(ctx, name);
        w = surfaces[surf].s.w;
        h = surfaces[surf].s.h;
        cs = surfaces[surf].s.cs;
    }

    canvas_t *c = NULL;
    JSValue obj = JS_UNDEFINED;
    if (tjs_opt_int(ctx, opts, "width", &w) || tjs_opt_int(ctx, opts, "height", &h) ||
        tjs_opt_int(ctx, opts, "engineOption", &eo) || (surf < 0 && tjs_opt_int(ctx, opts, "colorSpace", &cs))) {
        goto fail;
    }
    if (w <= 0 || h <= 0 || (surf >= 0 && ((uint32_t)w > surfaces[surf].s.w || (uint32_t)h > surfaces[surf].s.h))) {
        JS_ThrowRangeError(ctx, "Canvas: bad size %dx%d", (int)w, (int)h);
        goto fail;
    }

    obj = tjs_new_obj(ctx, new_target, TJS_CANVAS);
    if (JS_IsException(obj) || !(c = js_mallocz(ctx, sizeof(*c)))) {
        goto fail;
    }
    c->surf = surf;
    c->buf = JS_UNDEFINED;
    c->w = w;
    c->h = h;
    c->cs = cs;
    c->stride = surf >= 0 ? surfaces[surf].s.stride : (uint32_t)w;
    /* same test ThorVG uses to turn on its dirty region tracking */
    c->smart = eo == TVG_ENGINE_OPTION_DEFAULT || (eo & TVG_ENGINE_OPTION_SMART_RENDER);
    JS_SetOpaque(obj, c);
    surf = -1; /* c owns it now */

    if (c->surf < 0) {
        size_t len, bpe;
        len = SIZE_MAX;
        c->buf = JS_IsArrayBuffer(target) ? JS_DupValue(ctx, target)
                                          : JS_GetTypedArrayBuffer(ctx, target, &c->off, &len, &bpe);
        if (JS_IsException(c->buf)) {
            c->buf = JS_UNDEFINED;
            goto fail;
        }
        c->view_len = len;  /* a view can't hand ThorVG the rest of its buffer */
    }
    c->c = tvg_swcanvas_create(eo);
    if (!c->c) {
        JS_ThrowInternalError(ctx, "Canvas: tvg_swcanvas_create failed, engine not initialised?");
        goto fail;
    }
    if (retarget(ctx, c, true) < 0) {
        goto fail;
    }
    return obj;     /* a target ThorVG refused (handled by onError) is retried on render */

fail:
    if (surf >= 0) {
        surface_drop(surf);
    }
    JS_FreeValue(ctx, obj); /* finalizer cleans c */
    return JS_EXCEPTION;
}

static JSValue canvas_add(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    CANVAS();
    tjs_paint_t *p = tjs_paint(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, -1);
    if (!p) {
        return JS_EXCEPTION;
    }
    TJS_TRY(ctx, tvg_canvas_add(c->c, p->p), "add");
    return tjs_this(ctx, this_val);
}

static JSValue canvas_remove(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    CANVAS();
    Tvg_Paint target = NULL;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        tjs_paint_t *p = tjs_paint(ctx, argv[0], -1);
        if (!p) {
            return JS_EXCEPTION;
        }
        target = p->p;
    }
    TJS_TRY(ctx, tvg_canvas_remove(c->c, target), "remove");
    return tjs_this(ctx, this_val);
}

static JSValue canvas_update(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    CANVAS();
    TJS_TRY(ctx, tvg_canvas_update(c->c), "update");
    return tjs_this(ctx, this_val);
}

/*
 * draw + sync, then tell the surface owner what changed. 0 / -1 / 1 like
 * TJS_CHECK. Any failure stops right there: onError may even have destroyed
 * the canvas, so nothing after it touches c.
 */
static int draw(JSContext *ctx, canvas_t *c)
{
    int r = retarget(ctx, c, false);
    if (r) {
        return r;
    }
    TJS_CHECK(ctx, tvg_canvas_update(c->c), "update");
    /*
     * webcanvas always clears, but a clear makes ThorVG redraw everything.
     * With partial rendering on it clears the dirty regions itself, so only
     * wipe a target we haven't drawn into yet.
     */
    TJS_CHECK(ctx, tvg_canvas_draw(c->c, c->fresh || !c->smart), "draw");
    c->fresh = false;

    /* regions are only readable between draw and sync */
    Tvg_Region reg[TJS_MAX_REGIONS];
    uint32_t n = 0;
    tvg_js_present_t present = c->surf >= 0 ? surfaces[c->surf].s.present : NULL;
    void *arg = c->surf >= 0 ? surfaces[c->surf].s.arg : NULL;
    if (present) {
        n = tvg_canvas_get_dirty_regions_cnt(c->c);
        if (n > TJS_MAX_REGIONS) {
            reg[0] = (Tvg_Region){ 0, 0, (int32_t)c->w, (int32_t)c->h };
            n = 1;
        } else if (n && tvg_canvas_get_dirty_regions(c->c, reg, n) != TVG_RESULT_SUCCESS) {
            n = 0;
        }
    }
    TJS_CHECK(ctx, tvg_canvas_sync(c->c), "sync");
    if (n) {
        present(arg, reg, n);
    }
    return 0;
}

static JSValue canvas_render(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    CANVAS();
    if (tjs_anim_tick(ctx)) {
        return JS_EXCEPTION;
    }
    if (!c->c) {
        return JS_ThrowTypeError(ctx, "Canvas destroyed by an onFrame callback");
    }
    return TJS_RET(ctx, draw(ctx, c));
}

/* drop everything and push a blank frame */
static JSValue canvas_clear(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    CANVAS();
    TJS_TRY(ctx, tvg_canvas_remove(c->c, NULL), "clear");
    /* full wipe: ThorVG doesn't damage paints that die with the remove (JS let
     * go of them already), so partial rendering wouldn't clear their pixels */
    c->fresh = true;
    return TJS_RET(ctx, draw(ctx, c));
}

static JSValue canvas_resize(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    CANVAS();
    int32_t wh[2] = { (int32_t)c->w, (int32_t)c->h };
    if (tjs_ints(ctx, wh, 2, argc, argv)) {
        return JS_EXCEPTION;
    }
    uint32_t max_w = c->surf >= 0 ? surfaces[c->surf].s.w : UINT32_MAX;
    uint32_t max_h = c->surf >= 0 ? surfaces[c->surf].s.h : UINT32_MAX;
    if (wh[0] <= 0 || wh[1] <= 0 || (uint32_t)wh[0] > max_w || (uint32_t)wh[1] > max_h) {
        return JS_ThrowRangeError(ctx, "resize: bad size %dx%d", (int)wh[0], (int)wh[1]);
    }
    uint32_t old[3] = { c->w, c->h, c->stride };
    c->w = wh[0];
    c->h = wh[1];
    if (c->surf < 0) {
        c->stride = c->w;
    }
    int r = retarget(ctx, c, true);
    if (r) {
        /* ThorVG kept its old target (or has none and the next render retries
         * set_target), so go back to the size that matches it */
        c->w = old[0];
        c->h = old[1];
        c->stride = old[2];
    }
    return TJS_RET(ctx, r);
}

static JSValue canvas_viewport(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    CANVAS();
    int32_t v[4] = { 0, 0, (int32_t)c->w, (int32_t)c->h };
    if (tjs_ints(ctx, v, 4, argc, argv)) {
        return JS_EXCEPTION;
    }
    TJS_TRY(ctx, tvg_canvas_set_viewport(c->c, v[0], v[1], v[2], v[3]), "viewport");
    return tjs_this(ctx, this_val);
}

static JSValue canvas_destroy(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    canvas_t *c = JS_GetOpaque(this_val, tjs.ids[TJS_CANVAS]);
    if (c) {
        canvas_release(JS_GetRuntime(ctx), c);
    }
    return JS_UNDEFINED;
}

static JSValue canvas_renderer(JSContext *ctx, JSValueConst this_val)
{
    return JS_NewString(ctx, "sw");
}

static JSValue canvas_dpr(JSContext *ctx, JSValueConst this_val)
{
    return JS_NewInt32(ctx, 1);
}

static const JSCFunctionListEntry canvas_proto[] = {
    JS_CFUNC_DEF("add", 1, canvas_add),
    JS_CFUNC_DEF("remove", 1, canvas_remove),
    JS_CFUNC_DEF("clear", 0, canvas_clear),
    JS_CFUNC_DEF("update", 0, canvas_update),
    JS_CFUNC_DEF("render", 0, canvas_render),
    JS_CFUNC_DEF("resize", 2, canvas_resize),
    JS_CFUNC_DEF("viewport", 4, canvas_viewport),
    JS_CFUNC_DEF("destroy", 0, canvas_destroy),
    JS_CGETSET_DEF("renderer", canvas_renderer, NULL),
    JS_CGETSET_DEF("dpr", canvas_dpr, NULL),
};

/* ---- Font (static only) ---- */

/* load(name, bytes, { type }). Copied into ThorVG, so the bytes can go */
static JSValue font_load(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 2 || JS_IsString(argv[1]) || !JS_IsObject(argv[1])) {
        return JS_ThrowTypeError(ctx, "Font.load: pass the font bytes, there's no font provider here");
    }
    char type[16] = "ttf";
    if (argc > 2 && JS_IsObject(argv[2])) {
        JSValue t = JS_GetPropertyStr(ctx, argv[2], "type");
        const char *s = JS_IsUndefined(t) ? NULL : JS_ToCString(ctx, t);
        JS_FreeValue(ctx, t);
        if (s) {
            snprintf(type, sizeof(type), "%s", s);
            JS_FreeCString(ctx, s);
        } else if (JS_HasException(ctx)) {
            return JS_EXCEPTION;
        }
    }
    size_t len;
    const uint8_t *buf = tjs_bytes(ctx, argv[1], &len);
    const char *name = buf ? JS_ToCString(ctx, argv[0]) : NULL;
    if (!name) {
        return JS_EXCEPTION;
    }
    Tvg_Result r = tvg_font_load_data(name, (const char *)buf, len, type, true);
    JS_FreeCString(ctx, name);
    TJS_TRY(ctx, r, "Font.load");
    return JS_UNDEFINED;
}

/* not in webcanvas: from the filesystem, named after the file minus extension */
static JSValue font_load_file(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *path = tjs_str(ctx, argc, argv, 0);
    if (!path) {
        return JS_EXCEPTION;
    }
    Tvg_Result r = tvg_font_load(path);
    JS_FreeCString(ctx, path);
    TJS_TRY(ctx, r, "Font.loadFile");
    return JS_UNDEFINED;
}

/* name for load(), path for loadFile() */
static JSValue font_unload(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *name = tjs_str(ctx, argc, argv, 0);
    if (!name) {
        return JS_EXCEPTION;
    }
    Tvg_Result r = tvg_font_load_data(name, NULL, 0, NULL, false);
    if (r == TVG_RESULT_INSUFFICIENT_CONDITION) {
        r = tvg_font_unload(name);
    }
    JS_FreeCString(ctx, name);
    TJS_TRY(ctx, r, "Font.unload");
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry font_fns[] = {
    JS_CFUNC_DEF("load", 3, font_load),
    JS_CFUNC_DEF("loadFile", 1, font_load_file),
    JS_CFUNC_DEF("unload", 1, font_unload),
};

/* ---- Accessor ---- */

/* opaque of Accessor. The walk calls JS, which may dispose() or set() this same
 * accessor while ThorVG is still iterating with it: flags catch both */
typedef struct {
    Tvg_Accessor a;     /* NULL once disposed */
    bool walking;
    bool doomed;        /* dispose() during the walk: delete once it's over */
} acc_t;

static acc_t *accessor_get(JSContext *ctx, JSValueConst v)
{
    acc_t *x = JS_GetOpaque2(ctx, v, tjs.ids[TJS_ACCESSOR]);
    if (x && (!x->a || x->doomed)) {
        JS_ThrowTypeError(ctx, "Object has been disposed");
        return NULL;
    }
    return x;
}

static void accessor_finalizer(JSRuntime *rt, JSValueConst val)
{
    JSClassID cid;
    acc_t *x = JS_GetAnyOpaque(val, &cid);  /* never mid-walk: set() holds this */
    if (x) {
        if (x->a) {
            tvg_accessor_del(x->a);
        }
        js_free_rt(rt, x);
    }
}

static JSValue accessor_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv, int magic)
{
    JSValue obj = tjs_new_obj(ctx, new_target, TJS_ACCESSOR);
    acc_t *x = JS_IsException(obj) ? NULL : js_mallocz(ctx, sizeof(*x));
    if (!x) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    JS_SetOpaque(obj, x);
    x->a = tvg_accessor_new();
    if (!x->a) {
        JS_FreeValue(ctx, obj);
        return JS_ThrowOutOfMemory(ctx);
    }
    return obj;
}

static JSValue accessor_id(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *s = tjs_str(ctx, argc, argv, 0);
    if (!s) {
        return JS_EXCEPTION;
    }
    uint32_t id = tvg_accessor_generate_id(s);
    JS_FreeCString(ctx, s);
    return JS_NewUint32(ctx, id);
}

typedef struct {
    JSContext *ctx;
    JSValueConst cb;
    bool failed;
} walk_t;

/* runs synchronously inside tvg_accessor_set(), on our task */
static bool walk_cb(Tvg_Paint paint, void *data)
{
    walk_t *wk = data;
    JSValue p = tjs_wrap_paint(wk->ctx, paint);
    if (JS_IsException(p)) {
        wk->failed = true;
        return false;
    }
    JSValue ret = JS_Call(wk->ctx, wk->cb, JS_UNDEFINED, 1, (JSValueConst *)&p);
    JS_FreeValue(wk->ctx, p);
    if (JS_IsException(ret)) {
        wk->failed = true;
        return false;
    }
    bool more = JS_ToBool(wk->ctx, ret);
    JS_FreeValue(wk->ctx, ret);
    return more;
}

/* set(paint, cb): cb(paint) for each descendant until it returns false. Don't change the tree from cb */
static JSValue accessor_set(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    acc_t *x = accessor_get(ctx, this_val);
    tjs_paint_t *p = x ? tjs_paint(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, -1) : NULL;
    if (!p) {
        return JS_EXCEPTION;
    }
    if (argc < 2 || !JS_IsFunction(ctx, argv[1])) {
        return JS_ThrowTypeError(ctx, "Accessor.set: callback isn't a function");
    }
    if (x->walking) {
        return JS_ThrowTypeError(ctx, "Accessor.set: already walking with this accessor");
    }
    walk_t wk = { ctx, argv[1], false };
    x->walking = true;
    Tvg_Result r = tvg_accessor_set(x->a, p->p, walk_cb, &wk);
    x->walking = false;
    if (x->doomed) {
        tvg_accessor_del(x->a);
        x->a = NULL;
        x->doomed = false;
    }
    if (wk.failed) {
        return JS_EXCEPTION;
    }
    TJS_TRY(ctx, r, "Accessor.set");
    return JS_UNDEFINED;
}

static JSValue accessor_name(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    acc_t *x = accessor_get(ctx, this_val);
    uint32_t id = 0;
    if (!x || (argc > 0 && JS_ToUint32(ctx, &id, argv[0]))) {
        return JS_EXCEPTION;
    }
    const char *s = tvg_accessor_get_name(x->a, id);
    return s ? JS_NewString(ctx, s) : JS_NULL;
}

static JSValue accessor_dispose(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    acc_t *x = JS_GetOpaque(this_val, tjs.ids[TJS_ACCESSOR]);
    if (x && x->a) {
        if (x->walking) {
            x->doomed = true;   /* ThorVG is iterating with it right now */
        } else {
            tvg_accessor_del(x->a);
            x->a = NULL;
        }
    }
    return JS_UNDEFINED;
}

static JSValue accessor_disposed(JSContext *ctx, JSValueConst this_val)
{
    acc_t *x = JS_GetOpaque(this_val, tjs.ids[TJS_ACCESSOR]);
    return JS_NewBool(ctx, !x || !x->a || x->doomed);
}

static const JSCFunctionListEntry accessor_proto[] = {
    JS_CFUNC_DEF("set", 2, accessor_set),
    JS_CFUNC_DEF("name", 1, accessor_name),
    JS_CFUNC_DEF("dispose", 0, accessor_dispose),
    JS_CGETSET_DEF("isDisposed", accessor_disposed, NULL),
};

static const JSCFunctionListEntry accessor_static[] = {
    JS_CFUNC_DEF("id", 1, accessor_id),
};

int tjs_init_canvas(JSContext *ctx, JSValueConst ns)
{
    static const tjs_class_t canvas = TJS_CLASS("Canvas", TJS_CANVAS, canvas_ctor, 2, canvas_proto,
                                                canvas_finalizer, canvas_mark);
    static const tjs_class_t accessor = TJS_CLASS("Accessor", TJS_ACCESSOR, accessor_ctor, 0, accessor_proto,
                                                  accessor_finalizer, NULL);
    JSValue cv = tjs_def_class(ctx, &canvas, JS_UNDEFINED);
    if (JS_IsException(cv)) {
        return -1;
    }
    JS_SetPropertyStr(ctx, ns, "Canvas", cv);

    JSValue ac = tjs_def_class(ctx, &accessor, JS_UNDEFINED);
    if (JS_IsException(ac)) {
        return -1;
    }
    JS_SetPropertyFunctionList(ctx, ac, accessor_static, countof(accessor_static));
    JS_SetPropertyStr(ctx, ns, "Accessor", ac);

    JSValue font = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, font, font_fns, countof(font_fns));
    JS_SetPropertyStr(ctx, ns, "Font", font);
    return 0;
}
