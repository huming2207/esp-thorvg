# Gradients

```js
const g = new TVG.LinearGradient(0, 0, 200, 0)          // x1, y1, x2, y2
    .addStop(0, [255, 0, 0, 255])
    .addStop(1, [0, 0, 255, 255]);
shape.fill(g);
```

- `new LinearGradient(x1, y1, x2, y2)`
- `new RadialGradient(cx, cy, r, fx?, fy?, fr?)`: optional focal point and focal radius
- `new ConicGradient(cx, cy, angle?)`

| Call | Does |
|---|---|
| `addStop(offset, [r, g, b, a])` | offset 0..1 |
| `setStops([0, colour], [1, colour], ...)` | Replace all stops at once. |
| `clearStops()` | |
| `spread(TVG.GradientSpread.Pad \| Reflect \| Repeat)` | What happens past the ends. |
| `build()` | Does nothing, there for webcanvas compatibility. |
| `dispose()` / `isDisposed` | |

A gradient is a template. `fill(g)`, `stroke({ gradient: g })` and `Text.fill(g)` hand ThorVG a
copy, so you can reuse one gradient on as many paints as you like, and changing it afterwards
doesn't change what's already filled. Re-`fill()` if you want the new version.

Gradient coordinates are in the paint's own space, before its transform.

On palette displays gradients get dithered, which is usually what you want for big areas and ugly
for small ones. See [e-paper.md](e-paper.md).
