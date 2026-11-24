/*
 * Layout: Yoga flexbox node that can drive one Paint. Usage in README.md.
 *
 * Yoga calls abort() on any broken invariant (no exceptions here), so every
 * call it could assert on gets checked first and thrown as a JS error instead.
 * A parent holds a JS ref on each child, a node holds one on its paint.
 *
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <string.h>
#include <yoga/Yoga.h>
#include "tvg_js_priv.h"

struct tjs_layout {
    YGNodeRef node;
    JSValue self;       /* weak, our own wrapper */
    JSValue paint;      /* strong */
    tjs_paint_t *pw;    /* whichever finalizer runs first clears this */
    bool text;
    /* last values pushed to the paint. ThorVG dirties a Text on every align()/layout()
     * call even if nothing changed, so only push changes or every frame is all dirty */
    float x, y, w, h;
    int visible;        /* -1 unknown */
    bool touched;       /* measure_text() messed with box/align, apply() must redo them */
};

#define LAYOUT() \
    tjs_layout_t *l = layout_get(ctx, this_val); \
    if (!l) return JS_EXCEPTION

static tjs_layout_t *layout_get(JSContext *ctx, JSValueConst v)
{
    tjs_layout_t *l = JS_GetOpaque(v, tjs.ids[TJS_LAYOUT]);
    if (!l) {
        JS_ThrowTypeError(ctx, "not a Layout");
        return NULL;
    }
    if (!l->node) {
        JS_ThrowTypeError(ctx, "Object has been disposed");
        return NULL;
    }
    return l;
}

static tjs_layout_t *layout_of(YGNodeConstRef n)
{
    return YGNodeGetContext(n);
}


/* widest line's ink, but whole line heights, so 一 and 夏 get the same height */
static YGSize measure_text(YGNodeConstRef node, float width, YGMeasureMode wmode, float height,
                           YGMeasureMode hmode)
{
    YGSize size = { 0, 0 };
    tjs_layout_t *l = layout_of(node);
    if (!l || !l->pw || !l->pw->p) {
        return size;
    }
    tjs_paint_t *w = l->pw;
    Tvg_Paint p = w->p;
    bool bounded = wmode != YGMeasureModeUndefined && isfinite(width) && width > 0;

    /* measure left aligned with no box height; apply() restores both */
    tvg_text_align(p, 0, 0);
    tvg_text_layout(p, (bounded && w->text.wrap) ? width : 0, 0);
    l->touched = true;

    /* AABB includes the paint's own translation */
    Tvg_Matrix m = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    float x = 0, y = 0, bw = 0, bh = 0;
    tvg_paint_get_transform(p, &m);
    if (tvg_paint_get_aabb(p, &x, &y, &bw, &bh) == TVG_RESULT_SUCCESS && bw > 0) {
        size.width = ceilf(x + bw - m.e13);
    }
    /* same formula as ThorVG's sfnt loader uses for the text box */
    Tvg_Text_Metrics tm = { 0 };
    uint32_t lines = tvg_text_line_count(p);
    if (tvg_text_get_text_metrics(p, &tm) == TVG_RESULT_SUCCESS && lines > 0) {
        size.height = ceilf((tm.advance * lines - tm.linegap) * w->text.line_spacing);
    }

    if (wmode == YGMeasureModeExactly) {
        size.width = width;
    } else if (wmode == YGMeasureModeAtMost && size.width > width) {
        size.width = width;
    }
    if (hmode == YGMeasureModeExactly) {
        size.height = height;
    } else if (hmode == YGMeasureModeAtMost && size.height > height) {
        size.height = height;
    }
    return size;
}

void tjs_layout_text_changed(tjs_paint_t *w)
{
    if (w->layout && w->layout->text && YGNodeHasMeasureFunc(w->layout->node)) {
        YGNodeMarkDirty(w->layout->node);
    }
}

/* tvg_js_abandon(): the wrapper is gone with the dropped heap, free the bare node */
void tjs_layout_free_native(void *node)
{
    YGNodeSetContext(node, NULL);
    YGNodeFree(node);
}

void tjs_layout_paint_gone(tjs_paint_t *w)
{
    if (w->layout) {
        w->layout->pw = NULL;
        w->layout = NULL;
    }
}


static void unbind(JSRuntime *rt, tjs_layout_t *l)
{
    if (l->text) {
        YGNodeSetMeasureFunc(l->node, NULL);
        l->text = false;
    }
    if (l->pw) {
        l->pw->layout = NULL;
        l->pw = NULL;
    }
    JSValue p = l->paint;
    l->paint = JS_UNDEFINED;
    JS_FreeValueRT(rt, p);
}

static int bind(JSContext *ctx, tjs_layout_t *l, JSValueConst v)
{
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        unbind(JS_GetRuntime(ctx), l);
        return 0;
    }
    JSClassID cid;
    tjs_paint_t *w = tjs_paint(ctx, v, -1);
    if (!w) {
        return -1;
    }
    JS_GetAnyOpaque(v, &cid);
    bool text = cid == tjs.ids[TJS_TEXT];
    if (w->layout && w->layout != l) {
        JS_ThrowRangeError(ctx, "Layout: that paint already belongs to another layout node");
        return -1;
    }
    if (text && YGNodeGetChildCount(l->node) > 0) {
        JS_ThrowRangeError(ctx, "Layout: a node with children can't be bound to a Text");
        return -1;
    }
    unbind(JS_GetRuntime(ctx), l);
    l->paint = JS_DupValue(ctx, v);
    l->pw = w;
    w->layout = l;
    l->text = text;
    l->x = l->y = l->w = l->h = NAN;  /* force a full push on the next apply() */
    l->visible = -1;
    l->touched = text;
    if (text) {
        YGNodeSetMeasureFunc(l->node, measure_text);
        YGNodeMarkDirty(l->node);
    }
    return 0;
}


typedef struct {
    const char *name;
    const char *const *values;  /* Yoga's enum order */
    int nvalues;
    int def;                    /* Yoga default, for null/undefined */
    void (*set)(YGNodeRef, int);
} enum_prop_t;

#define ENUM_SETTER(fn, type) \
    static void set_##fn(YGNodeRef n, int v) { YGNodeStyleSet##fn(n, (type)v); }
ENUM_SETTER(Direction, YGDirection)
ENUM_SETTER(FlexDirection, YGFlexDirection)
ENUM_SETTER(JustifyContent, YGJustify)
ENUM_SETTER(AlignContent, YGAlign)
ENUM_SETTER(AlignItems, YGAlign)
ENUM_SETTER(AlignSelf, YGAlign)
ENUM_SETTER(PositionType, YGPositionType)
ENUM_SETTER(FlexWrap, YGWrap)
ENUM_SETTER(Overflow, YGOverflow)
ENUM_SETTER(Display, YGDisplay)
ENUM_SETTER(BoxSizing, YGBoxSizing)

static const char *const v_direction[] = { "inherit", "ltr", "rtl" };
static const char *const v_flex_dir[] = { "column", "column-reverse", "row", "row-reverse" };
static const char *const v_justify[] = {
    "flex-start", "center", "flex-end", "space-between", "space-around", "space-evenly",
};
static const char *const v_align[] = {
    "auto", "flex-start", "center", "flex-end", "stretch", "baseline",
    "space-between", "space-around", "space-evenly",
};
static const char *const v_position[] = { "static", "relative", "absolute" };
static const char *const v_wrap[] = { "nowrap", "wrap", "wrap-reverse" };
static const char *const v_overflow[] = { "visible", "hidden", "scroll" };
static const char *const v_display[] = { "flex", "none", "contents" };
static const char *const v_box[] = { "border-box", "content-box" };

#define EP(n, vals, def, fn) { n, vals, countof(vals), def, set_##fn }
static const enum_prop_t enum_props[] = {
    EP("direction", v_direction, YGDirectionInherit, Direction),
    EP("flexDirection", v_flex_dir, YGFlexDirectionColumn, FlexDirection),
    EP("justifyContent", v_justify, YGJustifyFlexStart, JustifyContent),
    EP("alignContent", v_align, YGAlignFlexStart, AlignContent),
    EP("alignItems", v_align, YGAlignStretch, AlignItems),
    EP("alignSelf", v_align, YGAlignAuto, AlignSelf),
    EP("position", v_position, YGPositionTypeRelative, PositionType),
    EP("flexWrap", v_wrap, YGWrapNoWrap, FlexWrap),
    EP("overflow", v_overflow, YGOverflowVisible, Overflow),
    EP("display", v_display, YGDisplayFlex, Display),
    EP("boxSizing", v_box, YGBoxSizingBorderBox, BoxSizing),
};

typedef enum { V_UNSET, V_PX, V_PCT, V_AUTO } vkind_t;

static int parse_value(JSContext *ctx, JSValueConst v, const char *key, vkind_t *kind, float *out)
{
    *out = YGUndefined;
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        *kind = V_UNSET;
        return 0;
    }
    if (JS_IsNumber(v)) {
        double d;
        JS_ToFloat64(ctx, &d, v);
        if (!isfinite(d)) {
            JS_ThrowRangeError(ctx, "Layout: %s must be finite", key);
            return -1;
        }
        *kind = V_PX;
        *out = (float)d;
        return 0;
    }
    if (JS_IsString(v)) {
        const char *s = JS_ToCString(ctx, v);
        if (!s) {
            return -1;
        }
        int ret = 0;
        size_t len = strlen(s);
        char *end = NULL;
        if (!strcmp(s, "auto")) {
            *kind = V_AUTO;
        } else if (len > 1 && s[len - 1] == '%' && (*out = strtof(s, &end), end == s + len - 1) &&
                   isfinite(*out)) {
            *kind = V_PCT;
        } else {
            JS_ThrowTypeError(ctx, "Layout: bad %s '%s' (number, 'N%%' or 'auto')", key, s);
            ret = -1;
        }
        JS_FreeCString(ctx, s);
        return ret;
    }
    JS_ThrowTypeError(ctx, "Layout: bad %s (number, 'N%%' or 'auto')", key);
    return -1;
}

enum { A_PX = 1, A_PCT = 2, A_AUTO = 4 };

typedef struct {
    const char *name;
    int allow;
    void (*set)(YGNodeRef, int edge, vkind_t, float);
    int edge;   /* YGEdge, YGGutter, or 0 */
} len_prop_t;

static void set_width(YGNodeRef n, int e, vkind_t k, float v)
{
    k == V_AUTO ? YGNodeStyleSetWidthAuto(n) : k == V_PCT ? YGNodeStyleSetWidthPercent(n, v) : YGNodeStyleSetWidth(n, v);
}
static void set_height(YGNodeRef n, int e, vkind_t k, float v)
{
    k == V_AUTO ? YGNodeStyleSetHeightAuto(n) : k == V_PCT ? YGNodeStyleSetHeightPercent(n, v) : YGNodeStyleSetHeight(n, v);
}
static void set_basis(YGNodeRef n, int e, vkind_t k, float v)
{
    k == V_AUTO ? YGNodeStyleSetFlexBasisAuto(n) : k == V_PCT ? YGNodeStyleSetFlexBasisPercent(n, v) : YGNodeStyleSetFlexBasis(n, v);
}
#define MINMAX_SETTER(fn) \
    static void set_##fn(YGNodeRef n, int e, vkind_t k, float v) \
    { k == V_PCT ? YGNodeStyleSet##fn##Percent(n, v) : YGNodeStyleSet##fn(n, v); }
MINMAX_SETTER(MinWidth)
MINMAX_SETTER(MinHeight)
MINMAX_SETTER(MaxWidth)
MINMAX_SETTER(MaxHeight)
static void set_margin(YGNodeRef n, int e, vkind_t k, float v)
{
    k == V_AUTO ? YGNodeStyleSetMarginAuto(n, e) : k == V_PCT ? YGNodeStyleSetMarginPercent(n, e, v) : YGNodeStyleSetMargin(n, e, v);
}
static void set_padding(YGNodeRef n, int e, vkind_t k, float v)
{
    k == V_PCT ? YGNodeStyleSetPaddingPercent(n, e, v) : YGNodeStyleSetPadding(n, e, v);
}
static void set_border(YGNodeRef n, int e, vkind_t k, float v)
{
    YGNodeStyleSetBorder(n, e, v);
}
static void set_inset(YGNodeRef n, int e, vkind_t k, float v)
{
    k == V_AUTO ? YGNodeStyleSetPositionAuto(n, e) : k == V_PCT ? YGNodeStyleSetPositionPercent(n, e, v) : YGNodeStyleSetPosition(n, e, v);
}
static void set_gap(YGNodeRef n, int g, vkind_t k, float v)
{
    k == V_PCT ? YGNodeStyleSetGapPercent(n, g, v) : YGNodeStyleSetGap(n, g, v);
}

#define ALL_EDGES(prefix, allow, fn) \
    { prefix, allow, fn, YGEdgeAll }, \
    { prefix "Top", allow, fn, YGEdgeTop }, { prefix "Right", allow, fn, YGEdgeRight }, \
    { prefix "Bottom", allow, fn, YGEdgeBottom }, { prefix "Left", allow, fn, YGEdgeLeft }, \
    { prefix "Start", allow, fn, YGEdgeStart }, { prefix "End", allow, fn, YGEdgeEnd }, \
    { prefix "Horizontal", allow, fn, YGEdgeHorizontal }, { prefix "Vertical", allow, fn, YGEdgeVertical }

static const len_prop_t len_props[] = {
    { "width", A_PX | A_PCT | A_AUTO, set_width, 0 },
    { "height", A_PX | A_PCT | A_AUTO, set_height, 0 },
    { "flexBasis", A_PX | A_PCT | A_AUTO, set_basis, 0 },
    { "minWidth", A_PX | A_PCT, set_MinWidth, 0 },
    { "minHeight", A_PX | A_PCT, set_MinHeight, 0 },
    { "maxWidth", A_PX | A_PCT, set_MaxWidth, 0 },
    { "maxHeight", A_PX | A_PCT, set_MaxHeight, 0 },
    ALL_EDGES("margin", A_PX | A_PCT | A_AUTO, set_margin),
    ALL_EDGES("padding", A_PX | A_PCT, set_padding),
    ALL_EDGES("border", A_PX, set_border),
    { "inset", A_PX | A_PCT | A_AUTO, set_inset, YGEdgeAll },
    { "top", A_PX | A_PCT | A_AUTO, set_inset, YGEdgeTop },
    { "right", A_PX | A_PCT | A_AUTO, set_inset, YGEdgeRight },
    { "bottom", A_PX | A_PCT | A_AUTO, set_inset, YGEdgeBottom },
    { "left", A_PX | A_PCT | A_AUTO, set_inset, YGEdgeLeft },
    { "start", A_PX | A_PCT | A_AUTO, set_inset, YGEdgeStart },
    { "end", A_PX | A_PCT | A_AUTO, set_inset, YGEdgeEnd },
    { "gap", A_PX | A_PCT, set_gap, YGGutterAll },
    { "rowGap", A_PX | A_PCT, set_gap, YGGutterRow },
    { "columnGap", A_PX | A_PCT, set_gap, YGGutterColumn },
};

typedef struct {
    const char *name;
    void (*set)(YGNodeRef, float);
} num_prop_t;

static const num_prop_t num_props[] = {
    { "flex", YGNodeStyleSetFlex },
    { "flexGrow", YGNodeStyleSetFlexGrow },
    { "flexShrink", YGNodeStyleSetFlexShrink },
    { "aspectRatio", YGNodeStyleSetAspectRatio },
};

static int set_prop(JSContext *ctx, tjs_layout_t *l, const char *key, JSValueConst v)
{
    for (size_t i = 0; i < countof(enum_props); i++) {
        const enum_prop_t *p = &enum_props[i];
        if (strcmp(key, p->name)) {
            continue;
        }
        int val = p->def;
        if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
            const char *s = JS_ToCString(ctx, v);
            if (!s) {
                return -1;
            }
            val = -1;
            for (int j = 0; j < p->nvalues; j++) {
                if (!strcmp(s, p->values[j])) {
                    val = j;
                }
            }
            if (val < 0) {
                JS_ThrowRangeError(ctx, "Layout: bad %s '%s'", key, s);
            }
            JS_FreeCString(ctx, s);
            if (val < 0) {
                return -1;
            }
        }
        p->set(l->node, val);
        return 0;
    }
    for (size_t i = 0; i < countof(len_props); i++) {
        const len_prop_t *p = &len_props[i];
        if (strcmp(key, p->name)) {
            continue;
        }
        vkind_t k;
        float f;
        if (parse_value(ctx, v, key, &k, &f)) {
            return -1;
        }
        if ((k == V_PCT && !(p->allow & A_PCT)) || (k == V_AUTO && !(p->allow & A_AUTO))) {
            JS_ThrowRangeError(ctx, "Layout: %s doesn't take %s", key, k == V_AUTO ? "'auto'" : "percentages");
            return -1;
        }
        p->set(l->node, p->edge, k, f);
        return 0;
    }
    for (size_t i = 0; i < countof(num_props); i++) {
        if (strcmp(key, num_props[i].name)) {
            continue;
        }
        vkind_t k;
        float f;
        if (parse_value(ctx, v, key, &k, &f)) {
            return -1;
        }
        if (k == V_PCT || k == V_AUTO) {
            JS_ThrowRangeError(ctx, "Layout: %s takes a number", key);
            return -1;
        }
        num_props[i].set(l->node, f);
        return 0;
    }
    JS_ThrowRangeError(ctx, "Layout: unknown style '%s'", key);
    return -1;
}

static int apply_style(JSContext *ctx, tjs_layout_t *l, JSValueConst obj)
{
    if (JS_IsUndefined(obj) || JS_IsNull(obj)) {
        return 0;
    }
    if (!JS_IsObject(obj)) {
        JS_ThrowTypeError(ctx, "Layout: style must be an object");
        return -1;
    }
    JSPropertyEnum *props = NULL;
    uint32_t n = 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &n, obj, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)) {
        return -1;
    }
    int ret = 0;
    for (uint32_t i = 0; i < n && !ret; i++) {
        const char *key = JS_AtomToCString(ctx, props[i].atom);
        JSValue v = key ? JS_GetProperty(ctx, obj, props[i].atom) : JS_EXCEPTION;
        ret = (!key || JS_IsException(v)) ? -1 : set_prop(ctx, l, key, v);
        JS_FreeValue(ctx, v);
        JS_FreeCString(ctx, key);
    }
    JS_FreePropertyEnum(ctx, props, n);
    return ret;
}


static void detach_children(JSRuntime *rt, tjs_layout_t *l)
{
    while (YGNodeGetChildCount(l->node) > 0) {
        YGNodeRef c = YGNodeGetChild(l->node, 0);
        YGNodeRemoveChild(l->node, c);
        tjs_layout_t *cl = layout_of(c);
        if (cl) {
            JS_FreeValueRT(rt, cl->self);
        }
    }
}

static void release(JSRuntime *rt, tjs_layout_t *l)
{
    if (!l->node) {
        return;
    }
    YGNodeRef owner = YGNodeGetOwner(l->node);
    if (owner) {
        YGNodeRemoveChild(owner, l->node);  /* only in a GC cycle, a live parent keeps us alive */
    }
    detach_children(rt, l);
    unbind(rt, l);
    tjs_nat_untrack(l->node);
    YGNodeFree(l->node);
    l->node = NULL;
}

static void layout_finalizer(JSRuntime *rt, JSValueConst val)
{
    tjs_layout_t *l = JS_GetOpaque(val, tjs.ids[TJS_LAYOUT]);
    if (l) {
        release(rt, l);
        js_free_rt(rt, l);
    }
}

static void layout_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark)
{
    tjs_layout_t *l = JS_GetOpaque(val, tjs.ids[TJS_LAYOUT]);
    if (!l || !l->node) {
        return;
    }
    JS_MarkValue(rt, l->paint, mark);
    size_t n = YGNodeGetChildCount(l->node);
    for (size_t i = 0; i < n; i++) {
        tjs_layout_t *cl = layout_of(YGNodeGetChild(l->node, i));
        if (cl) {
            JS_MarkValue(rt, cl->self, mark);
        }
    }
}

/* new Layout(style?, paint?) */
static JSValue layout_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv, int magic)
{
    JSValue obj = tjs_new_obj(ctx, new_target, TJS_LAYOUT);
    if (JS_IsException(obj)) {
        return obj;
    }
    tjs_layout_t *l = js_mallocz(ctx, sizeof(*l));
    if (!l) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    l->node = YGNodeNew();
    tjs_nat_track(TJS_NAT_LAYOUT, l->node);
    l->self = obj;
    l->paint = JS_UNDEFINED;
    YGNodeSetContext(l->node, l);
    JS_SetOpaque(obj, l);
    if ((argc > 0 && apply_style(ctx, l, argv[0])) || (argc > 1 && bind(ctx, l, argv[1]))) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

static JSValue layout_style(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LAYOUT();
    if (apply_style(ctx, l, argc > 0 ? argv[0] : JS_UNDEFINED)) {
        return JS_EXCEPTION;
    }
    return tjs_this(ctx, this_val);
}

static JSValue layout_get_paint(JSContext *ctx, JSValueConst this_val)
{
    LAYOUT();
    return JS_DupValue(ctx, l->paint);
}

static JSValue layout_set_paint(JSContext *ctx, JSValueConst this_val, JSValueConst v)
{
    LAYOUT();
    return bind(ctx, l, v) ? JS_EXCEPTION : JS_UNDEFINED;
}

static bool is_ancestor(YGNodeRef anc, YGNodeRef n)
{
    for (; n; n = YGNodeGetOwner(n)) {
        if (n == anc) {
            return true;
        }
    }
    return false;
}

static JSValue layout_add(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LAYOUT();
    tjs_layout_t *c = layout_get(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (!c) {
        return JS_EXCEPTION;
    }
    size_t n = YGNodeGetChildCount(l->node);
    size_t at = n;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        tjs_layout_t *b = layout_get(ctx, argv[1]);
        if (!b) {
            return JS_EXCEPTION;
        }
        if (YGNodeGetOwner(b->node) != l->node) {
            return JS_ThrowRangeError(ctx, "add: 'before' isn't a child of this layout");
        }
        for (at = 0; at < n && YGNodeGetChild(l->node, at) != b->node; at++) {
        }
    }
    /* all of these abort inside Yoga */
    if (YGNodeGetOwner(c->node)) {
        return JS_ThrowRangeError(ctx, "add: that layout already has a parent, remove it first");
    }
    if (YGNodeHasMeasureFunc(l->node)) {
        return JS_ThrowRangeError(ctx, "add: a layout bound to a Text can't have children");
    }
    if (is_ancestor(c->node, l->node)) {
        return JS_ThrowRangeError(ctx, "add: that would put the layout inside itself");
    }
    YGNodeInsertChild(l->node, c->node, at);
    JS_DupValue(ctx, c->self);
    return tjs_this(ctx, this_val);
}

/* remove(child), or remove() for all */
static JSValue layout_remove(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LAYOUT();
    if (argc < 1 || JS_IsUndefined(argv[0])) {
        detach_children(JS_GetRuntime(ctx), l);
        return tjs_this(ctx, this_val);
    }
    tjs_layout_t *c = layout_get(ctx, argv[0]);
    if (!c) {
        return JS_EXCEPTION;
    }
    if (YGNodeGetOwner(c->node) != l->node) {
        return JS_ThrowRangeError(ctx, "remove: not a child of this layout");
    }
    YGNodeRemoveChild(l->node, c->node);
    JSValue ret = tjs_this(ctx, this_val);
    JS_FreeValue(ctx, c->self);     /* may finalize c */
    return ret;
}

static JSValue layout_count(JSContext *ctx, JSValueConst this_val)
{
    LAYOUT();
    return JS_NewUint32(ctx, (uint32_t)YGNodeGetChildCount(l->node));
}

static void apply(tjs_layout_t *l)
{
    YGNodeRef n = l->node;
    YGNodeSetHasNewLayout(n, false);
    if (l->pw && l->pw->p) {
        Tvg_Paint p = l->pw->p;
        float x = YGNodeLayoutGetLeft(n), y = YGNodeLayoutGetTop(n);
        float w = YGNodeLayoutGetWidth(n), h = YGNodeLayoutGetHeight(n);
        int visible = YGNodeStyleGetDisplay(n) != YGDisplayNone;
        if (visible != l->visible) {
            tvg_paint_set_visible(p, visible);
            l->visible = visible;
        }
        if (x != l->x || y != l->y) {
            tvg_paint_translate(p, x, y);
            l->x = x;
            l->y = y;
        }
        if (l->text && (l->touched || w != l->w || h != l->h)) {
            tvg_text_layout(p, w, h);
            tvg_text_align(p, l->pw->text.align[0], l->pw->text.align[1]);
            l->w = w;
            l->h = h;
            l->touched = false;
        }
    }
    size_t cnt = YGNodeGetChildCount(n);
    for (size_t i = 0; i < cnt; i++) {
        tjs_layout_t *cl = layout_of(YGNodeGetChild(n, i));
        if (cl) {
            apply(cl);
        }
    }
}

/* calculate(width?, height?), default: the root's own style size */
static JSValue layout_calculate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LAYOUT();
    if (YGNodeGetOwner(l->node)) {
        return JS_ThrowRangeError(ctx, "calculate: call it on the root layout");
    }
    float wh[2] = { YGUndefined, YGUndefined };
    if (tjs_nums(ctx, wh, 2, argc, argv)) {
        return JS_EXCEPTION;
    }
    YGNodeCalculateLayout(l->node, wh[0], wh[1], YGDirectionLTR);
    apply(l);
    return tjs_this(ctx, this_val);
}

static JSValue layout_computed(JSContext *ctx, JSValueConst this_val)
{
    LAYOUT();
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "left", JS_NewFloat64(ctx, YGNodeLayoutGetLeft(l->node)));
    JS_SetPropertyStr(ctx, o, "top", JS_NewFloat64(ctx, YGNodeLayoutGetTop(l->node)));
    JS_SetPropertyStr(ctx, o, "width", JS_NewFloat64(ctx, YGNodeLayoutGetWidth(l->node)));
    JS_SetPropertyStr(ctx, o, "height", JS_NewFloat64(ctx, YGNodeLayoutGetHeight(l->node)));
    return o;
}

static JSValue layout_dirty(JSContext *ctx, JSValueConst this_val)
{
    LAYOUT();
    return JS_NewBool(ctx, YGNodeIsDirty(l->node));
}

/* Yoga aborts on markDirty() of an unmeasured node; those never need it anyway */
static JSValue layout_mark_dirty(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    LAYOUT();
    if (YGNodeHasMeasureFunc(l->node)) {
        YGNodeMarkDirty(l->node);
    }
    return tjs_this(ctx, this_val);
}

static JSValue layout_dispose(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    tjs_layout_t *l = JS_GetOpaque(this_val, tjs.ids[TJS_LAYOUT]);
    if (l && l->node) {
        if (YGNodeGetOwner(l->node)) {
            return JS_ThrowRangeError(ctx, "dispose: remove the layout from its parent first");
        }
        release(JS_GetRuntime(ctx), l);
    }
    return JS_UNDEFINED;
}

static JSValue layout_disposed(JSContext *ctx, JSValueConst this_val)
{
    tjs_layout_t *l = JS_GetOpaque(this_val, tjs.ids[TJS_LAYOUT]);
    return JS_NewBool(ctx, !l || !l->node);
}

static const JSCFunctionListEntry layout_proto[] = {
    JS_CFUNC_DEF("style", 1, layout_style),
    JS_CGETSET_DEF("paint", layout_get_paint, layout_set_paint),
    JS_CFUNC_DEF("add", 2, layout_add),
    JS_CFUNC_DEF("remove", 1, layout_remove),
    JS_CGETSET_DEF("childCount", layout_count, NULL),
    JS_CFUNC_DEF("calculate", 2, layout_calculate),
    JS_CGETSET_DEF("layout", layout_computed, NULL),
    JS_CGETSET_DEF("isDirty", layout_dirty, NULL),
    JS_CFUNC_DEF("markDirty", 0, layout_mark_dirty),
    JS_CFUNC_DEF("dispose", 0, layout_dispose),
    JS_CGETSET_DEF("isDisposed", layout_disposed, NULL),
};

int tjs_init_layout(JSContext *ctx, JSValueConst ns)
{
    static const tjs_class_t def =
        TJS_CLASS("Layout", TJS_LAYOUT, layout_ctor, 2, layout_proto, layout_finalizer, layout_mark);
    JSValue c = tjs_def_class(ctx, &def, JS_UNDEFINED);
    if (JS_IsException(c)) {
        return -1;
    }
    JS_SetPropertyStr(ctx, ns, "Layout", c);
    return 0;
}
