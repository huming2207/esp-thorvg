/*
 * Bits shared by the binding's source files.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/queue.h>
#include "sdkconfig.h"
#include "tvg_js.h"

#define countof(a) (sizeof(a) / sizeof((a)[0]))

#define TJS_MAX_STOPS 32    /* gradient stops, copied through the stack */
#define TJS_MAX_DASH 16     /* stroke dash pattern entries */
#define TJS_MAX_REGIONS 16  /* dirty regions passed to present(), more -> one full region */

enum {
    /* paints first, tjs_is_paint() relies on the order */
    TJS_SHAPE, TJS_SCENE, TJS_PICTURE, TJS_TEXT,
    TJS_LINEAR, TJS_RADIAL, TJS_CONIC,
    TJS_ANIM, TJS_LOTTIE,
    TJS_CANVAS, TJS_ACCESSOR, TJS_STATE,
    TJS_CLASS_MAX
};

/* opaque of every Paint subclass */
typedef struct {
    Tvg_Paint p;        /* JS owns one ref, NULL once disposed */
    bool accessible;    /* only meaningful for Picture, ThorVG has no getter */
} tjs_paint_t;

typedef struct tjs_anim {
    Tvg_Animation a;    /* NULL once disposed */
    JSValue pic;        /* cached Picture wrapper */
    JSValue on_frame;
    TAILQ_ENTRY(tjs_anim) link;
    float total, duration, cur;
    int64_t last_us;
    uint32_t pin;       /* key in the anchor while playing, 0 = not playing */
    bool loop, fire, ended;
} tjs_anim_t;

/*
 * One binding per process: ThorVG's loaders and fonts are global, and a
 * static struct keeps this auditable. Class IDs are allocated fresh for each
 * context that binds, so they're always valid for its runtime.
 */
typedef struct {
    JSContext *ctx;         /* bound context, NULL when free */
    JSValue anchor;         /* borrowed; owned by init()'s function data */
    JSClassID ids[TJS_CLASS_MAX];
    TAILQ_HEAD(, tjs_anim) playing;
    uint32_t pin_seq;
} tjs_state_t;

extern tjs_state_t tjs;

/*
 * Errors: a ThorVG failure goes to init({onError}) if set, else it throws.
 * Either way the operation stops there, nothing after the failed call runs.
 * TJS_TRY is for methods: handled -> return this, so chains keep going.
 * TJS_CHECK is for int helpers: 0 ok, -1 thrown, 1 handled.
 */
int tjs_fail(JSContext *ctx, Tvg_Result res, const char *op);
#define TJS_TRY(ctx, expr, op) do { \
        Tvg_Result r_ = (expr); \
        if (r_ != TVG_RESULT_SUCCESS) \
            return tjs_fail(ctx, r_, op) ? JS_EXCEPTION : JS_DupValue(ctx, this_val); \
    } while (0)
#define TJS_CHECK(ctx, expr, op) do { \
        Tvg_Result r_ = (expr); \
        if (r_ != TVG_RESULT_SUCCESS) \
            return tjs_fail(ctx, r_, op) ? -1 : 1; \
    } while (0)
/* what a method returns after an int helper: thrown, handled (stop, chain on) or ok */
#define TJS_RET(ctx, r) ((r) < 0 ? JS_EXCEPTION : JS_DupValue(ctx, this_val))

/* args: missing/undefined entries keep whatever the caller put in out[] */
int tjs_nums(JSContext *ctx, float *out, int n, int argc, JSValueConst *argv);
int tjs_ints(JSContext *ctx, int32_t *out, int n, int argc, JSValueConst *argv);
int tjs_opt_num(JSContext *ctx, JSValueConst obj, const char *key, float *out);
int tjs_opt_int(JSContext *ctx, JSValueConst obj, const char *key, int32_t *out);
const uint8_t *tjs_bytes(JSContext *ctx, JSValueConst v, size_t *len);
int tjs_color(JSContext *ctx, JSValueConst arr, int32_t rgba[4]);
uint8_t tjs_u8(int32_t v);
int tjs_clampi(float v, int lo, int hi);    /* NaN -> lo; for anything ThorVG takes as int */
/* argv[i] as a C string; TypeError if missing instead of the string "undefined" */
const char *tjs_str(JSContext *ctx, int argc, JSValueConst *argv, int i);

/* classes */
JSValue tjs_new_obj(JSContext *ctx, JSValueConst new_target, int cls);
typedef struct {
    const char *name;
    int cls;                    /* -1: abstract base, no instances */
    JSCFunctionMagic *ctor;     /* called with cls as magic */
    int argc;
    const JSCFunctionListEntry *fns;
    int nfns;
    JSClassFinalizer *finalizer;
    JSClassGCMark *gc_mark;
} tjs_class_t;

#define TJS_CLASS(n, c, ct, ac, f, fin, mark) { n, c, ct, ac, f, countof(f), fin, mark }

JSValue tjs_def_class(JSContext *ctx, const tjs_class_t *c, JSValueConst parent);
JSValue tjs_this(JSContext *ctx, JSValueConst this_val);

/* paints (tvg_js_paint.c) */
tjs_paint_t *tjs_paint(JSContext *ctx, JSValueConst v, int cls); /* cls -1 = any paint */
JSValue tjs_wrap_paint(JSContext *ctx, Tvg_Paint p);
/* would making `from` a child / clipper / mask of `to` create a cycle? */
bool tjs_reaches(Tvg_Paint from, Tvg_Paint to);
int tjs_init_paints(JSContext *ctx, JSValueConst ns);

/* fills (tvg_js_fill.c), tjs_fill_dup() hands out a copy the caller owns */
bool tjs_is_fill(JSValueConst v);
Tvg_Gradient tjs_fill_dup(JSContext *ctx, JSValueConst v);
int tjs_init_fills(JSContext *ctx, JSValueConst ns);

/* animations (tvg_js_anim.c) */
int tjs_anim_tick(JSContext *ctx);
void tjs_anim_drop_all(void);
int tjs_init_anims(JSContext *ctx, JSValueConst ns);

/* canvas, font, accessor (tvg_js_canvas.c) */
int tjs_init_canvas(JSContext *ctx, JSValueConst ns);
void tjs_surfaces_abandon(void);    /* every canvas is gone: no surface has users */

/* every native object a wrapper owns, so tvg_js_abandon() can free them. Untrack before freeing */
enum { TJS_NAT_PAINT, TJS_NAT_ACCESSOR, TJS_NAT_FILL, TJS_NAT_ANIM, TJS_NAT_CANVAS, TJS_NAT_KINDS };
void tjs_nat_track(int kind, void *ptr);
void tjs_nat_untrack(void *ptr);
