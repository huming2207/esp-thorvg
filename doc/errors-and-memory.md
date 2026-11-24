# Errors and memory

## When ThorVG says no

By default a failed ThorVG call throws a `ThorVGError`:

```js
try {
    scene.add(alreadyParented);
} catch (e) {
    e.code;         // TVG.ThorVGResultCode.InsufficientCondition
    e.operation;    // 'add'
}
```

Or install a handler, webcanvas style:

```js
await init({ onError: (err, info) => console.log(info.operation, err.message) });
```

With a handler the method doesn't throw. It reports and stops right there, returning `this` so
your chain doesn't explode, but **nothing after the failed call happens** (that call and the rest of
that method). `await init({})` removes the handler again.

Things the binding catches before ThorVG gets a chance to crash are plain JS errors:
`TypeError` for wrong types or disposed objects, `RangeError` for bad sizes, cycles and similar.
Those throw even with an `onError` handler.

## Who owns what

- Every JS wrapper holds one ThorVG reference on its object. Parents (scenes, canvases) hold their
  own. So a paint lives as long as JS or a parent needs it, whichever lasts longer.
- Dropping the last JS reference to a `Scene` frees it and **detaches its children**. If you still
  need the scene on screen, keep a reference to it (or to something up the chain that you keep).
- `dispose()` frees now instead of waiting for GC. Calling methods after that throws `TypeError`.
  `isDisposed` tells you.
- `Picture.load()`, `Font.load()` and raw image data are copied. Your buffers can go right away.
- A `Canvas` on a surface keeps that surface busy until the canvas is destroyed or GC'd.
- Playing animations pin themselves until paused or stopped.
- `Layout` boxes: parents hold their children, a box holds its paint. See [layout.md](layout.md).

Memory: the JS objects live in the QuickJS heap. ThorVG's own objects (paths, render data, decoded
images, fonts) go wherever the host told ThorVG to allocate (see the allocator section in
[building.md](building.md)). Yoga nodes use the normal heap.

## Leak check

From C, `tvg_js_live_objects()` counts the native objects JS still holds. After a clean context
teardown it should be 0. If it isn't, something kept a reference past the context, and that's a
bug worth reporting.

## Scripts that get killed

If the host runs scripts with time/memory limits and kills one, the runtime gets thrown away
without finalizers. The host must call `tvg_js_abandon()` then, or everything the script made
leaks and the next script can't import the module. See
[js-host-setup.md](js-host-setup.md#killed-scripts-tvg_js_abandon).

## Things that still crash

The binding guards everything it knows about, but a few things are ThorVG/Yoga's and can still
take the chip down:

- scene effects on an RGB565 target ([paints.md](paints.md#scene))
- running out of memory inside Yoga (`abort()`)
- ThorVG not checking the big scratch allocation of scene effects
