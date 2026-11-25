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
| `transform({ e11, e12, e13, e21, e22, e23, e31, e32, e33 })` | Full 3x3 matrix, missing entries = identity. After this, `translate`/`rotate`/`scale` throw `InsufficientCondition` (paints out of an SVG already have one). |
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

**Effects eat memory, and running out crashes the chip.** They work on every target, RGB565 and
Grayscale8 included (those go through a 32-bit layer). But every buffer is a square of
`max(w, h)^2 * 4` bytes, so the blur can flip the image. At 1024x600 that's 4 MB each:

| Effect | Scratch at 1024x600 |
|---|---|
| `fillEffect`, `tint`, `tritone` | 4 MB |
| `gaussianBlur`, `motionBlur` | 8 MB |
| `dropShadow` | 12 MB |

ThorVG keeps those buffers until the canvas is destroyed, and it doesn't check whether the
allocation worked: when it fails, it draws through a NULL pointer and the chip reboots. Masks and
translucent scenes do the same with smaller (screen-sized) buffers. On an embedded screen, skip
effects unless you've checked the PSRAM budget.

Effects on a scene with `opacity()` below 255 come out slightly different on RGB565 and Grayscale8
than on 32-bit targets (up to ~30/255 on a drop shadow). That's ThorVG's direct and layered effect
paths disagreeing, not the colour format.

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
- `paint('name')`: a paint inside the picture by its SVG `id`, or `null`. Compares the name
  itself, see below.
- `paint(number)`: by numeric id, i.e. `Accessor.id('name')`. That's a hash, so it can collide.
- `accessible`: **on by default** here (off in webcanvas). ThorVG keeps an id-to-name table for
  SVGs, which makes `paint('name')` exact and fast. Set it to `false` before `load()` to skip the
  table; then `paint('name')` only compares hashes.

## Editing an SVG (templates)

Paints you get from `paint()` are live. Change them after the picture is on screen and `render()`
redraws just those:

```js
const pic = new TVG.Picture().load(svgBytes, { type: 'svg' });
canvas.add(pic);
pic.paint('title').text('消防演习通知');
pic.paint('alert').visible(false);
if (canvas.render().length) await refresh();
```

Why names and not numbers: ThorVG stores element ids as 32-bit djb2 hashes, and those collide
(`'Ab'` and `'BA'` hash the same). `paint('name')` compares the actual names, `paint(number)`
can hand you the wrong element without any error.

What the SVG needs:

- **Placeholder text.** An empty `<text id="x"/>` makes no paint, so there's nothing to find.
- **No `<tspan>`** in text you want to change. It turns the element into a `Scene`, not a `Text`.
- **`font-family` names a loaded font.** Otherwise ThorVG silently picks whatever font it has.
- **No text boxes.** SVG 1.1 text is just a point. Inkscape's flowed text and Figma's text boxes
  don't load. For wrapping, draw a `<rect id="body-box" fill="none">` where the text goes and
  wrap into it:

  ```js
  const b = pic.paint('body-box').bounds();
  pic.paint('body')
      .transform({ e13: b.x, e23: b.y })      // translate() throws on SVG paints, see below
      .layout(b.width, b.height)
      .wrap(TVG.TextWrapMode.Word)
      .text(longText);
  ```

Things that bite:

- `translate()`, `rotate()` and `scale()` throw `InsufficientCondition` on paints that came out
  of an SVG: they already have a full matrix. Use `transform({...})`, which replaces it.
- Wrapping knows nothing about CJK line-break rules, so a line can start with `，`.
- Every `paint()` call makes a new JS wrapper: `pic.paint('a') !== pic.paint('a')`. Keep the one
  you got.
- `Accessor.set()` on the picture walks the same paints, if you'd rather find things by a pattern.

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
