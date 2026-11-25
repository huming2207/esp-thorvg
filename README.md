# ThorVG component

ESP-IDF component tracking the [ThorVG ESP fork](https://github.com/huming2207/thorvg/tree/jmh/main)
`jmh/main` (version 1.2.0), rebased on upstream `main` at `11bdd483`. On top of upstream: custom
allocator callbacks, class-scoped allocation, Grayscale8/RGB565 targets, PIE SIMD, editable SVG
pictures and exact name lookup (`Picture::find()`).
This is not the [official Espressif port](https://github.com/espressif/idf-extra-components/tree/master/thorvg).
Do not use for production just yet.

```sh
git submodule update --init --recursive
```

Optionally comes with a QuickJS binding (`CONFIG_THORVG_QUICKJS`) shaped after
[@thorvg/webcanvas](https://github.com/thorvg/thorvg.web):

```js
import { init } from 'thorvg';
const TVG = await init();
const canvas = new TVG.Canvas('#lcd');
canvas.add(new TVG.Shape().appendRect(0, 0, 100, 100, { rx: 10 }).fill(255, 0, 0));
const changed = canvas.render();    // [{ x, y, width, height }, ...]
```

Everything else is in [doc/](doc/README.md): building and Kconfig, the C host side, the JS API
topic by topic, memory and errors, e-paper tips, and what differs from webcanvas.

`examples/quickjs` runs the whole JS API in esp-emu and dumps the final frame
(`QJS_ESP_DIR=/path/to/esp-quickjs-ng idf.py build`).
