# Layout (Yoga flexbox)

ThorVG only knows absolute positions. `Layout` gives you flexbox, using
[Yoga](https://yogalayout.dev) (same engine as React Native). Needs `THORVG_JS_LAYOUT=y`.

## The idea

A `Layout` is a box. You nest boxes, set flexbox styles, call `calculate()` on the root, and every
box that has a paint bound to it gets that paint moved to the right spot.

```js
const { Layout, Scene, Text, Shape } = TVG;

const rootScene = new Scene();
const root = new Layout({ width: 800, height: 480, flexDirection: 'row', padding: 16, gap: 8 }, rootScene);

const title = new Text().font('Sans').fontSize(32).text('Weather').fill(0, 0, 0);
const titleBox = new Layout({ flexGrow: 1 }, title);

rootScene.add(title);       // paints nest the same way the boxes do
root.add(titleBox);

canvas.add(rootScene);
root.calculate();
canvas.render();
```

## Positions are relative to the parent box

Yoga gives each box's left/top relative to its parent box, and `calculate()` puts exactly that into
the bound paint's `translate()`. So the paint tree has to match the box tree: a box with children
should be bound to a `Scene`, and its children's paints go inside that scene. Mix the two up and
things land in the wrong place.

A box doesn't need a paint at all. Then it's just spacing.

## API

| Call | Does |
|---|---|
| `new Layout(style?, paint?)` | Make a box, optionally styled and bound. |
| `style({...})` | Merge styles in. `null`/`undefined` resets one to Yoga's default. |
| `paint` | Get/set the bound paint. `null` unbinds. |
| `add(child, before?)` | Add a child box, last or in front of `before`. |
| `remove(child?)` | Remove one, or all with no argument. |
| `childCount` | Number of children. |
| `calculate(width?, height?)` | Lay out the tree and move the paints. Root only. Without sizes it uses the root's own style. |
| `layout` | `{ left, top, width, height }` after `calculate()`, relative to the parent. |
| `isDirty` | Needs a new `calculate()`. |
| `markDirty()` | Force a re-measure. Only matters for text boxes, no-op otherwise. |
| `dispose()` / `isDisposed` | Free it now. Only on a box without a parent. |

## Styles

React Native names. Numbers are pixels, `'50%'` is a percentage, `'auto'` where Yoga allows it.

- Direction: `flexDirection` (`column` default, `row`, `column-reverse`, `row-reverse`), `direction`
  (`inherit`, `ltr`, `rtl`)
- Alignment: `justifyContent`, `alignItems`, `alignSelf`, `alignContent` with CSS values
  (`flex-start`, `center`, `flex-end`, `stretch`, `space-between`, `space-around`,
  `space-evenly`, `baseline`, `auto`)
- Flex: `flex`, `flexGrow`, `flexShrink`, `flexBasis`, `flexWrap` (`nowrap`, `wrap`, `wrap-reverse`)
- Size: `width`, `height`, `minWidth`, `minHeight`, `maxWidth`, `maxHeight`, `aspectRatio`
- Spacing: `margin*`, `padding*`, `border*` (`Top`, `Right`, `Bottom`, `Left`, `Start`, `End`,
  `Horizontal`, `Vertical` suffixes, or none for all sides), `gap`, `rowGap`, `columnGap`
- Positioning: `position` (`relative`, `absolute`, `static`), `top`, `right`, `bottom`, `left`,
  `start`, `end`, `inset`
- Other: `display` (`flex`, `none`, `contents`), `overflow`, `boxSizing`

Yoga's defaults, **not the web's**: `flexDirection: 'column'`, `flexShrink: 0`,
`alignContent: 'flex-start'`. If something doesn't shrink like you expect, that's why.

`border*` only reserves space, nothing gets drawn. Draw your own `Shape` if you want a visible
border.

`display: 'none'` also hides the bound paint.

## Text boxes measure themselves

Bind a `Text` to a box with no children and Yoga asks ThorVG how big the text is. Height is whole
lines (so `一` is as tall as `夏`), width is the widest line. With `wrap()` set, the text wraps to
whatever width the box gets. After layout the text's `layout()` box is set to the box size, so
`align(0.5, 0)` centres it in the box.

Changing the text's string, font, size, spacing, wrap or align marks the box dirty by itself. Just
call `calculate()` again.

## It won't dirty what didn't move

`calculate()` only touches paints whose position or size actually changed. With SmartRender, an
unchanged layout costs nothing on the next `render()`. Change one label, and only that label (plus
whatever moved because of it) gets redrawn.

## Errors instead of crashes

Without C++ exceptions, Yoga calls `abort()` when you break its rules, and that reboots the chip.
So `Layout` checks everything first and throws instead:

- adding a box that already has a parent, or adding a box inside itself (`RangeError`)
- adding children to a box bound to a `Text`, or binding a `Text` to a box that has children
- binding one paint to two boxes
- `calculate()` on a non-root, `remove()` of a non-child, `dispose()` of a box that has a parent
- unknown style names, bad enum values (`RangeError`), bad lengths like `'12px'` (`TypeError`),
  `NaN`, `'auto'` where Yoga has no auto

Out of memory inside Yoga still aborts. Keep trees sane.

## Ownership

A parent box holds a JS reference to each child, and a box holds one to its paint. Drop your last
reference to the root and the whole tree goes away by itself. Drop a parent and its children get
detached, not freed, as long as you still hold them.
