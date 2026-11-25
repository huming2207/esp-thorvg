// Pokes every class of the thorvg module, then draws a test scene on "#lcd".
// Same calls as @thorvg/webcanvas, just no wasm and no <canvas>.
import { init, Shape, Canvas, ThorVGError } from 'thorvg';
import * as os from 'qjs:os';
import * as std from 'qjs:std';

const sleep = (ms) => new Promise((r) => os.setTimeout(r, ms));
let failed = 0;

function check(name, ok, extra = '') {
    if (!ok) failed++;
    console.log(`${ok ? 'ok  ' : 'FAIL'} ${name} ${extra}`);
}

function throws(name, fn, type) {
    try {
        fn();
        check(name, false, '(no throw)');
    } catch (e) {
        check(name, e instanceof type, `(${e.name}: ${e.message})`);
    }
}

function time(name, fn) {
    const t = performance.now();
    const r = fn();
    console.log(`time ${name}: ${(performance.now() - t).toFixed(1)} ms`);
    return r;
}

const TVG = await init({ renderer: 'gl' }); // renderer is ignored, it's always sw
check('init returns the namespace', TVG.Shape === Shape && TVG.version.length > 0, TVG.version);

// offscreen: render into a JS buffer and look at pixels
{
    const px = new Uint32Array(16 * 16);
    const c = new TVG.Canvas(px, { width: 16, height: 16 }); // ABGR8888 by default
    c.add(new TVG.Shape().appendRect(0, 0, 8, 16).fill(255, 0, 0));
    c.render();
    check('offscreen red pixel', px[0] === 0xff0000ff, px[0].toString(16));
    check('offscreen untouched pixel', px[15] === 0);

    const g = new TVG.LinearGradient(0, 0, 16, 0).addStop(0, [0, 0, 255, 255]).addStop(1, [0, 0, 255, 255]);
    const a = new TVG.Shape().appendRect(8, 0, 8, 8).fill(g);
    const b = new TVG.Shape().appendRect(8, 8, 8, 8).fill(g); // same gradient twice is fine here
    c.add(a).add(b).render();
    check('gradient reused', px[8] === 0xffff0000 && px[15 * 16 + 15] === 0xffff0000, px[8].toString(16));

    const fx = new Uint32Array(64 * 64);
    const fc = new TVG.Canvas(fx, { width: 64, height: 64, engineOption: TVG.EngineOption.SmartRender });
    const sc = new TVG.Scene().add(new TVG.Shape().appendRect(8, 8, 32, 32).fill(255, 255, 255));
    sc.dropShadow(0, 0, 0, 255, 45, 8, 2);
    fc.add(sc).render();
    // 45 degrees is up-right in ThorVG (y up), so the shadow pokes out top-right
    check('dropShadow on ABGR8888', fx[4 * 64 + 44] !== 0 && fx[60 * 64 + 4] === 0, fx[4 * 64 + 44].toString(16));
    fc.destroy();

    throws('buffer too small', () => new TVG.Canvas(new Uint8Array(10), { width: 16, height: 16 }), RangeError);
    c.destroy();
    throws('destroyed canvas', () => c.render(), TypeError);
}

// ownership and errors
{
    const s = new TVG.Shape().appendCircle(10, 10, 5);
    s.dispose();
    check('dispose', s.isDisposed);
    throws('use after dispose', () => s.translate(1, 1), TypeError);

    const a = new TVG.Scene(), b = new TVG.Scene();
    a.add(b);
    throws('scene cycle', () => b.add(a), RangeError);

    // keep the parent alive: a dropped Scene wrapper frees the scene and detaches p
    const p = new TVG.Shape();
    const holder = new TVG.Scene().add(p);
    throws('paint with two parents', () => new TVG.Scene().add(p), ThorVGError);
    try {
        new TVG.Scene().add(p);
    } catch (e) {
        check('ThorVGError fields', e.code === TVG.ThorVGResultCode.InsufficientCondition && e.operation === 'add',
              e.message);
    }

    let seen = null;
    await init({ onError: (e, info) => { seen = info.operation; } });
    new TVG.Scene().add(p); // handled, no throw
    check('onError handler', seen === 'add');
    await init({});
    holder.clear();
    new TVG.Scene().add(p);
    check('detached after clear', true);

    class Star extends TVG.Shape {}
    const st = new Star();
    check('subclassing', st instanceof Star && st instanceof TVG.Paint && typeof st.moveTo === 'function');
}

// shapes, paths, bounds
{
    const s = new TVG.Shape();
    const P = TVG.PathCommand;
    s.appendPath([P.MoveTo, P.LineTo, P.CubicTo, P.Close], [[0, 0], [10, 0], [10, 5], [5, 10], [0, 10]]);
    const path = s.path();
    check('path round trip', path.commands.length === 4 && path.points.length === 5, JSON.stringify(path.commands));
    throws('path too few points', () => s.appendPath([P.LineTo], []), RangeError);
    const bb = s.bounds();
    check('bounds', Math.round(bb.width) === 10 && Math.round(bb.height) === 10, JSON.stringify(bb));
    check('oriented bounds', s.bounds({ oriented: true }).length === 4);
    s.stroke({ width: 2, color: [1, 2, 3], dash: [4, 2], cap: TVG.StrokeCap.Round });
    s.stroke({ dashOffset: 1 });
    check('opacity', s.opacity(128).opacity() === 128);
    check('visible', s.visible(false).visible() === false);
    s.id = 'thing';
    check('id from name', s.id === TVG.Accessor.id('thing'));
    const d = s.duplicate();
    check('duplicate', d instanceof TVG.Shape && d !== s && d.path().points.length === 5);
}

// text
{
    TVG.Font.load('PublicSans', asset('font'), { type: 'ttf' });
    const t = new TVG.Text().font('PublicSans').fontSize(40).text('Hello ThorVG');
    const m = t.metrics();
    check('text metrics', m.ascent > 0 && m.advance > 0, JSON.stringify(m));
    const gm = t.metrics('H');
    check('glyph metrics', gm.advance > 0, `advance ${gm.advance.toFixed(1)}`);
    t.layout(100, 0).wrap(TVG.TextWrapMode.Word);
    check('wrap lines', t.lines() > 1, `${t.lines()} lines`);
    throws('Font.load without bytes', () => TVG.Font.load('nope'), TypeError);
}

// pictures and accessor
const svg = asset('svg');
{
    const pic = new TVG.Picture();
    time('svg load', () => pic.load(svg, { type: 'svg' }));
    const { width, height } = pic.size();
    check('svg size', width > 0 && height > 0, `${width}x${height}`);
    let n = 0;
    new TVG.Accessor().set(pic, (p) => { n++; return true; });
    check('accessor walk', n > 10, `${n} paints`);
    pic.dispose();

    const raw = new Uint32Array(4 * 4).fill(0xff00ff00);
    const r = new TVG.Picture().load(raw, { type: 'raw', width: 4, height: 4 });
    check('raw picture', Math.round(r.size().width) === 4);
}

// SVG template: change paints inside a picture by name. "Ab" and "BA" hash to the same id
{
    const tpl = '<svg viewBox="0 0 64 64"><rect id="Ab" width="32" height="64" fill="#f00"/>' +
                '<rect id="BA" x="32" width="32" height="64" fill="#f00"/></svg>';
    const px = new Uint32Array(64 * 64);
    const c = new TVG.Canvas(px, { width: 64, height: 64, engineOption: TVG.EngineOption.SmartRender });
    const pic = new TVG.Picture().load(tpl, { type: 'svg' });
    check('picture accessible by default', pic.accessible === true);
    c.add(pic).render();

    const ab = pic.paint('Ab');
    const ba = pic.paint('BA');
    check('paint(name): colliding names stay apart', ab && ba && ab.id === ba.id && ab.bounds().x !== ba.bounds().x,
          `${ab?.bounds().x} ${ba?.bounds().x}`);
    check('paint(name): unknown name', pic.paint('nope') === null);

    ab.fill(0, 0, 255);
    const regions = c.render();
    check('edit inside a picture: redrawn', px[32 * 64 + 8] === 0xffff0000 && px[32 * 64 + 40] === 0xff0000ff,
          `${px[32 * 64 + 8].toString(16)} ${px[32 * 64 + 40].toString(16)}`);
    check('edit inside a picture: its region only', regions.length > 0 && regions.every((r) => r.x < 32),
          JSON.stringify(regions));

    const hashed = new TVG.Picture();
    hashed.accessible = false;
    hashed.load(tpl, { type: 'svg' });
    check('not accessible: paint(name) only has the hash', hashed.paint('Ab').bounds().x === hashed.paint('BA').bounds().x);
    c.destroy();
}

// lottie
{
    const seg = new TVG.LottieAnimation().load(asset('segment'));
    check('markers', seg.markersCnt() === 3 && seg.marker(1).name === 'sectionB', JSON.stringify(seg.marker(1)));
    seg.segment('sectionB');
    seg.dispose();

    const sl = new TVG.LottieAnimation().load(asset('slot'));
    const id = sl.gen({ 'lottie-icon-outline': { p: { a: 0, k: [1, 0, 0] } } });
    check('slot gen', id > 0);
    sl.apply(id);
    sl.dispose();
}

// things the second review caught
{
    const s = new TVG.Shape().appendRect(0, 0, 10, 10);
    throws('clip by a non-Shape', () => s.clip(new TVG.Scene()), TypeError);
    throws('clip by itself', () => s.clip(s), RangeError);
    s.clip(new TVG.Shape().appendCircle(5, 5, 5));
    s.clip(null);   // clears it
    const a = new TVG.Shape(), b = new TVG.Shape();
    a.mask(b, TVG.MaskMethod.Alpha);
    throws('mask cycle', () => b.mask(a, TVG.MaskMethod.Alpha), RangeError);
    throws('mask(null) needs None', () => a.mask(null, TVG.MaskMethod.Alpha), TypeError);
    a.mask(null, TVG.MaskMethod.None);
    const sc = new TVG.Scene(), x = new TVG.Shape();
    x.mask(sc, TVG.MaskMethod.Alpha);
    throws('scene holding its own mask', () => sc.add(x), RangeError);

    throws('huge canvas on a small buffer', () => new TVG.Canvas(new Uint32Array(16), { width: 32768, height: 32768 }), RangeError);
    throws('huge raw picture', () => new TVG.Picture().load(new Uint32Array(4), { type: 'raw', width: 32768, height: 32768 }), RangeError);
    throws('empty canvas buffer', () => new TVG.Canvas(new Uint32Array(0), { width: 1, height: 1 }), RangeError);
    throws('empty picture data', () => new TVG.Picture().load(new Uint8Array(0)), ThorVGError);
    const big = new Uint32Array(64 * 64);
    throws('canvas bigger than its view', () => new TVG.Canvas(new Uint32Array(big.buffer, 0, 16), { width: 64, height: 64 }), RangeError);

    // negative quality used to overrun ThorVG's blur kernel array
    const px = new Uint32Array(32 * 32);
    const c = new TVG.Canvas(px, { width: 32, height: 32 });
    const fx = new TVG.Scene().add(new TVG.Shape().appendRect(4, 4, 8, 8).fill(255, 255, 255));
    fx.gaussianBlur(2, 0, 0, -1).dropShadow(0, 0, 0, 255, 45, 2, 2, -1);
    c.add(fx).render();
    check('effects with quality -1 render', true);

    throws('resize past the buffer', () => c.resize(64, 64), RangeError);
    c.render();
    check('resize failure keeps the old size', true);

    c.remove();
    c.add(new TVG.Shape().appendRect(0, 0, 32, 32).fill(255, 0, 0));  // no JS ref kept
    c.render();
    c.clear();
    check('clear() wipes paints JS already let go of', px[0] === 0, px[0].toString(16));

    let n = 0;
    const acc = new TVG.Accessor();
    acc.set(new TVG.Picture().load(svg), () => { if (++n === 3) acc.dispose(); return true; });
    check('dispose() inside the accessor walk', acc.isDisposed && n > 3, `${n} paints`);

    const seg = new TVG.LottieAnimation().load(asset('segment'));
    seg.segment('sectionB');
    check('segment refreshes info', Math.round(seg.info().totalFrames) === 11, JSON.stringify(seg.info()));
    throws('frame(NaN)', () => seg.frame(NaN), RangeError);

    let caught = null;
    await init({ onError: (e) => { caught = e; } });
    const before = JSON.stringify(seg.info());
    check('handled error stops the operation', seg.load('not lottie') === seg && JSON.stringify(seg.info()) === before && caught);
    await init({});

    // onFrame that renders (used to recurse) and one that destroys the canvas
    const fc = new TVG.Canvas(new Uint32Array(16 * 16), { width: 16, height: 16 });
    const sa = new TVG.LottieAnimation().load(asset('lottie'));
    let calls = 0;
    sa.play(() => { calls++; fc.render(); });
    fc.render();
    check('render() from onFrame', calls === 1, `${calls} calls`);
    sa.play();  // already playing, keeps the callback
    sa.stop();
    sa.play(() => fc.destroy());
    throws('canvas destroyed in onFrame', () => fc.render(), TypeError);
    sa.stop();
    seg.dispose();
    sa.dispose();
}

// render() returns the dirty regions
{
    const px = new Uint32Array(64 * 64);
    const c = new TVG.Canvas(px, { width: 64, height: 64, engineOption: TVG.EngineOption.SmartRender });
    const box = new TVG.Shape().appendRect(0, 0, 8, 8).fill(255, 0, 0).translate(40, 40);
    c.add(new TVG.Shape().appendRect(0, 0, 4, 4).fill(0, 255, 0)).add(box);
    const first = c.render();
    check('first render: whole canvas', first.length === 1 && first[0].x === 0 && first[0].width === 64 && first[0].height === 64, JSON.stringify(first));
    const idle = c.render();
    check('nothing changed: no regions', Array.isArray(idle) && idle.length === 0, JSON.stringify(idle));
    box.fill(0, 0, 255);
    const changed = c.render();
    const hit = changed.some((r) => r.x <= 40 && r.y <= 40 && r.x + r.width >= 48 && r.y + r.height >= 48);
    const small = changed.every((r) => r.width * r.height < 64 * 64);
    check('one shape changed: its region only', changed.length > 0 && hit && small, JSON.stringify(changed));
    const plain = new TVG.Canvas(new Uint32Array(16 * 16), { width: 16, height: 16, engineOption: TVG.EngineOption.None });
    plain.render();
    const again = plain.render();
    check('no dirty tracking: always whole canvas', again.length === 1 && again[0].width === 16, JSON.stringify(again));
    c.destroy();
    plain.destroy();
}

// add(paint, before)
{
    const px = new Uint32Array(4 * 4);
    const c = new TVG.Canvas(px, { width: 4, height: 4 });
    const red = new TVG.Shape().appendRect(0, 0, 4, 4).fill(255, 0, 0);
    const blue = new TVG.Shape().appendRect(0, 0, 4, 4).fill(0, 0, 255);
    const s = new TVG.Scene().add(red);
    s.add(blue, red);                   // blue goes under red
    c.add(s);
    c.render();
    check('scene add before: order', px[0] === 0xff0000ff, px[0].toString(16));
    const green = new TVG.Shape().appendRect(0, 0, 4, 4).fill(0, 255, 0);
    c.add(green, s);                    // green under the whole scene
    c.render();
    check('canvas add before: order', px[0] === 0xff0000ff, px[0].toString(16));
    const stray = new TVG.Shape();
    const loose = new TVG.Shape();
    throws('scene add before a non-child', () => s.add(stray, loose), RangeError);
    throws('canvas add before a non-child', () => c.add(stray, loose), RangeError);
    s.add(stray);                       // refused add left it free, no leaked parent
    check('paint still addable after a refused add', true);
    throws('scene add before: before not a paint', () => s.add(loose, {}), TypeError);
    c.destroy();
}

// the visible scene on #lcd
const lcd = new Canvas('#lcd', { engineOption: TVG.EngineOption.SmartRender });
check('surface canvas', lcd.renderer === 'sw');
throws('unknown surface', () => new Canvas('#nope'), ReferenceError);

const bg = new TVG.Shape().appendRect(0, 0, 800, 480);
bg.fill(new TVG.RadialGradient(400, 240, 500).setStops([0, [40, 44, 52, 255]], [1, [10, 10, 14, 255]]));
lcd.add(bg);

const tiger = new TVG.Picture().load(svg);
const sz = tiger.size();
tiger.size(sz.width * 440 / sz.height, 440).translate(20, 20);
lcd.add(tiger);

const card = new TVG.Scene();
card.add(new TVG.Shape().appendRect(0, 0, 310, 120, { rx: 16, ry: 16 }).fill(250, 250, 250));
const title = new TVG.Text().font('PublicSans').fontSize(36).text('esp-thorvg').fill(30, 30, 30).translate(20, 20);
const sub = new TVG.Text().font('PublicSans').fontSize(18).text('QuickJS + ThorVG on ESP32').fill(90, 90, 90).translate(20, 70);
card.add(title).add(sub);
// no card.dropShadow() here: its square scratch buffers cost max(w, h)^2 * 4 bytes each, and
// ThorVG doesn't check them. Checked on a small 32-bit target above.
card.translate(460, 40);
lcd.add(card);

const anim = new TVG.LottieAnimation();
anim.load(asset('lottie'));
const info = anim.info();
check('lottie info', info.totalFrames > 0 && info.fps > 0, JSON.stringify(info));
const pic = anim.picture;
check('picture cached', pic === anim.picture);
pic.size(250, 200).translate(490, 220);
lcd.add(pic);

time('first render (800x480 RGB565)', () => lcd.render());

let frames = 0, lastFrame = -1;
anim.play((f) => { frames++; lastFrame = f; });
check('playing', anim.isPlaying());

// 30 frames as fast as the loop goes; tiger + card are static, SmartRender only redraws the lottie
const t0 = performance.now();
for (let i = 0; i < 30; i++) {
    await sleep(1);
    lcd.render();
}
const per = (performance.now() - t0) / 30;
console.log(`time lottie frame (smart render): ${per.toFixed(1)} ms avg, ${(1000 / per).toFixed(1)} fps`);
check('onFrame fired', frames === 30 && lastFrame > 0, `${frames} calls, frame ${lastFrame?.toFixed?.(1)}`);

anim.stop();
check('stopped', !anim.isPlaying() && anim.frame() === 0);

anim.frame(60);
time('full redraw', () => { bg.opacity(254); lcd.render(); });

console.log(failed ? `${failed} FAILED` : 'ALL PASS');
