const names = ['聚气', '发波', '吸收', '防御'];
const ids = ['charge', 'wave', 'absorb', 'guard'];
const W = 160;
const el = id => document.getElementById(id);
const fresh = () => Object.assign(document.createElement('canvas'), {width: W, height: W});
const tiles = ids.map((id, i) => {
  const card = document.createElement('section');
  card.className = 'card';
  card.innerHTML = `<h2>${names[i]}</h2><div class="views"><div><label>旧贴图</label><canvas width="160" height="160"></canvas></div><div><label>矢量重绘 · 未验收</label><canvas width="160" height="160"></canvas></div></div><div class="metrics"></div>`;
  el('grid').append(card);
  return {old: card.querySelectorAll('canvas')[0], next: card.querySelectorAll('canvas')[1], metrics: card.querySelector('.metrics')};
});
const image = src => new Promise((resolve, reject) => {
  const i = new Image();
  i.onload = () => resolve(i);
  i.onerror = () => reject(Error('Failed to load ' + src));
  i.src = src;
});
async function atlas(url, expectedCount) {
  const response = await fetch(url);
  if (!response.ok) throw Error(`Reference HTTP ${response.status}`);
  const buffer = await response.arrayBuffer(), v = new DataView(buffer);
  if (buffer.byteLength < 12 || String.fromCharCode(...new Uint8Array(buffer, 0, 4)) !== 'H2RS' ||
      v.getUint16(4, true) !== W || v.getUint16(6, true) !== W || v.getUint16(8, true) !== expectedCount)
    throw Error('Invalid reference atlas');
  const frames = [];
  for (let i = 0; i < expectedCount; i++) {
    const offset = v.getUint32(12 + i * 8, true), length = v.getUint32(16 + i * 8, true);
    if (offset < 12 + expectedCount * 8 || offset + length > buffer.byteLength) throw Error('Invalid frame extent');
    const bytes = new Uint8ClampedArray(await new Response(new Blob([buffer.slice(offset, offset + length)])
      .stream().pipeThrough(new DecompressionStream('deflate'))).arrayBuffer());
    if (bytes.length !== W * W * 4) throw Error('Bad frame length');
    frames.push(new ImageData(bytes, W, W));
  }
  return frames;
}
const focuses = Array.from({length: 33}, (_, i) => i / 32);
focuses.splice(17, 0, .500001);
let styles, colors, baseOld, baseNext;
const characterFrames = new Map();
let ready = false, running = false, tick = 0;

function variant(base, dir) {
  const c = fresh(), g = c.getContext('2d');
  g.drawImage(base, 0, 0);
  if (dir) {
    g.globalCompositeOperation = 'destination-in';
    const f = g.createLinearGradient(0, 0, W, 0);
    f.addColorStop(0, dir === 1 ? '#fff0' : '#fff');
    f.addColorStop(dir === 1 ? .42 : .58, 'rgba(255,255,255,.32)');
    f.addColorStop(1, dir === 1 ? '#fff' : '#fff0');
    g.fillStyle = f;
    g.fillRect(0, 0, W, W);
  }
  return c;
}
function bake(i, step, dir, bases = baseNext) {
  const c = fresh(), g = c.getContext('2d'), f = focuses[step], size = 52 + f * 36;
  g.globalAlpha = .34 + f * .66;
  if (f > .5) {
    g.shadowColor = `rgba(255,255,255,${.38 + f * .46})`;
    g.shadowBlur = 9 + f * 8;
    g.filter = `brightness(${1.05 + f * .32})`;
  }
  g.drawImage(variant(bases[i], dir), 80 - size / 2, 80 - size / 2, size, size);
  return g.getImageData(0, 0, W, W);
}
function tint(raw, i, sheen = -1, locked = false) {
  const a = raw.data, b = new Uint8ClampedArray(a);
  const pal = [[.2, .82, 1], [1, .53, .13], [.73, .3, 1], [.2, 1, .58]][i];
  for (let p = 0; p < a.length; p += 4) {
    const hi = (Math.max(a[p], a[p + 1], a[p + 2]) / 255) ** 5 * .4;
    const x = p / 4 % W, y = Math.floor(p / 4 / W);
    const stripe = sheen < 0 ? 0 : Math.max(0, 1 - Math.abs((x + y * .3) / W - (-.15 + sheen * 1.6)) / .09);
    for (let k = 0; k < 3; k++) {
      const colored = a[p + k] * (pal[k] + (1 - pal[k]) * hi);
      b[p + k] = Math.min(255, Math.round(colored * (locked ? 1.15 : 1) + (255 - colored) * stripe * .85));
    }
  }
  return new ImageData(b, W, W);
}
function compare(a, b) {
  if (a.length !== b.length) throw Error('Mismatched comparison dimensions');
  let sum = 0, n = 0, different = 0, intersection = 0, union = 0;
  for (let p = 0; p < a.length; p += 4) {
    const am = a[p + 3] > 16, bm = b[p + 3] > 16;
    if (am || bm) union++;
    if (am && bm) intersection++;
    let changed = false;
    for (let c = 0; c < 3; c++) {
      const av = Math.floor((a[p + c] * a[p + 3] + 127) / 255);
      const bv = Math.floor((b[p + c] * b[p + 3] + 127) / 255);
      if (am || bm) {sum += Math.abs(av - bv); n++;}
      if (av !== bv) changed = true;
    }
    if (changed) different++;
  }
  return {active_premultiplied_mae: n ? sum / n : 0, alpha_coverage_iou: union ? intersection / union : 1, differing_pixels: different};
}
function frame(i, mode, step, dir) {
  if (mode === 'source') return [baseOld[i].getContext('2d').getImageData(0, 0, W, W), baseNext[i].getContext('2d').getImageData(0, 0, W, W)];
  if (mode === 'sheen') {
    const f = Math.round(step * 20 / 33);
    return [colors[408 + i * 21 + f], tint(bake(i, 33, 0), i, f === 20 ? -1 : f / 19, true)];
  }
  const index = i * 102 + dir * 34 + step;
  return [mode === 'tint' ? colors[index] : styles[index], mode === 'tint' ? tint(bake(i, step, dir), i) : bake(i, step, dir)];
}
function displayComparison(canvas, a, b) {
  const mode = el('view').value;
  if (mode === 'pair') {canvas.getContext('2d').putImageData(b, 0, 0); return;}
  const output = new Uint8ClampedArray(a.data.length);
  for (let p = 0; p < output.length; p += 4) {
    output[p + 3] = 255;
    for (let k = 0; k < 3; k++) {
      const av = a.data[p + k] * a.data[p + 3] / 255, bv = b.data[p + k] * b.data[p + 3] / 255;
      output[p + k] = mode === 'overlay' ? (av + bv) / 2 : Math.abs(av - bv) * 3;
    }
  }
  canvas.getContext('2d').putImageData(new ImageData(output, a.width, a.height), 0, 0);
}
function describe(m) {
  return `有效区域平均误差 ${m.active_premultiplied_mae.toFixed(2)}/255 · 透明覆盖交并比 ${(m.alpha_coverage_iou * 100).toFixed(1)}% · 差异像素 ${m.differing_pixels}`;
}
function drawCharacter(height = +el('character-size').value) {
  if (!ready) return;
  const frames = characterFrames.get(height);
  el('character-old').getContext('2d').putImageData(frames[0], 0, 0);
  displayComparison(el('character-new'), frames[0], frames[1]);
  el('character-metrics').textContent = describe(compare(frames[0].data, frames[1].data));
  return {height, ...compare(frames[0].data, frames[1].data)};
}
function draw() {
  if (!ready) return;
  const mode = el('mode').value, step = +el('focus').value, dir = +el('direction').value;
  tiles.forEach((v, i) => {
    const [a, b] = frame(i, mode, step, dir);
    v.old.getContext('2d').putImageData(a, 0, 0);
    displayComparison(v.next, a, b);
    v.metrics.textContent = describe(compare(a.data, b.data));
  });
  drawCharacter();
}
el('focus').oninput = draw;
for (const id of ['direction', 'mode', 'view']) el(id).onchange = draw;
el('character-size').onchange = () => drawCharacter();
el('zoom').onchange = () => el('grid').classList.toggle('zoom', el('zoom').value === 'fit');
el('refresh').onclick = () => location.reload();
el('play').onclick = () => {running = !running; el('play').textContent = running ? '暂停样式序列' : '播放样式序列';};
function loop(t) {
  if (running && ready && t - tick >= 33) {
    tick = t; el('focus').value = (+el('focus').value + 1) % 34; draw();
  }
  requestAnimationFrame(loop);
}
requestAnimationFrame(loop);
function average(rows) {return rows.reduce((s, v) => s + v.active_premultiplied_mae, 0) / rows.length;}
el('verify').onclick = async () => {
  if (!ready) return;
  running = false; el('play').textContent = '播放样式序列'; el('verify').disabled = true;
  try {
    const results = [], replay = [];
    for (let i = 0; i < 4; i++) {
      for (const mode of ['style', 'tint']) for (let dir = 0; dir < 3; dir++) for (let step = 0; step < 34; step++) {
        const [a, b] = frame(i, mode, step, dir);
        results.push({skill: ids[i], mode, dir, step, focus: focuses[step], ...compare(a.data, b.data)});
        if (mode === 'style') replay.push({skill: ids[i], dir, step, ...compare(a.data, bake(i, step, dir, baseOld).data)});
      }
      for (let f = 0; f < 21; f++) {
        const a = colors[408 + i * 21 + f], b = tint(bake(i, 33, 0), i, f === 20 ? -1 : f / 19, true);
        results.push({skill: ids[i], mode: 'sheen', step: f, ...compare(a.data, b.data)});
      }
      el('result').textContent = `已检查 ${results.length} 张样式帧…`;
      await new Promise(requestAnimationFrame);
    }
    const report = {status: 'draft-not-accepted', scope: '900 resource styles, not full game temporal frames',
      created_at: new Date().toISOString(), browser: navigator.userAgent,
      renderer: 'Browser SVG + Canvas; NOT native ESP32',
      summary: ids.map(skill => ({skill, mean_active_mae: average(results.filter(r => r.skill === skill))})),
      reference_replay: {count: replay.length, mean_active_mae: average(replay),
        max_active_mae: Math.max(...replay.map(r => r.active_premultiplied_mae)), frames: replay},
      character: [81, 156, 321].map(height => drawCharacter(height)), frames: results};
    drawCharacter();
    const response = await fetch('/save-vector-report', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(report)});
    if (!response.ok) throw Error(`保存报告失败 HTTP ${response.status}`);
    const saved = await response.json();
    el('result').textContent = `未验收：900 张样式均已记录差异。\n旧资源重放平均误差：${report.reference_replay.mean_active_mae.toFixed(3)}/255\n` +
      report.summary.map(s => `${names[ids.indexOf(s.skill)]}平均误差：${s.mean_active_mae.toFixed(2)}/255`).join('\n') + `\n报告：${saved.saved}`;
  } catch (e) {el('result').textContent = String(e);}
  finally {el('verify').disabled = false;}
};
try {
  const [source, vectors, oldStyles, oldColors, oldCharacter, newCharacter] = await Promise.all([
    image('../assets/source/skill-icons-v1.png'), Promise.all(ids.map(x => image(`../assets/vector/skill-${x}.svg`))),
    atlas('../assets/generated/skill-styles.h2rs', 408), atlas('../assets/generated/skill-colors.h2rs', 492),
    image('../assets/source/opponent-full.png'),
    Promise.all([81, 156, 321].map(height => image(`../assets/vector/opponent-full.svg?review-height=${height}`)))]);
  styles = oldStyles; colors = oldColors;
  // Independent SVG image instances prevent a browser's adaptive SVG raster
  // cache from changing a small comparison after the enlarged view was drawn.
  // These are transient review render outputs, never exported runtime artwork.
  [81, 156, 321].forEach((height, index) => {
    const width = height * 1224 / 1285;
    characterFrames.set(height, [oldCharacter, newCharacter[index]].map(img => {
      const c = document.createElement('canvas'); c.width = 306; c.height = 322;
      const g = c.getContext('2d');
      g.drawImage(img, (306 - width) / 2, (322 - height) / 2, width, height);
      return g.getImageData(0, 0, 306, 322);
    }));
  });
  baseOld = ids.map((_, i) => {
    const c = fresh(), half = source.width / 2;
    c.getContext('2d').drawImage(source, i % 2 * half, Math.floor(i / 2) * half, half, half, 0, 0, W, W);
    return c;
  });
  baseNext = vectors.map(v => {const c = fresh(); c.getContext('2d').drawImage(v, 0, 0, W, W); return c;});
  ready = true; el('verify').disabled = false;
  el('status').textContent = '参考资源已加载 · 408 焦点样式 / 492 染色与确认帧 / 对手原图';
  draw();
} catch (e) {el('status').textContent = '读取失败：' + e.message; throw e;}
