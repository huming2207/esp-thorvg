# E-paper and other palette displays

Panels like Spectra E6 show 6 colours. Everything ThorVG draws gets dithered down to those, and
that goes wrong in predictable ways.

## Turn anti-aliasing off

Anti-aliased edges are blends of two colours, like pink between red and white. A dither like
Floyd-Steinberg can't show pink on a strip one pixel wide, so it fakes it with an on/off pattern.
That turns every glyph edge into a comb of teeth. Looks terrible on CJK text.

Fix: register the surface with `.aliased = true`. Its canvases then default to
`SmartRender | Aliased` and edges come out solid.

```c
tvg_js_surface_t s = { /* ... */ .aliased = true };
```

JS can still pass its own `engineOption` if it wants anti-aliasing back for some canvas.

## Use the panel's colours exactly

Fill text and lines with the panel's exact palette colours (for E6: black, white, red, yellow, blue,
green, as pure RGB). Anything in between gets dithered into dots. That's fine for photos and big
gradients, bad for text and thin lines.

## Dirty regions

`render()` returns what changed, and an empty array means nothing did. On a panel that takes 20 s
per refresh and flashes, that's how you avoid pointless refreshes:

```js
const regions = canvas.render();
if (regions.length) await refreshPanel();   // whatever your firmware gives you
```

When you do push a frame, dither the whole thing, not just the regions. Error diffusion spreads
across region borders, and dithering only parts leaves visible seams.

## Signage: draw it in an SVG, change it from JS

For a screen that's a fixed design with a few changing bits (announcements, dates, prices), don't
build the UI in code. Draw it in Inkscape or Figma, give the changing elements ids, load it as one
`Picture`, and swap texts by name. Only what changed gets redrawn. How, and what the SVG needs:
[paints.md](paints.md#editing-an-svg-templates).

## Skip scene effects

They work on RGB565, but a drop shadow on a 1024x600 panel wants 12 MB of PSRAM, and running out
reboots the chip (see [paints.md](paints.md#scene)). Even when they fit, a shadow or blur dithered
down to 6 colours just looks like noise.

## Animation

Doesn't work here in any useful way. See [animation.md](animation.md#on-e-paper).
