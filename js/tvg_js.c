/*
 * Module glue: binding state, errors, arg helpers, enums and init().
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include "tvg_js_priv.h"

tjs_state_t tjs = {
    .anchor = JS_UNDEFINED,
    .playing = TAILQ_HEAD_INITIALIZER(tjs.playing),
};

static const char *result_str(Tvg_Result r)
{
    switch (r) {
    case TVG_RESULT_SUCCESS: return "Success";
    case TVG_RESULT_INVALID_ARGUMENT: return "Invalid arguments";
    case TVG_RESULT_INSUFFICIENT_CONDITION: return "Insufficient condition";
    case TVG_RESULT_FAILED_ALLOCATION: return "Failed allocation";
    case TVG_RESULT_MEMORY_CORRUPTION: return "Memory corruption";
    case TVG_RESULT_NOT_SUPPORTED: return "Not supported";
    case TVG_RESULT_SYSTEM_ERROR: return "System error";
    default: return "Unknown error";
    }
}

int tjs_fail(JSContext *ctx, Tvg_Result res, const char *op)
{
    char msg[96];
    snprintf(msg, sizeof(msg), "TVGERR - %s failed: %s", op, result_str(res));

    JSValue ctor = JS_GetPropertyStr(ctx, tjs.anchor, "ThorVGError");
    JSValue args[3] = { JS_NewString(ctx, msg), JS_NewInt32(ctx, res), JS_NewString(ctx, op) };
    JSValue err = JS_CallConstructor(ctx, ctor, 3, args);
    JS_FreeValue(ctx, ctor);
    for (int i = 0; i < 3; i++) {
        JS_FreeValue(ctx, args[i]);
    }
    if (JS_IsException(err)) {
        return -1;
    }

    JSValue h = JS_GetPropertyStr(ctx, tjs.anchor, "onError");
    if (!JS_IsFunction(ctx, h)) {
        JS_FreeValue(ctx, h);
        JS_Throw(ctx, err);
        return -1;
    }
    JSValue info = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, info, "operation", JS_NewString(ctx, op));
    JSValue hargs[2] = { err, info };
    JSValue ret = JS_Call(ctx, h, JS_UNDEFINED, 2, hargs);
    JS_FreeValue(ctx, h);
    JS_FreeValue(ctx, err);
    JS_FreeValue(ctx, info);
    if (JS_IsException(ret)) {
        return -1;
    }
    JS_FreeValue(ctx, ret);
    return 0;
}

int tjs_nums(JSContext *ctx, float *out, int n, int argc, JSValueConst *argv)
{
    for (int i = 0; i < n && i < argc; i++) {
        double d;
        if (JS_IsUndefined(argv[i])) {
            continue;
        }
        if (JS_ToFloat64(ctx, &d, argv[i])) {
            return -1;
        }
        out[i] = (float)d;
    }
    return 0;
}

int tjs_ints(JSContext *ctx, int32_t *out, int n, int argc, JSValueConst *argv)
{
    for (int i = 0; i < n && i < argc; i++) {
        if (!JS_IsUndefined(argv[i]) && JS_ToInt32(ctx, &out[i], argv[i])) {
            return -1;
        }
    }
    return 0;
}

int tjs_opt_num(JSContext *ctx, JSValueConst obj, const char *key, float *out)
{
    if (!JS_IsObject(obj)) {
        return 0;
    }
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    int ret = JS_IsException(v) ? -1 : tjs_nums(ctx, out, 1, 1, (JSValueConst *)&v);
    JS_FreeValue(ctx, v);
    return ret;
}

int tjs_opt_int(JSContext *ctx, JSValueConst obj, const char *key, int32_t *out)
{
    if (!JS_IsObject(obj)) {
        return 0;
    }
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    int ret = JS_IsException(v) ? -1 : tjs_ints(ctx, out, 1, 1, (JSValueConst *)&v);
    JS_FreeValue(ctx, v);
    return ret;
}

/*
 * ArrayBuffer or any typed array; the pointer lives as long as v does.
 * NULL only with an exception set: an empty buffer has no data pointer in
 * QuickJS, so it gets a dummy one and *len = 0 (ThorVG then says no).
 */
const uint8_t *tjs_bytes(JSContext *ctx, JSValueConst v, size_t *len)
{
    static const uint8_t empty;
    const uint8_t *p;
    *len = 0;
    if (JS_IsArrayBuffer(v)) {
        p = JS_GetArrayBuffer(ctx, len, v);
    } else {
        size_t off, bpe, ab_len;
        JSValue ab = JS_GetTypedArrayBuffer(ctx, v, &off, len, &bpe);
        if (JS_IsException(ab)) {
            return NULL;
        }
        p = JS_GetArrayBuffer(ctx, &ab_len, ab);
        JS_FreeValue(ctx, ab);
        p = p ? p + off : NULL;
    }
    if (JS_HasException(ctx)) {
        return NULL;    /* detached */
    }
    if (!p) {
        *len = 0;
        p = &empty;
    }
    return p;
}

/* [r, g, b, a?], alpha defaults to opaque */
int tjs_color(JSContext *ctx, JSValueConst arr, int32_t rgba[4])
{
    rgba[0] = rgba[1] = rgba[2] = 0;
    rgba[3] = 255;
    for (int i = 0; i < 4; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, arr, i);
        int bad = JS_IsException(v) ? -1 : tjs_ints(ctx, &rgba[i], 1, 1, (JSValueConst *)&v);
        JS_FreeValue(ctx, v);
        if (bad) {
            return -1;
        }
    }
    return 0;
}

uint8_t tjs_u8(int32_t v)
{
    return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
}

int tjs_clampi(float v, int lo, int hi)
{
    return !(v >= lo) ? lo : v > hi ? hi : (int)v;     /* NaN fails v >= lo */
}

const char *tjs_str(JSContext *ctx, int argc, JSValueConst *argv, int i)
{
    if (argc <= i || JS_IsUndefined(argv[i]) || JS_IsNull(argv[i])) {
        JS_ThrowTypeError(ctx, "argument %d: expected a string", i + 1);
        return NULL;
    }
    return JS_ToCString(ctx, argv[i]);
}

/* honours new.target so `class Foo extends Shape` gets the right prototype */
JSValue tjs_new_obj(JSContext *ctx, JSValueConst new_target, int cls)
{
    JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(proto)) {
        return proto;
    }
    if (!JS_IsObject(proto)) {      /* GetPrototypeFromConstructor: fall back to ours */
        JS_FreeValue(ctx, proto);
        return JS_NewObjectClass(ctx, tjs.ids[cls]);
    }
    JSValue obj = JS_NewObjectProtoClass(ctx, proto, tjs.ids[cls]);
    JS_FreeValue(ctx, proto);
    return obj;
}

static JSValue abstract_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv,
                             int magic)
{
    return JS_ThrowTypeError(ctx, "abstract class");
}

/*
 * Class + constructor + prototype. parent is the base constructor (or
 * undefined); both chains get linked so instanceof and statics work.
 */
JSValue tjs_def_class(JSContext *ctx, const tjs_class_t *c, JSValueConst parent)
{
    if (c->cls >= 0) {
        JSRuntime *rt = JS_GetRuntime(ctx);
        JSClassDef cd = { c->name, .finalizer = c->finalizer, .gc_mark = c->gc_mark };
        JS_NewClassID(rt, &tjs.ids[c->cls]);
        if (JS_NewClass(rt, tjs.ids[c->cls], &cd)) {
            return JS_ThrowOutOfMemory(ctx);
        }
    }
    JSValue proto = JS_NewObject(ctx);
    JSValue fn = JS_NewCFunctionMagic(ctx, c->ctor ? c->ctor : abstract_ctor, c->name, c->argc,
                                      JS_CFUNC_constructor_magic, c->cls);
    JS_SetPropertyFunctionList(ctx, proto, c->fns, c->nfns);
    if (!JS_IsUndefined(parent)) {
        JSValue pp = JS_GetPropertyStr(ctx, parent, "prototype");
        JS_SetPrototype(ctx, proto, pp);
        JS_SetPrototype(ctx, fn, parent);
        JS_FreeValue(ctx, pp);
    }
    JS_SetConstructor(ctx, fn, proto);
    if (c->cls >= 0) {
        JS_SetClassProto(ctx, tjs.ids[c->cls], proto);
    } else {
        JS_FreeValue(ctx, proto);
    }
    return fn;
}

JSValue tjs_this(JSContext *ctx, JSValueConst this_val)
{
    return JS_DupValue(ctx, this_val);
}

/* ---- enums, same names and values as webcanvas' constants.ts ---- */

#define E(n, v) JS_PROP_INT32_DEF(n, v, JS_PROP_ENUMERABLE)

static const JSCFunctionListEntry blend_method[] = {
    E("Normal", 0), E("Multiply", 1), E("Screen", 2), E("Overlay", 3), E("Darken", 4),
    E("Lighten", 5), E("ColorDodge", 6), E("ColorBurn", 7), E("HardLight", 8),
    E("SoftLight", 9), E("Difference", 10), E("Exclusion", 11), E("Hue", 12),
    E("Saturation", 13), E("Color", 14), E("Luminosity", 15), E("Add", 16),
    E("Composition", 255),
};
static const JSCFunctionListEntry stroke_cap[] = { E("Butt", 0), E("Round", 1), E("Square", 2) };
static const JSCFunctionListEntry stroke_join[] = { E("Miter", 0), E("Round", 1), E("Bevel", 2) };
static const JSCFunctionListEntry fill_rule[] = { E("Winding", 0), E("EvenOdd", 1) };
static const JSCFunctionListEntry path_command[] = {
    E("Close", 0), E("MoveTo", 1), E("LineTo", 2), E("CubicTo", 3),
};
static const JSCFunctionListEntry gradient_spread[] = { E("Pad", 0), E("Reflect", 1), E("Repeat", 2) };
static const JSCFunctionListEntry mask_method[] = {
    E("None", 0), E("Alpha", 1), E("InvAlpha", 2), E("Luma", 3), E("InvLuma", 4), E("Add", 5),
    E("Subtract", 6), E("Intersect", 7), E("Difference", 8), E("Lighten", 9), E("Darken", 10),
};
static const JSCFunctionListEntry engine_option[] = {
    E("None", 0), E("Default", 1), E("SmartRender", 2), E("Aliased", 4),
};
static const JSCFunctionListEntry scene_effect[] = {
    E("ClearAll", 0), E("GaussianBlur", 1), E("DropShadow", 2), E("Fill", 3), E("Tint", 4),
    E("Tritone", 5), E("MotionBlur", 6),
};
static const JSCFunctionListEntry text_wrap[] = {
    E("None", 0), E("Character", 1), E("Word", 2), E("Smart", 3), E("Ellipsis", 4),
};
/* RGB565 and the X variants exist in this ThorVG fork, webcanvas doesn't have them */
static const JSCFunctionListEntry color_space[] = {
    E("ABGR8888", 0), E("ARGB8888", 1), E("ABGR8888S", 2), E("ARGB8888S", 3),
    E("Grayscale8", 4), E("RGB565", 5), E("XBGR8888", 6), E("XRGB8888", 7), E("Unknown", 255),
};
static const JSCFunctionListEntry filter_method[] = { E("Bilinear", 0), E("Nearest", 1) };
static const JSCFunctionListEntry result_code[] = {
    E("Success", 0), E("InvalidArguments", 1), E("InsufficientCondition", 2),
    E("FailedAllocation", 3), E("MemoryCorruption", 4), E("NotSupported", 5),
    E("SystemError", 6), E("Unknown", 255),
};

#define ENUM(n, l) { n, l, countof(l) }
static const struct { const char *name; const JSCFunctionListEntry *list; int n; } enums[] = {
    ENUM("BlendMethod", blend_method), ENUM("StrokeCap", stroke_cap),
    ENUM("StrokeJoin", stroke_join), ENUM("FillRule", fill_rule),
    ENUM("PathCommand", path_command), ENUM("GradientSpread", gradient_spread),
    ENUM("MaskMethod", mask_method), ENUM("SceneEffect", scene_effect),
    ENUM("TextWrapMode", text_wrap), ENUM("ColorSpace", color_space),
    ENUM("FilterMethod", filter_method), ENUM("EngineOption", engine_option),
};

/* ---- init() / term() ---- */

/*
 * webcanvas loads the wasm here. We've nothing to load: the host already
 * started the engine, every renderer is the CPU one, and threads/locateFile
 * mean nothing. Only onError does something.
 */
static JSValue js_init(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                       int magic, JSValueConst *data)
{
    JSValueConst anchor = data[0];
    JSValueConst opts = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue h = JS_IsObject(opts) ? JS_GetPropertyStr(ctx, opts, "onError") : JS_UNDEFINED;
    if (JS_IsException(h)) {
        return h;
    }
    JS_SetPropertyStr(ctx, anchor, "onError", h);

    JSValue funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) {
        return promise;
    }
    JSValue ns = JS_GetPropertyStr(ctx, anchor, "ns");
    JSValue ret = JS_Call(ctx, funcs[0], JS_UNDEFINED, 1, (JSValueConst *)&ns);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, ns);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    return promise;
}

/* the host terminates the engine, after the context is gone */
static JSValue js_term(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    return JS_UNDEFINED;
}

static void state_finalizer(JSRuntime *rt, JSValueConst val)
{
    tjs_anim_drop_all();
    tjs.ctx = NULL;
    tjs.anchor = JS_UNDEFINED;
}

static const char error_src[] =
    "(class ThorVGError extends Error {\n"
    "  constructor(message, code, operation) {\n"
    "    super(message);\n"
    "    this.name = 'ThorVGError';\n"
    "    this.code = code;\n"
    "    this.operation = operation;\n"
    "  }\n"
    "})";

/* everything a JS user sees, minus init/default */
static const char *const exports[] = {
    "Paint", "Canvas", "Shape", "Scene", "Picture", "Text", "Animation", "LottieAnimation",
    "LinearGradient", "RadialGradient", "ConicGradient", "Font", "Accessor",
    "constants", "ThorVGResultCode", "ThorVGError",
};

static int bind_classes(JSContext *ctx)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    static const JSClassDef state_class = { "ThorVGState", .finalizer = state_finalizer };

    memset(tjs.ids, 0, sizeof(tjs.ids)); /* fresh IDs from this runtime */
    JS_NewClassID(rt, &tjs.ids[TJS_STATE]);
    if (JS_NewClass(rt, tjs.ids[TJS_STATE], &state_class)) {
        return -1;
    }
    JSValue anchor = JS_NewObjectClass(ctx, tjs.ids[TJS_STATE]);
    if (JS_IsException(anchor)) {
        return -1;
    }
    tjs.anchor = anchor;
    tjs.ctx = ctx;
    return 0;
}

static int module_init(JSContext *ctx, JSModuleDef *m)
{
    if (tjs.ctx) {
        JS_ThrowInternalError(ctx, "thorvg is already bound to another JS context");
        return -1;
    }
    if (bind_classes(ctx)) {
        return -1;
    }
    /* tjs.anchor's only owner is init()'s function data, created below */
    JSValue anchor = tjs.anchor;
    JSValue ns = JS_NewObject(ctx);
    JSValue err = JS_Eval(ctx, error_src, sizeof(error_src) - 1, "<thorvg>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(err)) {
        goto fail;
    }
    JS_SetPropertyStr(ctx, anchor, "ThorVGError", JS_DupValue(ctx, err));
    JS_SetPropertyStr(ctx, ns, "ThorVGError", err);

    if (tjs_init_paints(ctx, ns) || tjs_init_fills(ctx, ns) || tjs_init_anims(ctx, ns) ||
        tjs_init_canvas(ctx, ns)) {
        goto fail;
    }

    JSValue constants = JS_NewObject(ctx);
    for (size_t i = 0; i < countof(enums); i++) {
        JSValue e = JS_NewObject(ctx);
        JS_SetPropertyFunctionList(ctx, e, enums[i].list, enums[i].n);
        JS_SetPropertyStr(ctx, constants, enums[i].name, JS_DupValue(ctx, e));
        JS_SetPropertyStr(ctx, ns, enums[i].name, e);
    }
    JS_SetPropertyStr(ctx, ns, "constants", constants);
    JSValue rc = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, rc, result_code, countof(result_code));
    JS_SetPropertyStr(ctx, ns, "ThorVGResultCode", rc);

    const char *ver = NULL;
    tvg_engine_version(NULL, NULL, NULL, &ver);
    JS_SetPropertyStr(ctx, ns, "version", JS_NewString(ctx, ver ? ver : ""));
    JS_SetPropertyStr(ctx, ns, "term", JS_NewCFunction(ctx, js_term, "term", 0));

    for (size_t i = 0; i < countof(exports); i++) {
        JS_SetModuleExport(ctx, m, exports[i], JS_GetPropertyStr(ctx, ns, exports[i]));
    }
    JSValue init = JS_NewCFunctionData(ctx, js_init, 1, 0, 1, (JSValueConst *)&anchor);
    JSValue def = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, def, "init", JS_DupValue(ctx, init));
    JS_SetModuleExport(ctx, m, "default", def);
    JS_SetModuleExport(ctx, m, "init", init);
    JS_SetPropertyStr(ctx, anchor, "ns", ns);
    JS_FreeValue(ctx, anchor); /* init's data holds it now */
    return 0;

fail:
    JS_FreeValue(ctx, ns);
    JS_FreeValue(ctx, anchor); /* finalizer unbinds */
    return -1;
}

JSModuleDef *tvg_js_init_module(JSContext *ctx, const char *name)
{
    JSModuleDef *m = JS_NewCModule(ctx, name, module_init);
    if (!m) {
        return NULL;
    }
    for (size_t i = 0; i < countof(exports); i++) {
        JS_AddModuleExport(ctx, m, exports[i]);
    }
    JS_AddModuleExport(ctx, m, "init");
    JS_AddModuleExport(ctx, m, "default");
    return m;
}
