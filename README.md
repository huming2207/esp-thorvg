# ThorVG component

ESP-IDF component tracking the [ThorVG ESP fork](https://github.com/huming2207/thorvg/tree/jmh/esp)
`jmh/esp`, pinned to `5155c4e5e7a8ee23e738cd29fdb22640357ac794` (version 1.2.0).
Based on upstream `main` at `04eb68aa4c0e4f6d449bdf027983a664d93aadef`,
with custom allocator callbacks and class-scoped allocation.
This is not the [official Espressif port](https://github.com/espressif/idf-extra-components/tree/master/thorvg).
Do not use for production just yet.

```sh
git submodule update --init --recursive
```

Optionally comes with a QuickJS binding (`CONFIG_THORVG_QUICKJS`) shaped after
[@thorvg/webcanvas](https://github.com/thorvg/thorvg.web), plus flexbox layout through Yoga:

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
