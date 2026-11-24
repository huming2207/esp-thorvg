# Differences from @thorvg/webcanvas

Same class names and mostly the same calls, but this runs on a microcontroller with no browser, no
wasm and no `<canvas>`. If you port code from the web version, this is what bites.

## Different

- **`render()` returns the dirty regions**, not the canvas. It doesn't chain. See
  [canvas.md](canvas.md#render-returns-what-changed).
- **`render()` doesn't clear every frame.** Only the first time. With SmartRender ThorVG redraws
  just what changed. Clearing every frame like the web version forces a full redraw, ~10x slower.
- **No `requestAnimationFrame`.** Animations step on each `render()`. See
  [animation.md](animation.md).
- **`Canvas` takes a surface name or a buffer**, not a DOM selector. Buffers default to ABGR8888.
- **`init()` options:** `renderer`, `threads` and `locateFile` are ignored, it's always the CPU
  renderer. `term()` does nothing, the host shuts the engine down.
- **`onError`**: a handled failure stops the method right there and returns `this`.
- **`clip()` only takes a `Shape`.** ThorVG would crash on anything else. `clip(null)` removes it.
- **`mask(null, MaskMethod.None)`** removes a mask.
- **Cycles throw `RangeError`**: a scene inside itself, a paint as its own clip/mask, and so on.
- **Gradients are templates**: `fill(gradient)` copies it.
- **Dropping the last JS reference to a `Scene` frees it** and detaches its children.
- **`Font.load()` needs the bytes.** There's no font provider and no `FontsourceProvider`.
- **One JS context** can use the module at a time.

## Extra

- `Scene.add(paint, before)` and `Canvas.add(paint, before)`: insert in front of a child.
- `Layout`: flexbox via Yoga. See [layout.md](layout.md).
- `ColorSpace.RGB565`, `XRGB8888`, `XBGR8888`
- `EngineOption.Aliased`
- `Picture.loadFile(path)`, `Font.loadFile(path)`: from the filesystem, skipping the JS heap.

## Missing

- `Video`
- Lottie audio: `volume()`, `resolver()`
- `Picture.resolver()` (ThorVG may call it from a loader thread, which QuickJS can't handle)
- `FontsourceProvider`
