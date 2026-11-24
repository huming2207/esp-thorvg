# Canvas

## Making one

```js
const canvas = new TVG.Canvas('#lcd');                    // a surface the host registered
const off = new TVG.Canvas(new Uint32Array(64 * 64), {   // or your own buffer
    width: 64, height: 64,
    colorSpace: TVG.ColorSpace.ABGR8888,                   // buffers default to ABGR8888
    engineOption: TVG.EngineOption.SmartRender,
});
```

Options: `width`, `height`, `engineOption`, `colorSpace`. On a surface, size and colour space come
from the host; you can only make it smaller. A buffer that's too small for `width * height`
throws a `RangeError`.

## Engine options

`TVG.EngineOption`, can be OR'd:

- `Default`: smart rendering on, anti-aliasing on. What you get if you pass nothing, unless the
  surface is `aliased`.
- `SmartRender`: only redraw what changed. Keep it on, it's the difference between ~10 ms and
  ~250 ms a frame on a busy 800x480 scene.
- `Aliased`: no anti-aliasing. Hard pixel edges. You want this on palette displays, see
  [e-paper.md](e-paper.md).
- `None`: nothing. Every `render()` redraws everything.

## render() returns what changed

```js
const regions = canvas.render();
// [{ x, y, width, height }, ...]
```

- Empty array: nothing changed, the buffer is untouched.
- First render, no SmartRender, or more than 16 regions: one region covering the whole canvas.
- Otherwise ThorVG's actual dirty rectangles. They can be split oddly (it tiles the screen), so
  don't expect one rectangle per changed shape.

`render()` does **not** return `this` (webcanvas does), so it doesn't chain. The same regions also
go to the surface's `present()` callback in C, if there is one.

It clears the whole target only on the first draw. After that, ThorVG clears and redraws just the
dirty areas. Don't fake a full clear every frame like webcanvas examples do: it forces a full
redraw.

## The rest

| Call | Does |
|---|---|
| `add(paint, before?)` | Puts `paint` on the canvas, last (on top) or in front of `before`. `before` must already be on this canvas, `RangeError` otherwise. |
| `remove(paint?)` | Takes one paint off, or all of them with no argument. |
| `clear()` | Removes everything and pushes a blank frame (full wipe). |
| `update()` | Updates paints without drawing. You rarely need it, `render()` does it. |
| `resize(w, h)` | New size. On a surface, can't go past the surface size. |
| `viewport(x, y, w, h)` | Only draw inside this rectangle. |
| `destroy()` | Frees the canvas now (and the surface). Using it afterwards throws. |
| `renderer` | Always `'sw'`. |
| `dpr` | Always `1`. |

A paint can only have one parent. Adding something that's already in a scene or on a canvas fails
with a `ThorVGError` (`InsufficientCondition`). Remove it first.
