/*
 * Animation, LottieAnimation.
 *
 * webcanvas steps playing animations from requestAnimationFrame. There's no
 * such thing here, and a timer per animation would be silly, so playing ones
 * sit on a list and get stepped at the start of every Canvas.render(). The
 * frame maths is the same: frame += fps * dt, wrap or stop at the end. Your
 * render loop (setInterval or whatever) is the frame clock.
 *
 * A playing animation pins its JS object in the anchor, like the rAF closure
 * does in a browser, so `new Animation().play()` keeps going until paused.
 *
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <esp_timer.h>
#include "tvg_js_priv.h"

static tjs_anim_t *anim_get(JSContext *ctx, JSValueConst v, bool lottie)
{
    JSClassID cid;
    tjs_anim_t *a = JS_GetAnyOpaque(v, &cid);
    bool ok = cid && (cid == tjs.ids[TJS_LOTTIE] || (!lottie && cid == tjs.ids[TJS_ANIM]));
    if (!ok || !a) {
        JS_ThrowTypeError(ctx, "not a %s", lottie ? "LottieAnimation" : "Animation");
        return NULL;
    }
    if (!a->a) {
        JS_ThrowTypeError(ctx, "Object has been disposed");
        return NULL;
    }
    return a;
}

#define ANIM(lottie) \
    tjs_anim_t *a = anim_get(ctx, this_val, lottie); \
    if (!a) return JS_EXCEPTION

/* off the list and unpinned. Unpinning can free a, so it goes last */
static void unplay(JSContext *ctx, tjs_anim_t *a)
{
    if (!a->pin) {
        return;
    }
    uint32_t pin = a->pin;
    TAILQ_REMOVE(&tjs.playing, a, link);
    a->pin = 0;
    a->fire = a->ended = false;
    JSAtom k = JS_NewAtomUInt32(ctx, pin);
    JS_DeleteProperty(ctx, tjs.anchor, k, 0);
    JS_FreeAtom(ctx, k);
}

/* the anchor is going away with the context: just forget the list */
void tjs_anim_drop_all(void)
{
    tjs_anim_t *a;
    while ((a = TAILQ_FIRST(&tjs.playing))) {
        TAILQ_REMOVE(&tjs.playing, a, link);
        a->pin = 0;
    }
}

int tjs_anim_tick(JSContext *ctx)
{
    static bool ticking;    /* onFrame calling render() would tick again, forever */
    int64_t now = esp_timer_get_time();
    tjs_anim_t *a;

    if (ticking) {
        return 0;
    }
    /* no JS runs in this pass, so the list can't change under us */
    TAILQ_FOREACH(a, &tjs.playing, link) {
        float dt = a->last_us ? (float)(now - a->last_us) / 1e6f : 0;
        a->last_us = now;
        a->cur += a->total / a->duration * dt;
        if (a->cur >= a->total) {
            if (a->loop) {
                a->cur = fmodf(a->cur, a->total);
            } else {
                a->cur = fmaxf(0, a->total - 1);
                a->ended = true;
            }
        }
        tvg_animation_set_frame(a->a, a->cur);
        a->fire = !a->ended && !JS_IsUndefined(a->on_frame);
    }

    /* callbacks can pause/play/dispose anything, so rescan after each one */
    ticking = true;
    int ret = 0;
    for (;;) {
        TAILQ_FOREACH(a, &tjs.playing, link) {
            if (a->fire || a->ended) {
                break;
            }
        }
        if (!a) {
            break;
        }
        JSValue obj = JS_GetPropertyUint32(ctx, tjs.anchor, a->pin); /* keeps a alive */
        if (a->ended) {
            unplay(ctx, a);
            JS_FreeValue(ctx, obj);
            continue;
        }
        a->fire = false;
        JSValue fn = JS_DupValue(ctx, a->on_frame);
        JSValue arg = JS_NewFloat64(ctx, a->cur);
        JSValue r = JS_Call(ctx, fn, obj, 1, &arg);
        JS_FreeValue(ctx, fn);
        JS_FreeValue(ctx, obj);
        if (JS_IsException(r)) {
            ret = -1;
            break;
        }
        JS_FreeValue(ctx, r);
    }
    ticking = false;
    return ret;
}

static void anim_finalizer(JSRuntime *rt, JSValueConst val)
{
    JSClassID cid;
    tjs_anim_t *a = JS_GetAnyOpaque(val, &cid);
    if (!a) {
        return;
    }
    if (a->pin) {
        TAILQ_REMOVE(&tjs.playing, a, link);
    }
    JS_FreeValueRT(rt, a->pic);
    JS_FreeValueRT(rt, a->on_frame);
    if (a->a) {
        tjs_nat_untrack(a->a);
        tvg_animation_del(a->a);
    }
    js_free_rt(rt, a);
}

static void anim_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark)
{
    JSClassID cid;
    tjs_anim_t *a = JS_GetAnyOpaque(val, &cid);
    if (a) {
        JS_MarkValue(rt, a->pic, mark);
        JS_MarkValue(rt, a->on_frame, mark);
    }
}

/* magic: class */
static JSValue anim_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv, int magic)
{
    JSValue obj = tjs_new_obj(ctx, new_target, magic);
    if (JS_IsException(obj)) {
        return obj;
    }
    tjs_anim_t *a = js_mallocz(ctx, sizeof(*a));
    if (!a) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    a->pic = a->on_frame = JS_UNDEFINED;
    a->loop = true;
    JS_SetOpaque(obj, a);
    a->a = magic == TJS_LOTTIE ? tvg_lottie_animation_new() : tvg_animation_new();
    tjs_nat_track(TJS_NAT_ANIM, a->a);
    if (!a->a) {
        JS_FreeValue(ctx, obj);
        return JS_ThrowInternalError(ctx, "Failed to create animation (Lottie loader not built?)");
    }
    return obj;
}

static JSValue anim_picture(JSContext *ctx, JSValueConst this_val)
{
    ANIM(false);
    if (JS_IsUndefined(a->pic)) {
        a->pic = tjs_wrap_paint(ctx, tvg_animation_get_picture(a->a));
        if (JS_IsException(a->pic)) {
            a->pic = JS_UNDEFINED;
            return JS_EXCEPTION;
        }
    }
    return JS_DupValue(ctx, a->pic);
}

/* frame count and duration; a segment shrinks both, and frames count from its start */
static void refresh(tjs_anim_t *a)
{
    a->total = a->duration = 0;
    tvg_animation_get_total_frame(a->a, &a->total);
    tvg_animation_get_duration(a->a, &a->duration);
    a->cur = fmaxf(0, fminf(a->cur, a->total - 1));
}

static bool loaded(tjs_anim_t *a)
{
    return a->total > 0 && a->duration > 0;
}

static JSValue anim_load(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    ANIM(false);
    JSValueConst data = argc > 0 ? argv[0] : JS_UNDEFINED;
    Tvg_Paint pic = tvg_animation_get_picture(a->a);
    Tvg_Result r;
    size_t len;

    if (JS_IsString(data)) {
        const char *s = JS_ToCStringLen(ctx, &len, data);
        if (!s) {
            return JS_EXCEPTION;
        }
        r = tvg_picture_load_data(pic, s, len, "lot", "", true);
        JS_FreeCString(ctx, s);
    } else {
        const uint8_t *buf = tjs_bytes(ctx, data, &len);
        if (!buf) {
            return JS_EXCEPTION;
        }
        r = tvg_picture_load_data(pic, (const char *)buf, len, "lot", "", true);
    }
    TJS_TRY(ctx, r, "load");
    a->cur = 0;
    refresh(a);
    return tjs_this(ctx, this_val);
}

static JSValue anim_info(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    ANIM(false);
    if (!loaded(a)) {
        return JS_NULL;
    }
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "totalFrames", JS_NewFloat64(ctx, a->total));
    JS_SetPropertyStr(ctx, o, "duration", JS_NewFloat64(ctx, a->duration));
    JS_SetPropertyStr(ctx, o, "fps", JS_NewFloat64(ctx, a->total / a->duration));
    return o;
}

/* InsufficientCondition just means "same frame as before" */
static int set_frame(JSContext *ctx, tjs_anim_t *a, float no)
{
    if (!isfinite(no)) {    /* NaN would wedge playback: NaN >= total is never true */
        JS_ThrowRangeError(ctx, "frame: not a finite number");
        return -1;
    }
    Tvg_Result r = tvg_animation_set_frame(a->a, no);
    if (r != TVG_RESULT_INSUFFICIENT_CONDITION) {
        TJS_CHECK(ctx, r, "frame (set)");
    }
    a->cur = no;
    return 0;
}

static JSValue anim_frame(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    ANIM(false);
    if (argc < 1 || JS_IsUndefined(argv[0])) {
        return JS_NewFloat64(ctx, a->cur);
    }
    float no = 0;
    if (tjs_nums(ctx, &no, 1, argc, argv)) {
        return JS_EXCEPTION;
    }
    return TJS_RET(ctx, set_frame(ctx, a, no));
}

static JSValue anim_segment(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    ANIM(false);
    float v[2] = { 0, 0 };
    if (tjs_nums(ctx, v, 2, argc, argv)) {
        return JS_EXCEPTION;
    }
    TJS_TRY(ctx, tvg_animation_set_segment(a->a, v[0], v[1]), "segment");
    refresh(a);
    return tjs_this(ctx, this_val);
}

static JSValue anim_play(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    ANIM(false);
    if (a->pin) {
        return tjs_this(ctx, this_val);
    }
    if (!loaded(a)) {
        return JS_ThrowTypeError(ctx, "ERROR - play: No animation loaded");
    }
    JS_FreeValue(ctx, a->on_frame);
    a->on_frame = argc > 0 && JS_IsFunction(ctx, argv[0]) ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
    a->last_us = 0;
    if (++tjs.pin_seq == 0) {
        tjs.pin_seq = 1;
    }
    if (JS_SetPropertyUint32(ctx, tjs.anchor, tjs.pin_seq, JS_DupValue(ctx, this_val)) < 0) {
        return JS_EXCEPTION;
    }
    a->pin = tjs.pin_seq;
    TAILQ_INSERT_TAIL(&tjs.playing, a, link);
    return tjs_this(ctx, this_val);
}

/* magic: 0 pause, 1 stop */
static JSValue anim_pause(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    ANIM(false);
    unplay(ctx, a);
    return magic ? TJS_RET(ctx, set_frame(ctx, a, 0)) : tjs_this(ctx, this_val);
}

static JSValue anim_is_playing(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    ANIM(false);
    return JS_NewBool(ctx, a->pin != 0);
}

static JSValue anim_set_loop(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    ANIM(false);
    a->loop = argc > 0 && JS_ToBool(ctx, argv[0]);
    return tjs_this(ctx, this_val);
}

static JSValue anim_get_loop(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    ANIM(false);
    return JS_NewBool(ctx, a->loop);
}

/* time in seconds, like duration */
static JSValue anim_seek(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    ANIM(false);
    if (!loaded(a)) {
        return JS_ThrowTypeError(ctx, "ERROR - seek: No animation loaded");
    }
    float t = 0;
    if (tjs_nums(ctx, &t, 1, argc, argv)) {
        return JS_EXCEPTION;
    }
    float f = fmaxf(0, fminf(a->total - 1, t / a->duration * a->total));
    return TJS_RET(ctx, set_frame(ctx, a, f));
}

static JSValue anim_time(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    ANIM(false);
    return JS_NewFloat64(ctx, loaded(a) ? a->cur / a->total * a->duration : 0);
}

static JSValue anim_dispose(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSClassID cid;
    tjs_anim_t *a = JS_GetAnyOpaque(this_val, &cid);
    if (!a || (cid != tjs.ids[TJS_ANIM] && cid != tjs.ids[TJS_LOTTIE]) || !a->a) {
        return JS_UNDEFINED;
    }
    unplay(ctx, a);
    JS_FreeValue(ctx, a->on_frame);
    JS_FreeValue(ctx, a->pic); /* the wrapper keeps its own ref on the picture */
    a->on_frame = a->pic = JS_UNDEFINED;
    tjs_nat_untrack(a->a);
    tvg_animation_del(a->a);
    a->a = NULL;
    return JS_UNDEFINED;
}

static JSValue anim_disposed(JSContext *ctx, JSValueConst this_val)
{
    JSClassID cid;
    tjs_anim_t *a = JS_GetAnyOpaque(this_val, &cid);
    return JS_NewBool(ctx, !a || (cid != tjs.ids[TJS_ANIM] && cid != tjs.ids[TJS_LOTTIE]) || !a->a);
}

static const JSCFunctionListEntry anim_proto[] = {
    JS_CGETSET_DEF("picture", anim_picture, NULL),
    JS_CFUNC_DEF("load", 1, anim_load),
    JS_CFUNC_DEF("info", 0, anim_info),
    JS_CFUNC_DEF("frame", 1, anim_frame),
    JS_CFUNC_DEF("segment", 2, anim_segment),
    JS_CFUNC_DEF("play", 1, anim_play),
    JS_CFUNC_MAGIC_DEF("pause", 0, anim_pause, 0),
    JS_CFUNC_MAGIC_DEF("stop", 0, anim_pause, 1),
    JS_CFUNC_DEF("isPlaying", 0, anim_is_playing),
    JS_CFUNC_DEF("setLoop", 1, anim_set_loop),
    JS_CFUNC_DEF("getLoop", 0, anim_get_loop),
    JS_CFUNC_DEF("seek", 1, anim_seek),
    JS_CFUNC_DEF("getCurrentTime", 0, anim_time),
    JS_CFUNC_DEF("dispose", 0, anim_dispose),
    JS_CGETSET_DEF("isDisposed", anim_disposed, NULL),
};


/* segment(begin, end) or segment(marker | null) */
static JSValue lottie_segment(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    ANIM(true);
    if (argc > 0 && JS_IsNumber(argv[0])) {
        return anim_segment(ctx, this_val, argc, argv);
    }
    const char *m = NULL;
    if (argc > 0 && !JS_IsNull(argv[0]) && !JS_IsUndefined(argv[0])) {
        m = JS_ToCString(ctx, argv[0]);
        if (!m) {
            return JS_EXCEPTION;
        }
    }
    Tvg_Result r = tvg_lottie_animation_set_marker(a->a, m);
    JS_FreeCString(ctx, m);
    TJS_TRY(ctx, r, "segment (marker)");
    refresh(a);
    return tjs_this(ctx, this_val);
}

static JSValue lottie_markers_cnt(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    ANIM(true);
    uint32_t n = 0;
    TJS_TRY(ctx, tvg_lottie_animation_get_markers_cnt(a->a, &n), "markersCnt");
    return JS_NewUint32(ctx, n);
}

static JSValue lottie_marker(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    ANIM(true);
    uint32_t idx = 0;
    if (argc > 0 && JS_ToUint32(ctx, &idx, argv[0])) {
        return JS_EXCEPTION;
    }
    const char *name = NULL;
    float b = 0, e = 0;
    TJS_TRY(ctx, tvg_lottie_animation_get_marker_info(a->a, idx, &name, &b, &e), "marker");
    if (!name) {
        return JS_NULL;
    }
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, name));
    JS_SetPropertyStr(ctx, o, "begin", JS_NewFloat64(ctx, b));
    JS_SetPropertyStr(ctx, o, "end", JS_NewFloat64(ctx, e));
    return o;
}

/* slot data as a JSON string or anything JSON.stringify takes */
static JSValue lottie_gen(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    ANIM(true);
    JSValueConst v = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue json = JS_IsString(v) ? JS_DupValue(ctx, v) : JS_JSONStringify(ctx, v, JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(json)) {
        return json;
    }
    const char *s = JS_ToCString(ctx, json);
    JS_FreeValue(ctx, json);
    if (!s) {
        return JS_EXCEPTION;
    }
    uint32_t id = tvg_lottie_animation_gen_slot(a->a, s);
    JS_FreeCString(ctx, s);
    if (!id) {
        return JS_ThrowTypeError(ctx, "ERROR - gen: Failed to generate slot");
    }
    return JS_NewUint32(ctx, id);
}

/* magic: 0 apply, 1 del */
static JSValue lottie_slot(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    ANIM(true);
    uint32_t id = 0;
    if (argc > 0 && JS_ToUint32(ctx, &id, argv[0])) {
        return JS_EXCEPTION;
    }
    if (magic) {
        TJS_TRY(ctx, tvg_lottie_animation_del_slot(a->a, id), "del");
    } else {
        TJS_TRY(ctx, tvg_lottie_animation_apply_slot(a->a, id), "apply");
    }
    return tjs_this(ctx, this_val);
}

static JSValue lottie_quality(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    ANIM(true);
    float q = 0;
    if (tjs_nums(ctx, &q, 1, argc, argv)) {
        return JS_EXCEPTION;
    }
    q = roundf(fmaxf(0, fminf(100, q)));
    TJS_TRY(ctx, tvg_lottie_animation_set_quality(a->a, (uint8_t)q), "quality");
    return tjs_this(ctx, this_val);
}

/* no audio here, so no volume()/resolver() */
static const JSCFunctionListEntry lottie_proto[] = {
    JS_CFUNC_DEF("segment", 2, lottie_segment),
    JS_CFUNC_DEF("markersCnt", 0, lottie_markers_cnt),
    JS_CFUNC_DEF("marker", 1, lottie_marker),
    JS_CFUNC_DEF("gen", 1, lottie_gen),
    JS_CFUNC_MAGIC_DEF("apply", 1, lottie_slot, 0),
    JS_CFUNC_MAGIC_DEF("del", 1, lottie_slot, 1),
    JS_CFUNC_DEF("quality", 1, lottie_quality),
};

int tjs_init_anims(JSContext *ctx, JSValueConst ns)
{
    static const tjs_class_t anim = TJS_CLASS("Animation", TJS_ANIM, anim_ctor, 0, anim_proto,
                                              anim_finalizer, anim_mark);
    static const tjs_class_t lottie = TJS_CLASS("LottieAnimation", TJS_LOTTIE, anim_ctor, 0, lottie_proto,
                                                anim_finalizer, anim_mark);
    JSValue a = tjs_def_class(ctx, &anim, JS_UNDEFINED);
    if (JS_IsException(a)) {
        return -1;
    }
    JSValue l = tjs_def_class(ctx, &lottie, a);
    if (JS_IsException(l)) {
        JS_FreeValue(ctx, a);
        return -1;
    }
    JS_SetPropertyStr(ctx, ns, "Animation", a);
    JS_SetPropertyStr(ctx, ns, "LottieAnimation", l);
    return 0;
}
