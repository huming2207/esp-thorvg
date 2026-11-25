# esp-thorvg docs

ThorVG (vector graphics: shapes, SVG, Lottie, text) as an ESP-IDF component, plus an optional
QuickJS binding so you can drive it from JavaScript with roughly the same API as
[@thorvg/webcanvas](https://github.com/thorvg/thorvg.web).

Read in this order if you're new:

1. [building.md](building.md): getting it to compile, Kconfig options, PSRAM, SIMD
2. [js-host-setup.md](js-host-setup.md): the C side of the JS binding (surfaces, init, teardown)
3. [canvas.md](canvas.md): `Canvas`, `render()`, dirty regions
4. [paints.md](paints.md): `Shape`, `Scene`, `Picture`, transforms, clip/mask, `Accessor`
5. [text.md](text.md): fonts and `Text`
6. [gradients.md](gradients.md): `LinearGradient` and friends
7. [animation.md](animation.md): `Animation`, `LottieAnimation`
8. [errors-and-memory.md](errors-and-memory.md): errors, who owns what, GC, killed scripts
9. [e-paper.md](e-paper.md): using this on a palette panel without it looking awful, SVG templates
10. [webcanvas-differences.md](webcanvas-differences.md): what's not the same as the browser version

`examples/quickjs` pokes the entire JS API and runs in esp-emu. When the docs and that file
disagree, the example wins: it's what actually gets tested.
