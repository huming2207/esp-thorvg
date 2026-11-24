# Animation

```js
const anim = new TVG.LottieAnimation().load(lottieJson);
const pic = anim.picture;            // the Picture to put on a canvas
pic.size(200, 200).translate(10, 10);
canvas.add(pic);

anim.play((frame) => console.log(frame));
setInterval(() => canvas.render(), 33);   // you drive the clock
```

## There's no requestAnimationFrame

In a browser, webcanvas steps animations from `requestAnimationFrame`. There's no such thing here.
Instead, every playing animation is advanced at the start of each `Canvas.render()`, by the real
time since the last step. So:

- **your render loop is the frame clock.** No `render()`, no animation.
- frames advance by elapsed time, so rendering faster gives smoother motion, not faster motion.
  Rendering two canvases doesn't double the speed either.
- `onFrame` fires on every `render()`, with the current frame number.
- calling `render()` from inside `onFrame` is fine, it doesn't recurse.

A playing animation keeps itself alive (like a rAF closure would) until you `pause()` or `stop()`
it, even if you drop your reference.

## Animation

| Call | Does |
|---|---|
| `load(data)` | Lottie JSON as a string or bytes. |
| `picture` | The `Picture` that shows it. Same object every time. |
| `info()` | `{ totalFrames, duration, fps }`, duration in seconds. |
| `frame(n)` / `frame()` | Jump to a frame / get the current one. `NaN` throws. |
| `segment(begin, end)` | Only play this frame range. |
| `play(onFrame?)` | Start. Calling it again while playing does nothing (keeps the old callback). |
| `pause()` | Stop where it is. |
| `stop()` | Stop and go back to frame 0. |
| `isPlaying()` | |
| `setLoop(bool)` / `getLoop()` | Loop or stop at the end. Loops by default. |
| `seek(seconds)` / `getCurrentTime()` | Time based instead of frames. |
| `dispose()` / `isDisposed` | |

## LottieAnimation extras

| Call | Does |
|---|---|
| `segment('markerName')` | Play a named marker instead of a frame range. |
| `markersCnt()` / `marker(i)` | List markers: `{ name, begin, end }`. |
| `gen(slots)` | Make a slot override (object or JSON string), returns an id. |
| `apply(id)` / `del(id)` | Apply / delete a slot override. |
| `quality(0..100)` | Lower = cheaper effects. |

No audio, so no `volume()`, and no `resolver()`.

## On e-paper

Don't. A full refresh takes seconds and flashes. If you want a Lottie on a slow panel, pick one
frame with `frame(n)` and render that.
