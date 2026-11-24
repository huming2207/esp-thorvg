# Text and fonts

## Fonts

There's no font provider, no system fonts, nothing downloaded. You give ThorVG the bytes.

```js
TVG.Font.load('Sans', fontBytes, { type: 'ttf' });   // or 'otf'. Copied, the buffer can go after
TVG.Font.loadFile('/spiffs/NotoSans.ttf');            // name = file name without extension
TVG.Font.unload('Sans');
```

From C it's often easier: `tvg_font_load_data("Sans", data, len, "ttf", false)` on an embedded
`.ttf` doesn't copy anything (ThorVG reads straight from flash), and JS can use it by name.

Fonts are global and outlive JS contexts.

CJK fonts are big (several MB). Load them with `loadFile` or from C, not through the JS heap.

## Text

```js
const t = new TVG.Text()
    .font('Sans')
    .fontSize(32)
    .text('Hello 你好')
    .fill(0, 0, 0)                  // fill(r, g, b) or fill(gradient)
    .translate(20, 20);
canvas.add(t);
```

| Call | Does |
|---|---|
| `font(name)` | Pick a loaded font. Throws if there's no font with that name. |
| `text(str)` | The string. `\n` breaks lines. |
| `fontSize(px)` | Size. |
| `fill(...)` | Colour or gradient. |
| `outline(width, r, g, b)` | Stroke around the glyphs. |
| `italic(shear = 0.18)` | Fake italics by shearing. |
| `spacing(letter, line)` | Multipliers, 1 = normal. Pass both: a missing one ends up 0. |
| `layout(w, h)` | A box for wrapping and alignment. 0 on an axis = no limit. |
| `align(x, y)` | 0..1 per axis. Inside the layout box if there is one, otherwise an anchor on the text itself. |
| `wrap(TVG.TextWrapMode.X)` | `None`, `Character`, `Word`, `Smart`, `Ellipsis`. Needs a layout width. |
| `lines()` | How many lines it ended up with. |
| `metrics()` | `{ ascent, descent, linegap, advance }` for the font at this size. |
| `metrics('A')` | One glyph: `{ advance, bearing, min, max }`. |

The text box starts at the paint's position (top-left, not the baseline). One line is
`advance - linegap` tall, n lines are `advance * n - linegap`.

If the text is bound to a [Layout](layout.md) node, don't call `layout()` on it yourself: the node
sets the box. `align()` and `wrap()` still work inside it.

On e-paper, read [e-paper.md](e-paper.md) before picking text colours.
