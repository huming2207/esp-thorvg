/*
 * Paint, Shape, Scene, Picture, Text.
 *
 * Ownership: every JS wrapper holds one ThorVG ref on its paint, scenes and
 * the canvas hold their own. So a paint lives while JS or a parent uses it and
 * nothing dangles whichever goes first. Wrappers for paints we didn't create
 * (Picture.paint(), Accessor callbacks) work the same way.
 *
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <string.h>
#include "tvg_js_priv.h"

#define PAINT(cls) \
    tjs_paint_t *w = tjs_paint(ctx, this_val, cls); \
    if (!w) return JS_EXCEPTION

static bool arg_set(int argc, JSValueConst *argv, int i)
{
    return argc > i && !JS_IsUndefined(argv[i]);
}

static int paint_cls(JSClassID cid)
{
    for (int i = TJS_SHAPE; i <= TJS_TEXT; i++) {
        if (cid && cid == tjs.ids[i]) {
            return i;
        }
    }
    return -1;
}

tjs_paint_t *tjs_paint(JSContext *ctx, JSValueConst v, int cls)
{
    static const char *const names[] = { "Shape", "Scene", "Picture", "Text" };
    JSClassID cid;
    tjs_paint_t *w = JS_GetAnyOpaque(v, &cid);
    int got = paint_cls(cid);

    if (got < 0 || (cls >= 0 && got != cls) || !w) {
        JS_ThrowTypeError(ctx, "not a %s", cls < 0 ? "Paint" : names[cls]);
        return NULL;
    }
    if (!w->p) {
        JS_ThrowTypeError(ctx, "Object has been disposed");
        return NULL;
    }
    return w;
}

/* takes obj, takes a ref on p (a fresh p dies with the wrapper on failure) */
static JSValue adopt(JSContext *ctx, JSValue obj, Tvg_Paint p)
{
    if (!p) {
        JS_FreeValue(ctx, obj);
        return JS_ThrowOutOfMemory(ctx);
    }
    tvg_paint_ref(p);
    tjs_paint_t *w = JS_IsException(obj) ? NULL : js_mallocz(ctx, sizeof(*w));
    if (!w) {
        tvg_paint_unref(p, true);
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    w->p = p;
    w->text.line_spacing = 1.0f;    /* ThorVG's default */
    JS_SetOpaque(obj, w);
    return obj;
}

JSValue tjs_wrap_paint(JSContext *ctx, Tvg_Paint p)
{
    Tvg_Type t = TVG_TYPE_UNDEF;
    tvg_paint_get_type(p, &t);
    if (t < TVG_TYPE_SHAPE || t > TVG_TYPE_TEXT) {
        return JS_ThrowTypeError(ctx, "unknown paint type %d", t);
    }
    /* Tvg_Type shape..text is 1..4, our classes 0..3 */
    return adopt(ctx, JS_NewObjectClass(ctx, tjs.ids[TJS_SHAPE + t - TVG_TYPE_SHAPE]), p);
}

/*
 * Can `from` reach `to` through children, clippers or masks? Then making
 * `from` a child/clipper/mask of `to` is a cycle, and ThorVG recurses until
 * the stack dies. ThorVG itself only checks direct parents.
 */
#define REACH_MAX_DEPTH 16  /* clip/mask nesting; deeper than that we refuse rather than guess */

typedef struct {
    Tvg_Paint to;
    int depth;
    bool hit;
} reach_t;

static bool reach_tree(Tvg_Paint from, reach_t *r);

/* p itself, then its clipper and mask target with their subtrees */
static bool reach_one(Tvg_Paint p, void *data)
{
    reach_t *r = data;
    if (p == r->to) {
        r->hit = true;
        return false;
    }
    Tvg_Paint m = NULL;
    Tvg_Mask_Method mm;
    tvg_paint_get_mask_method(p, (Tvg_Paint)&m, &mm);  /* "target" is really a Paint** out */
    Tvg_Paint c = tvg_paint_get_clip(p);
    if (c || m) {
        if (++r->depth > REACH_MAX_DEPTH) {
            r->hit = true;
        } else {
            reach_tree(c, r);
            reach_tree(m, r);
        }
        r->depth--;
    }
    return !r->hit;     /* false stops the accessor walk */
}

static bool reach_tree(Tvg_Paint from, reach_t *r)
{
    if (!from || r->hit) {
        return r->hit;
    }
    Tvg_Type t = TVG_TYPE_UNDEF;
    tvg_paint_get_type(from, &t);
    if (t != TVG_TYPE_SCENE && t != TVG_TYPE_PICTURE) {
        reach_one(from, r);
        return r->hit;
    }
    /* pre-order walk of the whole subtree, root included */
    Tvg_Accessor a = tvg_accessor_new();
    if (!a) {
        return r->hit = true;   /* can't tell, refuse */
    }
    tvg_accessor_set(a, from, reach_one, r);
    tvg_accessor_del(a);
    return r->hit;
}

bool tjs_reaches(Tvg_Paint from, Tvg_Paint to)
{
    reach_t r = { .to = to };
    return reach_tree(from, &r);
}

static void paint_finalizer(JSRuntime *rt, JSValueConst val)
{
    JSClassID cid;
    tjs_paint_t *w = JS_GetAnyOpaque(val, &cid);
    if (w) {
        tjs_layout_paint_gone(w);
        if (w->p) {
            tvg_paint_unref(w->p, true);
        }
        js_free_rt(rt, w);
    }
}

/* ---- Paint ---- */

static JSValue paint_get_id(JSContext *ctx, JSValueConst this_val)
{
    PAINT(-1);
    return JS_NewUint32(ctx, tvg_paint_get_id(w->p));
}

/* strings hash the same way the loaders name things */
static int to_id(JSContext *ctx, JSValueConst v, uint32_t *id)
{
    if (!JS_IsString(v)) {
        return JS_ToUint32(ctx, id, v);
    }
    const char *s = JS_ToCString(ctx, v);
    if (!s) {
        return -1;
    }
    *id = tvg_accessor_generate_id(s);
    JS_FreeCString(ctx, s);
    return 0;
}

static JSValue paint_set_id(JSContext *ctx, JSValueConst this_val, JSValueConst val)
{
    PAINT(-1);
    uint32_t id;
    if (to_id(ctx, val, &id)) {
        return JS_EXCEPTION;
    }
    TJS_TRY(ctx, tvg_paint_set_id(w->p, id), "id");
    return JS_UNDEFINED;
}

static JSValue paint_translate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    PAINT(-1);
    float v[2] = { 0, 0 };
    if (tjs_nums(ctx, v, 2, argc, argv)) {
        return JS_EXCEPTION;
    }
    tvg_paint_translate(w->p, v[0], v[1]);
    return tjs_this(ctx, this_val);
}

static JSValue paint_rotate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    PAINT(-1);
    float deg = 0;
    if (tjs_nums(ctx, &deg, 1, argc, argv)) {
        return JS_EXCEPTION;
    }
    tvg_paint_rotate(w->p, deg);
    return tjs_this(ctx, this_val);
}

static JSValue paint_scale(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    PAINT(-1);
    float f = 1;
    if (tjs_nums(ctx, &f, 1, argc, argv)) {
        return JS_EXCEPTION;
    }
    tvg_paint_scale(w->p, f);
    return tjs_this(ctx, this_val);
}

static JSValue paint_blend(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    PAINT(-1);
    int32_t m = 0;
    if (tjs_ints(ctx, &m, 1, argc, argv)) {
        return JS_EXCEPTION;
    }
    TJS_TRY(ctx, tvg_paint_set_blend_method(w->p, m), "blend");
    return tjs_this(ctx, this_val);
}

static JSValue paint_opacity(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    PAINT(-1);
    if (!arg_set(argc, argv, 0)) {
        uint8_t o = 0;
        tvg_paint_get_opacity(w->p, &o);
        return JS_NewInt32(ctx, o);
    }
    int32_t o;
    if (JS_ToInt32(ctx, &o, argv[0])) {
        return JS_EXCEPTION;
    }
    tvg_paint_set_opacity(w->p, tjs_u8(o));
    return tjs_this(ctx, this_val);
}

static JSValue paint_visible(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    PAINT(-1);
    if (!arg_set(argc, argv, 0)) {
        return JS_NewBool(ctx, tvg_paint_get_visible(w->p));
    }
    tvg_paint_set_visible(w->p, JS_ToBool(ctx, argv[0]));
    return tjs_this(ctx, this_val);
}

static JSValue xy(JSContext *ctx, float x, float y)
{
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "x", JS_NewFloat64(ctx, x));
    JS_SetPropertyStr(ctx, o, "y", JS_NewFloat64(ctx, y));
    return o;
}

/* like webcanvas, failures give zeros / [] rather than throwing */
static JSValue paint_bounds(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    PAINT(-1);
    bool oriented = false;
    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValue o = JS_GetPropertyStr(ctx, argv[0], "oriented");
        if (JS_IsException(o)) {
            return o;
        }
        oriented = JS_ToBool(ctx, o);
        JS_FreeValue(ctx, o);
    }
    if (oriented) {
        Tvg_Point pt[4];
        JSValue arr = JS_NewArray(ctx);
        if (tvg_paint_get_obb(w->p, pt) == TVG_RESULT_SUCCESS) {
            for (int i = 0; i < 4; i++) {
                JS_SetPropertyUint32(ctx, arr, i, xy(ctx, pt[i].x, pt[i].y));
            }
        }
        return arr;
    }
    float b[4] = { 0, 0, 0, 0 };
    tvg_paint_get_aabb(w->p, &b[0], &b[1], &b[2], &b[3]);
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "x", JS_NewFloat64(ctx, b[0]));
    JS_SetPropertyStr(ctx, o, "y", JS_NewFloat64(ctx, b[1]));
    JS_SetPropertyStr(ctx, o, "width", JS_NewFloat64(ctx, b[2]));
    JS_SetPropertyStr(ctx, o, "height", JS_NewFloat64(ctx, b[3]));
    return o;
}

static JSValue paint_duplicate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    PAINT(-1);
    JSClassID cid;
    JS_GetAnyOpaque(this_val, &cid);
    JSValue obj = adopt(ctx, JS_NewObjectClass(ctx, cid), tvg_paint_duplicate(w->p));
    if (!JS_IsException(obj) && w->accessible) {
        /* ThorVG's duplicate() doesn't carry it over */
        tjs_paint_t *d = JS_GetOpaque(obj, cid);
        d->accessible = tvg_picture_set_accessible(d->p, true) == TVG_RESULT_SUCCESS;
    }
    return obj;
}

static JSValue paint_transform(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    static const char *const keys[9] = { "e11", "e12", "e13", "e21", "e22", "e23", "e31", "e32", "e33" };
    PAINT(-1);
    Tvg_Matrix m = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    float *e = &m.e11;
    for (int i = 0; i < 9; i++) {
        if (tjs_opt_num(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, keys[i], &e[i])) {
            return JS_EXCEPTION;
        }
    }
    TJS_TRY(ctx, tvg_paint_set_transform(w->p, &m), "transform");
    return tjs_this(ctx, this_val);
}

static bool is_nothing(int argc, JSValueConst *argv)
{
    return argc < 1 || JS_IsUndefined(argv[0]) || JS_IsNull(argv[0]);
}

/* clip(shape | null). ThorVG casts any clipper to Shape* unchecked, so check here */
static JSValue paint_clip(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    PAINT(-1);
    Tvg_Paint clipper = NULL;
    if (!is_nothing(argc, argv)) {
        tjs_paint_t *c = tjs_paint(ctx, argv[0], TJS_SHAPE);
        if (!c) {
            return JS_EXCEPTION;
        }
        if (tjs_reaches(c->p, w->p)) {
            return JS_ThrowRangeError(ctx, "clip: that would clip the paint by itself");
        }
        clipper = c->p;
    }
    TJS_TRY(ctx, tvg_paint_set_clip(w->p, clipper), "clip");
    return tjs_this(ctx, this_val);
}

/* mask(target, method); mask(null, MaskMethod.None) removes it */
static JSValue paint_mask(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    PAINT(-1);
    int32_t m = 0;
    if (tjs_ints(ctx, &m, 1, argc - 1, argv + 1)) {
        return JS_EXCEPTION;
    }
    Tvg_Paint target = NULL;
    if (is_nothing(argc, argv)) {
        if (m != TVG_MASK_METHOD_NONE) {    /* ThorVG would deref the NULL target */
            return JS_ThrowTypeError(ctx, "mask: no target needs MaskMethod.None");
        }
    } else {
        tjs_paint_t *t = tjs_paint(ctx, argv[0], -1);
        if (!t) {
            return JS_EXCEPTION;
        }
        if (tjs_reaches(t->p, w->p)) {
            return JS_ThrowRangeError(ctx, "mask: that would mask the paint by itself");
        }
        target = t->p;
    }
    TJS_TRY(ctx, tvg_paint_set_mask_method(w->p, target, m), "mask");
    return tjs_this(ctx, this_val);
}

static JSValue paint_intersects(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    PAINT(-1);
    int32_t r[4] = { 0, 0, 0, 0 };
    if (tjs_ints(ctx, r, 4, argc, argv)) {
        return JS_EXCEPTION;
    }
    bool vis = argc > 4 && JS_ToBool(ctx, argv[4]);
    return JS_NewBool(ctx, tvg_paint_intersects_region(w->p, r[0], r[1], r[2], r[3], vis));
}

static JSValue paint_dispose(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSClassID cid;
    tjs_paint_t *w = JS_GetAnyOpaque(this_val, &cid);
    if (paint_cls(cid) >= 0 && w && w->p) {
        tvg_paint_unref(w->p, true);
        w->p = NULL;
    }
    return JS_UNDEFINED;
}

static JSValue paint_disposed(JSContext *ctx, JSValueConst this_val)
{
    JSClassID cid;
    tjs_paint_t *w = JS_GetAnyOpaque(this_val, &cid);
    return JS_NewBool(ctx, paint_cls(cid) < 0 || !w || !w->p);
}

static const JSCFunctionListEntry paint_proto[] = {
    JS_CGETSET_DEF("id", paint_get_id, paint_set_id),
    JS_CFUNC_DEF("translate", 2, paint_translate),
    JS_CFUNC_DEF("rotate", 1, paint_rotate),
    JS_CFUNC_DEF("scale", 1, paint_scale),
    JS_CFUNC_DEF("blend", 1, paint_blend),
    JS_CFUNC_DEF("opacity", 1, paint_opacity),
    JS_CFUNC_DEF("visible", 1, paint_visible),
    JS_CFUNC_DEF("bounds", 1, paint_bounds),
    JS_CFUNC_DEF("duplicate", 0, paint_duplicate),
    JS_CFUNC_DEF("transform", 1, paint_transform),
    JS_CFUNC_DEF("clip", 1, paint_clip),
    JS_CFUNC_DEF("mask", 2, paint_mask),
    JS_CFUNC_DEF("intersects", 5, paint_intersects),
    JS_CFUNC_DEF("dispose", 0, paint_dispose),
    JS_CGETSET_DEF("isDisposed", paint_disposed, NULL),
};

/* magic: class */
static JSValue paint_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv, int magic)
{
    Tvg_Paint p = magic == TJS_SHAPE ? tvg_shape_new()
                : magic == TJS_SCENE ? tvg_scene_new()
                : magic == TJS_PICTURE ? tvg_picture_new()
                : tvg_text_new();
    return adopt(ctx, tjs_new_obj(ctx, new_target, magic), p);
}

/* ---- Shape ---- */

/* magic: 0 moveTo, 1 lineTo, 2 cubicTo */
static JSValue shape_path_op(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    static const char *const ops[] = { "moveTo", "lineTo", "cubicTo" };
    PAINT(TJS_SHAPE);
    float v[6] = { 0, 0, 0, 0, 0, 0 };
    if (tjs_nums(ctx, v, magic == 2 ? 6 : 2, argc, argv)) {
        return JS_EXCEPTION;
    }
    Tvg_Result r = magic == 0 ? tvg_shape_move_to(w->p, v[0], v[1])
                 : magic == 1 ? tvg_shape_line_to(w->p, v[0], v[1])
                 : tvg_shape_cubic_to(w->p, v[0], v[1], v[2], v[3], v[4], v[5]);
    TJS_TRY(ctx, r, ops[magic]);
    return tjs_this(ctx, this_val);
}

static JSValue shape_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    PAINT(TJS_SHAPE);
    TJS_TRY(ctx, tvg_shape_close(w->p), "close");
    return tjs_this(ctx, this_val);
}

static JSValue shape_append_rect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    PAINT(TJS_SHAPE);
    float v[4] = { 0, 0, 0, 0 }, rx = 0, ry = 0;
    JSValueConst opts = argc > 4 ? argv[4] : JS_UNDEFINED;
    if (tjs_nums(ctx, v, 4, argc, argv) || tjs_opt_num(ctx, opts, "rx", &rx) ||
        tjs_opt_num(ctx, opts, "ry", &ry)) {
        return JS_EXCEPTION;
    }
    bool cw = true;
    if (JS_IsObject(opts)) {
        JSValue c = JS_GetPropertyStr(ctx, opts, "clockwise");
        if (JS_IsException(c)) {
            return c;
        }
        cw = JS_IsUndefined(c) || JS_ToBool(ctx, c);
        JS_FreeValue(ctx, c);
    }
    TJS_TRY(ctx, tvg_shape_append_rect(w->p, v[0], v[1], v[2], v[3], rx, ry, cw), "appendRect");
    return tjs_this(ctx, this_val);
}

static JSValue shape_append_circle(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    PAINT(TJS_SHAPE);
    float v[4] = { 0, 0, 0, NAN };
    if (tjs_nums(ctx, v, 4, argc, argv)) {
        return JS_EXCEPTION;
    }
    if (isnan(v[3])) {
        v[3] = v[2];
    }
    bool cw = !arg_set(argc, argv, 4) || JS_ToBool(ctx, argv[4]);
    TJS_TRY(ctx, tvg_shape_append_circle(w->p, v[0], v[1], v[2], v[3], cw), "appendCircle");
    return tjs_this(ctx, this_val);
}

/* points[i] is [x, y] */
static int get_point(JSContext *ctx, JSValueConst pts, uint32_t i, uint32_t n, float out[2])
{
    if (i >= n) {
        JS_ThrowRangeError(ctx, "appendPath: not enough points");
        return -1;
    }
    JSValue pt = JS_GetPropertyUint32(ctx, pts, i);
    if (JS_IsException(pt)) {
        return -1;
    }
    if (!JS_IsObject(pt)) {
        JS_FreeValue(ctx, pt);
        JS_ThrowTypeError(ctx, "appendPath: points[%u] isn't [x, y]", (unsigned)i);
        return -1;
    }
    JSValue v[2] = { JS_GetPropertyUint32(ctx, pt, 0), JS_UNDEFINED };
    if (!JS_IsException(v[0])) {
        v[1] = JS_GetPropertyUint32(ctx, pt, 1);
    }
    out[0] = out[1] = 0;
    int ret = JS_IsException(v[0]) || JS_IsException(v[1]) ? -1 : tjs_nums(ctx, out, 2, 2, v);
    JS_FreeValue(ctx, v[0]);
    JS_FreeValue(ctx, v[1]);
    JS_FreeValue(ctx, pt);
    return ret;
}

/* fed command by command, so no scratch arrays */
static JSValue shape_append_path(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    PAINT(TJS_SHAPE);
    JSValueConst cmds = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValueConst pts = argc > 1 ? argv[1] : JS_UNDEFINED;
    int64_t ncmd = 0, npt = 0;
    if (JS_GetLength(ctx, cmds, &ncmd) || (ncmd > 0 && JS_GetLength(ctx, pts, &npt))) {
        return JS_EXCEPTION;
    }
    uint32_t pi = 0;
    if (ncmd > UINT32_MAX || npt > UINT32_MAX) {
        return JS_ThrowRangeError(ctx, "appendPath: too long");
    }
    for (uint32_t i = 0; i < (uint32_t)ncmd; i++) {
        int32_t cmd;
        float p[6];
        JSValue c = JS_GetPropertyUint32(ctx, cmds, i);
        int bad = JS_ToInt32(ctx, &cmd, c);
        JS_FreeValue(ctx, c);
        if (bad) {
            return JS_EXCEPTION;
        }
        Tvg_Result r;
        switch (cmd) {
        case TVG_PATH_COMMAND_CLOSE:
            r = tvg_shape_close(w->p);
            break;
        case TVG_PATH_COMMAND_MOVE_TO:
        case TVG_PATH_COMMAND_LINE_TO:
            if (get_point(ctx, pts, pi++, npt, p)) {
                return JS_EXCEPTION;
            }
            r = cmd == TVG_PATH_COMMAND_MOVE_TO ? tvg_shape_move_to(w->p, p[0], p[1])
                                                : tvg_shape_line_to(w->p, p[0], p[1]);
            break;
        case TVG_PATH_COMMAND_CUBIC_TO:
            for (int k = 0; k < 3; k++) {
                if (get_point(ctx, pts, pi++, npt, &p[k * 2])) {
                    return JS_EXCEPTION;
                }
            }
            r = tvg_shape_cubic_to(w->p, p[0], p[1], p[2], p[3], p[4], p[5]);
            break;
        default:
            return JS_ThrowRangeError(ctx, "appendPath: bad command %d", (int)cmd);
        }
        TJS_TRY(ctx, r, "appendPath");
    }
    return tjs_this(ctx, this_val);
}

static JSValue shape_path(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    PAINT(TJS_SHAPE);
    const Tvg_Path_Command *cmd = NULL;
    const Tvg_Point *pt = NULL;
    uint32_t ncmd = 0, npt = 0;
    TJS_TRY(ctx, tvg_shape_get_path(w->p, &cmd, &ncmd, &pt, &npt), "path");

    JSValue cmds = JS_NewArray(ctx), pts = JS_NewArray(ctx);
    for (uint32_t i = 0; i < ncmd; i++) {
        JS_SetPropertyUint32(ctx, cmds, i, JS_NewInt32(ctx, cmd[i]));
    }
    for (uint32_t i = 0; i < npt; i++) {
        JSValue p = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, p, 0, JS_NewFloat64(ctx, pt[i].x));
        JS_SetPropertyUint32(ctx, p, 1, JS_NewFloat64(ctx, pt[i].y));
        JS_SetPropertyUint32(ctx, pts, i, p);
    }
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "commands", cmds);
    JS_SetPropertyStr(ctx, o, "points", pts);
    return o;
}

static JSValue shape_fill_rule(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    PAINT(TJS_SHAPE);
    int32_t r = 0;
    if (tjs_ints(ctx, &r, 1, argc, argv)) {
        return JS_EXCEPTION;
    }
    TJS_TRY(ctx, tvg_shape_set_fill_rule(w->p, r), "fillRule");
    return tjs_this(ctx, this_val);
}

static JSValue shape_trim_path(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    PAINT(TJS_SHAPE);
    float v[2] = { 0, 1 };
    if (tjs_nums(ctx, v, 2, argc, argv)) {
        return JS_EXCEPTION;
    }
    bool sim = !arg_set(argc, argv, 2) || JS_ToBool(ctx, argv[2]);
    TJS_TRY(ctx, tvg_shape_set_trimpath(w->p, v[0], v[1], sim), "trimPath");
    return tjs_this(ctx, this_val);
}

static JSValue shape_fill(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    PAINT(TJS_SHAPE);
    if (argc > 0 && tjs_is_fill(argv[0])) {
        Tvg_Gradient g = tjs_fill_dup(ctx, argv[0]);
        if (!g) {
            return JS_EXCEPTION;
        }
        Tvg_Result r = tvg_shape_set_gradient(w->p, g);
        if (r != TVG_RESULT_SUCCESS) {
            tvg_gradient_del(g);
        }
        TJS_TRY(ctx, r, "fill (gradient)");
        return tjs_this(ctx, this_val);
    }
    int32_t c[4] = { 0, 0, 0, 255 };
    if (argc < 3 || !JS_IsNumber(argv[0])) {
        return JS_ThrowTypeError(ctx, "Invalid fill arguments");
    }
    if (tjs_ints(ctx, c, 4, argc, argv)) {
        return JS_EXCEPTION;
    }
    TJS_TRY(ctx, tvg_shape_set_fill_color(w->p, tjs_u8(c[0]), tjs_u8(c[1]), tjs_u8(c[2]), tjs_u8(c[3])),
            "fill (color)");
    return tjs_this(ctx, this_val);
}

static int stroke_dash(JSContext *ctx, Tvg_Paint p, JSValueConst opts)
{
    float dash[TJS_MAX_DASH], off = 0;
    uint32_t n = 0;
    JSValue pat = JS_GetPropertyStr(ctx, opts, "dash");
    if (JS_IsException(pat)) {
        return -1;
    }
    JSValue offv = JS_GetPropertyStr(ctx, opts, "dashOffset");
    bool has_pat = !JS_IsUndefined(pat), has_off = !JS_IsUndefined(offv);
    int bad = JS_IsException(offv) ? -1 : tjs_nums(ctx, &off, 1, 1, (JSValueConst *)&offv);
    JS_FreeValue(ctx, offv);

    if (!bad && has_pat) {
        int64_t len = 0;
        bad = JS_GetLength(ctx, pat, &len);
        if (!bad && len > TJS_MAX_DASH) {
            JS_ThrowRangeError(ctx, "stroke: dash pattern longer than %d", TJS_MAX_DASH);
            bad = -1;
        }
        for (n = 0; !bad && n < len; n++) {
            JSValue v = JS_GetPropertyUint32(ctx, pat, n);
            dash[n] = 0;
            bad = tjs_nums(ctx, &dash[n], 1, 1, (JSValueConst *)&v);
            JS_FreeValue(ctx, v);
        }
    } else if (!bad && has_off) {
        /* offset only: keep the current pattern, copied out as set() frees it */
        const float *cur = NULL;
        float old_off;
        if (tvg_shape_get_stroke_dash(p, &cur, &n, &old_off) != TVG_RESULT_SUCCESS || !cur) {
            n = 0;
        }
        n = n > TJS_MAX_DASH ? TJS_MAX_DASH : n;
        if (n) {
            memcpy(dash, cur, n * sizeof(float));
        }
        has_pat = n > 0;
    }
    JS_FreeValue(ctx, pat);
    if (bad) {
        return -1;
    }
    if (has_pat) {
        TJS_CHECK(ctx, tvg_shape_set_stroke_dash(p, n ? dash : NULL, n, off), "stroke (dash)");
    }
    return 0;
}

static JSValue shape_stroke(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    PAINT(TJS_SHAPE);
    JSValueConst o = argc > 0 ? argv[0] : JS_UNDEFINED;
    float f;

    if (!JS_IsObject(o)) {
        f = 0;
        if (tjs_nums(ctx, &f, 1, argc, argv)) {
            return JS_EXCEPTION;
        }
        TJS_TRY(ctx, tvg_shape_set_stroke_width(w->p, f), "stroke (width)");
        return tjs_this(ctx, this_val);
    }

    f = NAN;
    if (tjs_opt_num(ctx, o, "width", &f)) {
        return JS_EXCEPTION;
    }
    if (!isnan(f)) {
        TJS_TRY(ctx, tvg_shape_set_stroke_width(w->p, f), "stroke (width)");
    }

    JSValue v = JS_GetPropertyStr(ctx, o, "color");
    if (JS_IsException(v)) {
        return v;
    }
    if (JS_IsObject(v)) {
        int32_t c[4];
        int bad = tjs_color(ctx, v, c);
        JS_FreeValue(ctx, v);
        if (bad) {
            return JS_EXCEPTION;
        }
        TJS_TRY(ctx, tvg_shape_set_stroke_color(w->p, tjs_u8(c[0]), tjs_u8(c[1]), tjs_u8(c[2]), tjs_u8(c[3])),
                "stroke (color)");
    } else {
        JS_FreeValue(ctx, v);
    }

    v = JS_GetPropertyStr(ctx, o, "gradient");
    if (JS_IsException(v)) {
        return v;
    }
    if (tjs_is_fill(v)) {
        Tvg_Gradient g = tjs_fill_dup(ctx, v);
        JS_FreeValue(ctx, v);
        if (!g) {
            return JS_EXCEPTION;
        }
        Tvg_Result r = tvg_shape_set_stroke_gradient(w->p, g);
        if (r != TVG_RESULT_SUCCESS) {
            tvg_gradient_del(g);
        }
        TJS_TRY(ctx, r, "stroke (gradient)");
    } else {
        JS_FreeValue(ctx, v);
    }

    int32_t cap = -1, join = -1;
    if (tjs_opt_int(ctx, o, "cap", &cap) || tjs_opt_int(ctx, o, "join", &join)) {
        return JS_EXCEPTION;
    }
    if (cap >= 0) {
        TJS_TRY(ctx, tvg_shape_set_stroke_cap(w->p, cap), "stroke (cap)");
    }
    if (join >= 0) {
        TJS_TRY(ctx, tvg_shape_set_stroke_join(w->p, join), "stroke (join)");
    }
    f = NAN;
    if (tjs_opt_num(ctx, o, "miterLimit", &f)) {
        return JS_EXCEPTION;
    }
    if (!isnan(f)) {
        TJS_TRY(ctx, tvg_shape_set_stroke_miterlimit(w->p, f), "stroke (miterLimit)");
    }
    int r = stroke_dash(ctx, w->p, o);
    return r ? TJS_RET(ctx, r) : tjs_this(ctx, this_val);
}

static JSValue shape_order(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    PAINT(TJS_SHAPE);
    TJS_TRY(ctx, tvg_shape_set_paint_order(w->p, argc > 0 && JS_ToBool(ctx, argv[0])), "order");
    return tjs_this(ctx, this_val);
}

static JSValue shape_reset(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    PAINT(TJS_SHAPE);
    TJS_TRY(ctx, tvg_shape_reset(w->p), "reset");
    return tjs_this(ctx, this_val);
}

static const JSCFunctionListEntry shape_proto[] = {
    JS_CFUNC_MAGIC_DEF("moveTo", 2, shape_path_op, 0),
    JS_CFUNC_MAGIC_DEF("lineTo", 2, shape_path_op, 1),
    JS_CFUNC_MAGIC_DEF("cubicTo", 6, shape_path_op, 2),
    JS_CFUNC_DEF("close", 0, shape_close),
    JS_CFUNC_DEF("appendRect", 5, shape_append_rect),
    JS_CFUNC_DEF("appendCircle", 5, shape_append_circle),
    JS_CFUNC_DEF("appendPath", 2, shape_append_path),
    JS_CFUNC_DEF("path", 0, shape_path),
    JS_CFUNC_DEF("fillRule", 1, shape_fill_rule),
    JS_CFUNC_DEF("trimPath", 3, shape_trim_path),
    JS_CFUNC_DEF("fill", 4, shape_fill),
    JS_CFUNC_DEF("stroke", 1, shape_stroke),
    JS_CFUNC_DEF("order", 1, shape_order),
    JS_CFUNC_DEF("reset", 0, shape_reset),
};

/* ---- Scene ---- */

/* add(paint, before?): before must be a child, paint goes in front of it */
static JSValue scene_add(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    PAINT(TJS_SCENE);
    tjs_paint_t *c = tjs_paint(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, -1);
    if (!c) {
        return JS_EXCEPTION;
    }
    Tvg_Paint at = NULL;
    if (arg_set(argc, argv, 1) && !JS_IsNull(argv[1])) {
        tjs_paint_t *b = tjs_paint(ctx, argv[1], -1);
        if (!b) {
            return JS_EXCEPTION;
        }
        if (tvg_paint_get_parent(b->p) != w->p) {
            return JS_ThrowRangeError(ctx, "add: 'before' isn't a child of this scene");
        }
        at = b->p;
    }
    /* ThorVG doesn't check, a scene inside itself recurses forever on render */
    if (tjs_reaches(c->p, w->p)) {
        return JS_ThrowRangeError(ctx, "add: that would put the scene inside itself");
    }
    /* the C API's insert refuses a NULL `at`, despite its docs */
    TJS_TRY(ctx, at ? tvg_scene_insert(w->p, c->p, at) : tvg_scene_add(w->p, c->p), "add");
    return tjs_this(ctx, this_val);
}

static JSValue scene_remove(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    PAINT(TJS_SCENE);
    Tvg_Paint target = NULL;
    if (arg_set(argc, argv, 0)) {
        tjs_paint_t *c = tjs_paint(ctx, argv[0], -1);
        if (!c) {
            return JS_EXCEPTION;
        }
        target = c->p;
    }
    TJS_TRY(ctx, tvg_scene_remove(w->p, target), target ? "remove" : "remove (all)");
    return tjs_this(ctx, this_val);
}

/*
 * magic: which effect. Defaults are webcanvas'. Everything ThorVG stores as
 * a byte is clamped here: it only caps quality from above, and -1 turned
 * into 255 overruns its 3-entry blur kernel array.
 */
static JSValue scene_effect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    static const char *const ops[] = {
        "resetEffects", "gaussianBlur", "motionBlur", "dropShadow", "fillEffect", "tint", "tritone",
    };
    static const uint8_t nargs[] = { 0, 4, 3, 8, 4, 7, 10 };
    PAINT(TJS_SCENE);
    float v[10] = { 0 };
    if (magic == 1 || magic == 2) {
        v[magic == 1 ? 3 : 2] = 75;
    } else if (magic == 3) {
        v[7] = 60;
    }
    if (tjs_nums(ctx, v, nargs[magic], argc, argv)) {
        return JS_EXCEPTION;
    }
    for (int i = 0; i < 10; i++) {
        v[i] = isfinite(v[i]) ? v[i] : 0;
    }
#define B(i) tjs_clampi(v[i], 0, 255)
#define Q(i) tjs_clampi(v[i], 0, 100)
    Tvg_Result r;
    switch (magic) {
    case 0:
        r = tvg_scene_clear_effects(w->p);
        break;
    case 1: /* sigma, direction, border, quality */
        r = tvg_scene_add_effect_gaussian_blur(w->p, v[0], tjs_clampi(v[1], 0, 2), tjs_clampi(v[2], 0, 1), Q(3));
        break;
    case 2: /* distance, angle, quality */
        r = tvg_scene_add_effect_motion_blur(w->p, v[0], v[1], Q(2));
        break;
    case 3: /* r, g, b, a, angle, distance, sigma, quality */
        r = tvg_scene_add_effect_drop_shadow(w->p, B(0), B(1), B(2), B(3), v[4], v[5], v[6], Q(7));
        break;
    case 4: /* r, g, b, a */
        r = tvg_scene_add_effect_fill(w->p, B(0), B(1), B(2), B(3));
        break;
    case 5: /* black rgb, white rgb, intensity 0..100 */
        r = tvg_scene_add_effect_tint(w->p, B(0), B(1), B(2), B(3), B(4), B(5), Q(6));
        break;
    default: /* shadow rgb, midtone rgb, highlight rgb, blend */
        r = tvg_scene_add_effect_tritone(w->p, B(0), B(1), B(2), B(3), B(4), B(5), B(6), B(7), B(8), B(9));
        break;
    }
#undef B
#undef Q
    TJS_TRY(ctx, r, ops[magic]);
    return tjs_this(ctx, this_val);
}

static const JSCFunctionListEntry scene_proto[] = {
    JS_CFUNC_DEF("add", 2, scene_add),
    JS_CFUNC_DEF("remove", 1, scene_remove),
    JS_CFUNC_DEF("clear", 0, scene_remove),
    JS_CFUNC_MAGIC_DEF("resetEffects", 0, scene_effect, 0),
    JS_CFUNC_MAGIC_DEF("gaussianBlur", 4, scene_effect, 1),
    JS_CFUNC_MAGIC_DEF("motionBlur", 3, scene_effect, 2),
    JS_CFUNC_MAGIC_DEF("dropShadow", 8, scene_effect, 3),
    JS_CFUNC_MAGIC_DEF("fillEffect", 4, scene_effect, 4),
    JS_CFUNC_MAGIC_DEF("tint", 7, scene_effect, 5),
    JS_CFUNC_MAGIC_DEF("tritone", 10, scene_effect, 6),
};

/* ---- Picture ---- */

static int bpp(Tvg_Colorspace cs)
{
    return cs == TVG_COLORSPACE_GRAYSCALE8 ? 1 : cs == TVG_COLORSPACE_RGB565 ? 2 : 4;
}

/* raw pixels: ThorVG only takes 32-bit ones, read as uint32_t, so aligned too */
static int load_raw(JSContext *ctx, Tvg_Paint p, JSValueConst data, JSValueConst opts)
{
    int32_t wh[2] = { 0, 0 }, cs = TVG_COLORSPACE_ARGB8888;
    if (tjs_opt_int(ctx, opts, "width", &wh[0]) || tjs_opt_int(ctx, opts, "height", &wh[1]) ||
        tjs_opt_int(ctx, opts, "colorSpace", &cs)) {
        return -1;
    }
    if (wh[0] <= 0 || wh[1] <= 0) {
        JS_ThrowTypeError(ctx, "load: width and height are required for raw images");
        return -1;
    }
    if (bpp(cs) != 4) {
        JS_ThrowRangeError(ctx, "load: raw images must be a 32-bit color space");
        return -1;
    }
    size_t len;
    const uint8_t *buf = tjs_bytes(ctx, data, &len);
    if (!buf) {
        return -1;
    }
    /* 64-bit: on a 32-bit size_t 32768 x 32768 x 4 is 0 and would pass */
    if ((uintptr_t)buf % 4 || len < (uint64_t)wh[0] * wh[1] * 4) {
        JS_ThrowRangeError(ctx, "load: raw data too short or not 4-byte aligned");
        return -1;
    }
    TJS_CHECK(ctx, tvg_picture_load_raw(p, (const uint32_t *)buf, wh[0], wh[1], cs, true), "load");
    return 0;
}

/* always copies: the JS buffer can go away while the picture lives on */
static JSValue picture_load(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    PAINT(TJS_PICTURE);
    JSValueConst data = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValueConst opts = argc > 1 ? argv[1] : JS_UNDEFINED;
    char type[16] = "svg";

    if (JS_IsObject(opts)) {
        JSValue t = JS_GetPropertyStr(ctx, opts, "type");
        const char *s = JS_IsUndefined(t) ? NULL : JS_ToCString(ctx, t);
        JS_FreeValue(ctx, t);
        if (s) {
            snprintf(type, sizeof(type), "%s", s);
            JS_FreeCString(ctx, s);
        } else if (JS_HasException(ctx)) {
            return JS_EXCEPTION;
        }
    }
    if (!strcmp(type, "raw")) {
        return TJS_RET(ctx, load_raw(ctx, w->p, data, opts));
    }

    size_t len;
    Tvg_Result r;
    if (JS_IsString(data)) {
        const char *s = JS_ToCStringLen(ctx, &len, data);
        if (!s) {
            return JS_EXCEPTION;
        }
        r = tvg_picture_load_data(w->p, s, len, type, "", true);
        JS_FreeCString(ctx, s);
    } else {
        const uint8_t *buf = tjs_bytes(ctx, data, &len);
        if (!buf) {
            return JS_EXCEPTION;
        }
        r = tvg_picture_load_data(w->p, (const char *)buf, len, type, "", true);
    }
    TJS_TRY(ctx, r, "load");
    return tjs_this(ctx, this_val);
}

/* not in webcanvas: straight from the filesystem, no second copy in JS heap */
static JSValue picture_load_file(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    PAINT(TJS_PICTURE);
    const char *path = tjs_str(ctx, argc, argv, 0);
    if (!path) {
        return JS_EXCEPTION;
    }
    Tvg_Result r = tvg_picture_load(w->p, path);
    JS_FreeCString(ctx, path);
    TJS_TRY(ctx, r, "loadFile");
    return tjs_this(ctx, this_val);
}

static JSValue picture_size(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    PAINT(TJS_PICTURE);
    float v[2] = { 0, 0 };
    if (arg_set(argc, argv, 0) && arg_set(argc, argv, 1)) {
        if (tjs_nums(ctx, v, 2, argc, argv)) {
            return JS_EXCEPTION;
        }
        TJS_TRY(ctx, tvg_picture_set_size(w->p, v[0], v[1]), "size (set)");
        return tjs_this(ctx, this_val);
    }
    tvg_picture_get_size(w->p, &v[0], &v[1]);
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "width", JS_NewFloat64(ctx, v[0]));
    JS_SetPropertyStr(ctx, o, "height", JS_NewFloat64(ctx, v[1]));
    return o;
}

static JSValue picture_origin(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    PAINT(TJS_PICTURE);
    float v[2] = { 0, 0 };
    if (tjs_nums(ctx, v, 2, argc, argv)) {
        return JS_EXCEPTION;
    }
    TJS_TRY(ctx, tvg_picture_set_origin(w->p, v[0], v[1]), "origin");
    return tjs_this(ctx, this_val);
}

static JSValue picture_paint(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    PAINT(TJS_PICTURE);
    uint32_t id;
    if (to_id(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &id)) {
        return JS_EXCEPTION;
    }
    Tvg_Paint p = tvg_picture_get_paint(w->p, id);
    return p ? tjs_wrap_paint(ctx, p) : JS_NULL;
}

static JSValue picture_filter(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    PAINT(TJS_PICTURE);
    int32_t m = TVG_FILTER_METHOD_BILINEAR;
    if (tjs_ints(ctx, &m, 1, argc, argv)) {
        return JS_EXCEPTION;
    }
    TJS_TRY(ctx, tvg_picture_set_filter(w->p, m), "filter");
    return tjs_this(ctx, this_val);
}

static JSValue picture_get_accessible(JSContext *ctx, JSValueConst this_val)
{
    PAINT(TJS_PICTURE);
    return JS_NewBool(ctx, w->accessible);
}

static JSValue picture_set_accessible(JSContext *ctx, JSValueConst this_val, JSValueConst val)
{
    PAINT(TJS_PICTURE);
    bool on = JS_ToBool(ctx, val);
    TJS_TRY(ctx, tvg_picture_set_accessible(w->p, on), "accessible");
    w->accessible = on;
    return JS_UNDEFINED;
}

/*
 * No resolver(): ThorVG may call it from a loader worker thread, and JS can
 * only run on the JS task. Embed assets in the SVG/Lottie instead.
 */
static const JSCFunctionListEntry picture_proto[] = {
    JS_CFUNC_DEF("load", 2, picture_load),
    JS_CFUNC_DEF("loadFile", 1, picture_load_file),
    JS_CFUNC_DEF("size", 2, picture_size),
    JS_CFUNC_DEF("origin", 2, picture_origin),
    JS_CFUNC_DEF("paint", 1, picture_paint),
    JS_CFUNC_DEF("filter", 1, picture_filter),
    JS_CGETSET_DEF("accessible", picture_get_accessible, picture_set_accessible),
};

/* ---- Text ---- */

/* magic: 0 font, 1 text */
static JSValue text_str(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    PAINT(TJS_TEXT);
    const char *s = tjs_str(ctx, argc, argv, 0);
    if (!s) {
        return JS_EXCEPTION;
    }
    Tvg_Result r = magic ? tvg_text_set_text(w->p, s) : tvg_text_set_font(w->p, s);
    JS_FreeCString(ctx, s);
    TJS_TRY(ctx, r, magic ? "text" : "font");
    tjs_layout_text_changed(w);
    return tjs_this(ctx, this_val);
}

/* magic: 0 fontSize, 1 align, 2 layout, 3 spacing, 4 italic */
static JSValue text_nums(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    static const char *const ops[] = { "fontSize", "align", "layout", "spacing", "italic" };
    PAINT(TJS_TEXT);
    float v[2] = { magic == 4 ? 0.18f : 0, 0 };
    if (tjs_nums(ctx, v, 2, argc, argv)) {
        return JS_EXCEPTION;
    }
    Tvg_Result r;
    switch (magic) {
    case 0: r = tvg_text_set_size(w->p, v[0]); break;
    case 1: r = tvg_text_align(w->p, v[0], v[1]); break;
    case 2: r = tvg_text_layout(w->p, v[0], v[1]); break;
    case 3: r = tvg_text_spacing(w->p, v[0], v[1]); break;
    default: r = tvg_text_set_italic(w->p, v[0]); break;
    }
    TJS_TRY(ctx, r, ops[magic]);
    if (magic == 1) {
        w->text.align[0] = v[0];
        w->text.align[1] = v[1];
    } else if (magic == 3) {
        w->text.line_spacing = v[1];
    }
    if (magic != 2) {   /* a Layout overrides layout() anyway */
        tjs_layout_text_changed(w);
    }
    return tjs_this(ctx, this_val);
}

static JSValue text_fill(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    PAINT(TJS_TEXT);
    if (argc > 0 && tjs_is_fill(argv[0])) {
        Tvg_Gradient g = tjs_fill_dup(ctx, argv[0]);
        if (!g) {
            return JS_EXCEPTION;
        }
        Tvg_Result r = tvg_text_set_gradient(w->p, g);
        if (r != TVG_RESULT_SUCCESS) {
            tvg_gradient_del(g);
        }
        TJS_TRY(ctx, r, "fill (gradient)");
        return tjs_this(ctx, this_val);
    }
    int32_t c[3] = { 0, 0, 0 };
    if (argc < 3 || !JS_IsNumber(argv[0])) {
        return JS_ThrowTypeError(ctx, "Invalid fill arguments");
    }
    if (tjs_ints(ctx, c, 3, argc, argv)) {
        return JS_EXCEPTION;
    }
    TJS_TRY(ctx, tvg_text_set_color(w->p, tjs_u8(c[0]), tjs_u8(c[1]), tjs_u8(c[2])), "fill (color)");
    return tjs_this(ctx, this_val);
}

static JSValue text_wrap(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    PAINT(TJS_TEXT);
    int32_t m = 0;
    if (tjs_ints(ctx, &m, 1, argc, argv)) {
        return JS_EXCEPTION;
    }
    TJS_TRY(ctx, tvg_text_wrap_mode(w->p, m), "wrap");
    w->text.wrap = m != TVG_TEXT_WRAP_NONE;
    tjs_layout_text_changed(w);
    return tjs_this(ctx, this_val);
}

static JSValue text_lines(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    PAINT(TJS_TEXT);
    return JS_NewUint32(ctx, tvg_text_line_count(w->p));
}

static JSValue text_metrics(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    PAINT(TJS_TEXT);
    JSValue o;
    if (!arg_set(argc, argv, 0)) {
        Tvg_Text_Metrics m = { 0 };
        TJS_TRY(ctx, tvg_text_get_text_metrics(w->p, &m), "metrics");
        o = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, o, "ascent", JS_NewFloat64(ctx, m.ascent));
        JS_SetPropertyStr(ctx, o, "descent", JS_NewFloat64(ctx, m.descent));
        JS_SetPropertyStr(ctx, o, "linegap", JS_NewFloat64(ctx, m.linegap));
        JS_SetPropertyStr(ctx, o, "advance", JS_NewFloat64(ctx, m.advance));
        return o;
    }
    const char *ch = JS_ToCString(ctx, argv[0]);
    if (!ch) {
        return JS_EXCEPTION;
    }
    Tvg_Glyph_Metrics g = { 0 };
    Tvg_Result r = tvg_text_get_glyph_metrics(w->p, ch, &g, NULL);
    JS_FreeCString(ctx, ch);
    TJS_TRY(ctx, r, "metrics");
    o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "advance", JS_NewFloat64(ctx, g.advance));
    JS_SetPropertyStr(ctx, o, "bearing", JS_NewFloat64(ctx, g.bearing));
    JS_SetPropertyStr(ctx, o, "min", xy(ctx, g.min.x, g.min.y));
    JS_SetPropertyStr(ctx, o, "max", xy(ctx, g.max.x, g.max.y));
    return o;
}

static JSValue text_outline(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    PAINT(TJS_TEXT);
    float v[4] = { 0, 0, 0, 0 };
    if (tjs_nums(ctx, v, 4, argc, argv)) {
        return JS_EXCEPTION;
    }
    TJS_TRY(ctx, tvg_text_set_outline(w->p, v[0], tjs_clampi(v[1], 0, 255), tjs_clampi(v[2], 0, 255),
                                      tjs_clampi(v[3], 0, 255)), "outline");
    return tjs_this(ctx, this_val);
}

static const JSCFunctionListEntry text_proto[] = {
    JS_CFUNC_MAGIC_DEF("font", 1, text_str, 0),
    JS_CFUNC_MAGIC_DEF("text", 1, text_str, 1),
    JS_CFUNC_MAGIC_DEF("fontSize", 1, text_nums, 0),
    JS_CFUNC_MAGIC_DEF("align", 2, text_nums, 1),
    JS_CFUNC_MAGIC_DEF("layout", 2, text_nums, 2),
    JS_CFUNC_MAGIC_DEF("spacing", 2, text_nums, 3),
    JS_CFUNC_MAGIC_DEF("italic", 1, text_nums, 4),
    JS_CFUNC_DEF("fill", 3, text_fill),
    JS_CFUNC_DEF("wrap", 1, text_wrap),
    JS_CFUNC_DEF("lines", 0, text_lines),
    JS_CFUNC_DEF("metrics", 1, text_metrics),
    JS_CFUNC_DEF("outline", 4, text_outline),
};

int tjs_init_paints(JSContext *ctx, JSValueConst ns)
{
    static const tjs_class_t base = TJS_CLASS("Paint", -1, NULL, 0, paint_proto, NULL, NULL);
    static const tjs_class_t defs[] = {
        TJS_CLASS("Shape", TJS_SHAPE, paint_ctor, 0, shape_proto, paint_finalizer, NULL),
        TJS_CLASS("Scene", TJS_SCENE, paint_ctor, 0, scene_proto, paint_finalizer, NULL),
        TJS_CLASS("Picture", TJS_PICTURE, paint_ctor, 0, picture_proto, paint_finalizer, NULL),
        TJS_CLASS("Text", TJS_TEXT, paint_ctor, 0, text_proto, paint_finalizer, NULL),
    };
    JSValue paint = tjs_def_class(ctx, &base, JS_UNDEFINED);
    if (JS_IsException(paint)) {
        return -1;
    }
    for (size_t i = 0; i < countof(defs); i++) {
        JSValue c = tjs_def_class(ctx, &defs[i], paint);
        if (JS_IsException(c)) {
            JS_FreeValue(ctx, paint);
            return -1;
        }
        JS_SetPropertyStr(ctx, ns, defs[i].name, c);
    }
    JS_SetPropertyStr(ctx, ns, "Paint", paint);
    return 0;
}
