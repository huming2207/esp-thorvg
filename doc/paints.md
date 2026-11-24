# Paints

`Shape`, `Scene`, `Picture` and `Text` all extend `Paint`. Almost every method returns `this`, so
you chain:

```js
canvas.add(new TVG.Shape().appendRect(10, 10, 100, 50, { rx: 8 }).fill(255, 0, 0).translate(20, 0));
```

`Text` has its own page: [text.md](text.md).

## Paint (all of them)

| Call | Does |
|---|---|
| `translate(x, y)` | Move. Replaces the previous translation, it doesn't add up. |
| `rotate(deg)` | Rotate around the origin. |
| `scale(s)` | Uniform scale. |
| `transform({ e11, e12, e13, e21, e22, e23, e31, e32, e33 })` | Full 3x3 matrix, missing entries = identity. |
| `opacity(0..255)` / `opacity()` | Set / get. |
| `visible(bool)` / `visible()` | Set / get. Invisible paints cost nothing to render. |
| `blend(TVG.BlendMethod.X)` | `Normal`, `Multiply`, `Screen`, ... |
| `bounds()` | `{ x, y, width, height }` on the canvas, transforms included. |
| `bounds({ oriented: true })` | The 4 corners `[{ x, y }, ...]` of the rotated box. |
| `intersects(x, y, w, h, visibleOnly?)` | Hit test against a rectangle. Use it for touch. |
| `clip(shape)` / `clip(null)` | Clip to a `Shape`. Only Shapes, anything else throws. |
| `mask(paint, TVG.MaskMethod.X)` / `mask(null, TVG.MaskMethod.None)` | Mask by another paint. |
| `duplicate()` | Deep copy, new wrapper. |
| `id` | Get/set. Takes a number or a string (hashed the same way as `Accessor.id()`). |
| `dispose()` / `isDisposed` | Free it now. Using it afterwards throws `TypeError`. |

Anything that would put a paint inside itself (a scene in its own children, a paint as its own
clip or mask, a cycle through masks) throws a `RangeError`. ThorVG wouldn't catch those, it'd just
recurse until the stack blows.

## Shape

```js
const s = new TVG.Shape()
    .moveTo(0, 0).lineTo(100, 0).cubicTo(120, 0, 120, 40, 100, 40).close()
    .appendRect(0, 50, 80, 30, { rx: 6, ry: 6 })
    .appendCircle(150, 60, 20)              // appendCircle(cx, cy, rx, ry = rx, clockwise = true)
    .fill(0, 0, 255)                         // fill(r, g, b, a = 255) or fill(gradient)
    .stroke({ width: 3, color: [0, 0, 0] });
```

- `appendRect(x, y, w, h, { rx, ry, clockwise })`
- `appendPath(commands, points)`: `commands` are `TVG.PathCommand.MoveTo/LineTo/CubicTo/Close`,
  `points` are `[x, y]` pairs. Not enough points for the commands throws a `RangeError`.
- `path()` gives back `{ commands, points }`.
- `fillRule(TVG.FillRule.Winding | EvenOdd)`
- `stroke(width)` or `stroke({ width, color: [r, g, b, a], gradient, cap, join, miterLimit, dash: [on, off, ...], dashOffset })`.
  Only what you pass gets changed.
- `trimPath(begin = 0, end = 1, simultaneous = true)`: draw part of the path, for "drawing in" lines.
- `order(strokeFirst)`: stroke under the fill instead of over it.
- `reset()`: drop the path, keep the style.

## Scene

A group. Transform, opacity, clip and mask on a scene apply to everything in it.

```js
const card = new TVG.Scene().add(background).add(title);
card.translate(40, 40);
```

- `add(paint, before?)`: append, or insert in front of `before` (must be a child, `RangeError`
  otherwise).
- `remove(paint?)` / `clear()`: take one out, or all.
- Effects: `gaussianBlur(sigma, direction, border, quality)`, `motionBlur(distance, angle, quality)`,
  `dropShadow(r, g, b, a, angle, distance, sigma, quality)`, `fillEffect(r, g, b, a)`,
  `tint(blackR, blackG, blackB, whiteR, whiteG, whiteB, intensity)`,
  `tritone(shadow rgb, midtone rgb, highlight rgb, blend)`, `resetEffects()`.

**Effects crash on an RGB565 target**, because ThorVG's effect code assumes 32-bit pixels. They're
fine on ABGR8888/ARGB8888. They're also expensive: each one allocates `max(w, h)^2 * 4` bytes of
scratch (2.5 MB at 800x480) and ThorVG doesn't check whether that allocation worked. On an
embedded screen, skip them.

Keep a JS reference to scenes you still use. Dropping the last reference to a `Scene` wrapper frees
it and detaches its children, even if it's still on a canvas through its parent.

## Picture

SVG, Lottie (as a still frame), PNG/JPG/WebP (if those loaders are on), or raw pixels.

```js
const pic = new TVG.Picture().load(svgBytes, { type: 'svg' });   // type defaults to 'svg'
const { width, height } = pic.size();
pic.size(200, 200 * height / width).translate(10, 10);
```

- `load(data, { type })`: `data` is a string, `ArrayBuffer` or typed array. Always copied, so your
  buffer can go away right after. `type` is one of `svg` (`svg+xml`), `lot` (`lottie+json`),
  `png`, `jpg` (`jpeg`), `webp`.
- `load(pixels, { type: 'raw', width, height, colorSpace })`: raw 32-bit pixels, 4-byte aligned.
- `loadFile(path)`: straight from the filesystem, no copy in the JS heap. Better for big files.
- `size(w, h)` / `size()`: scale to a size / get the current size.
- `origin(x, y)`: the point that `translate()` moves (0..1, 0.5 = centre).
- `filter(TVG.FilterMethod.Bilinear | Nearest)`: scaling filter for bitmaps.
- `paint(id)`: a paint inside the picture by id (SVG `id`s), or `null`.
- `accessible = true` before `load()` makes ThorVG keep an id map, so `paint(id)` and the
  `Accessor` name lookups are fast.

## Accessor: walk a tree

```js
let shapes = 0;
new TVG.Accessor().set(pic, (p) => {
    shapes++;
    return true;            // false stops the walk
});
const id = TVG.Accessor.id('logo');     // same hash as the `id` property
```

Don't add or remove paints from inside the callback. `name(id)` gives the name back for an id.
