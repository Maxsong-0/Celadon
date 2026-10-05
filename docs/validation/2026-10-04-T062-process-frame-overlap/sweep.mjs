// 全站运行时实测 SVG 文字重叠 / 越界 / 压图形（Playwright，只读量，不改站点）。
// 用法：node sweep.mjs [--base URL] [--concurrency 4] [--routes /,/zh] [--widths 375,1024] [--out result.json] [--reduced] [--half] [--engine chromium|firefox|webkit] [--dry-run]
//   --engine：用哪个浏览器引擎量（默认 chromium；firefox / webkit 要先 `playwright install`，T-069 起）。
//   --half：首页「四道工序」处理前 / 处理后两态各 50 % 不透明度同时可见（滚动停在半程的样子），两态里的字也互相查（T-069）。
// 环境变量：PLAYWRIGHT_DIR（playwright 包目录，默认本机 ~/.npm/_npx/…）、SWEEP_SITE（源码对位用的 site 目录，默认 ../base/site）。
//   --reduced：模拟系统「减少动效」（首页四帧改为竖排四张 SVG、图停在处理后）。
//   --dry-run：只打印要量的「路由 × 屏宽」清单，不开浏览器（预演）。
// 产物：<out>（汇总 + 去重命中）与 <out 去掉 .json>-raw.json（每页每个 SVG 的逐行量值）。
import { createRequire } from 'node:module';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const require = createRequire(import.meta.url);
const pw = require(process.env.PLAYWRIGHT_DIR || '/home/maxsong/.npm/_npx/e41f203b7505f1fb/node_modules/playwright');

const HERE = path.dirname(fileURLToPath(import.meta.url));
// 源码对位用构建基线站点的那份源码（main fbe91f3 的副本，与 dist 同源）；工作树里的文件可能正被别的会话改着，行号会漂。
const SITE = process.env.SWEEP_SITE ? path.resolve(process.env.SWEEP_SITE) : path.resolve(HERE, '../base/site');

/* ---------------- 常量与配置（集中一处） ---------------- */

const ROUTES = [
  '/', '/zh', '/download', '/zh/download', '/changelog', '/zh/changelog', '/docs', '/zh/docs', '/docs/install', '/zh/docs/install',
  '/docs/interface', '/zh/docs/interface', '/docs/glaze', '/zh/docs/glaze', '/docs/balance', '/zh/docs/balance', '/docs/clay', '/zh/docs/clay',
  '/docs/color', '/zh/docs/color', '/docs/limit', '/zh/docs/limit', '/docs/advanced', '/zh/docs/advanced', '/docs/listening', '/zh/docs/listening',
  '/docs/meters', '/zh/docs/meters', '/docs/controls', '/zh/docs/controls', '/docs/faq', '/zh/docs/faq', '/algorithms', '/zh/algorithms',
  '/algorithms/balance', '/zh/algorithms/balance', '/algorithms/clay', '/zh/algorithms/clay', '/algorithms/color', '/zh/algorithms/color',
  '/algorithms/limit', '/zh/algorithms/limit', '/algorithms/glaze', '/zh/algorithms/glaze', '/algorithms/verification', '/zh/algorithms/verification'
];
const WIDTHS = [320, 360, 375, 390, 414, 600, 768, 820, 1024, 1180, 1280, 1440, 1920];

const CFG = {
  base: 'http://127.0.0.1:8862',
  height: 900,
  concurrency: 4,
  gotoTimeoutMs: 45000,
  scrollStepRatio: 0.6, // 从上滚到底：每步 0.6 屏
  scrollStepWaitMs: 70,
  scrollEndWaitMs: 600,
  settleMs: 700, // 滚到某个 SVG 处后等多久再量（懒加载与入场动画）
  waitCjkMs: 15000, // 中文页等思源声明插入的上限
  restableMs: 300, // 第二次量与第一次的间隔（检查是否稳定）
  stableTol: 0.25, // 两次量值差超过这么多单位算不稳定，再等一会儿重量
  glyphAscent: 0.78, // 字形盒：基线上 0.78 em、下 0.22 em（避开 bbox 的行距空白）
  glyphDescent: 0.22,
  overlapMinArea: 0.5, // 两段字形盒相交面积 > 0.5 单位² = 重叠
  nearGap: 12, // 同一行水平间距 < 12 单位 = 贴近
  oobTol: 0.5, // 超出 viewBox 的容差
  shapeDepthMin: 1.5, // 图形压进字形盒的深度 ≥ 1.5 单位才算「明显遮挡」
  nearShapeGap: 6, // 图形离字形盒（四边取最大偏离，比欧氏距离略严）< 6 单位 = 贴近（T-069；不到 1.5 的压入也算在内）
  fitTight: 6, // 框内文字离框边 < 6 单位 = 太挤
  fullVbRatio: 0.85, // 面积 ≥ 85 % viewBox 的矩形当底图（网格），不算图形也不当容器
  stepAlong: 0.75, // 沿描边取样步长（单位）
  stepGrid: 1 // 填充区域网格取样步长（单位）
};

const FRAME_NAMES = ['BALANCE', 'CLAY', 'COLOR', 'LIMIT'];

/* ---------------- 命令行 ---------------- */

function parseArgs(argv) {
  const a = { dryRun: false };
  for (let i = 2; i < argv.length; i++) {
    const k = argv[i];
    const v = argv[i + 1];
    if (k === '--dry-run') a.dryRun = true;
    else if (k === '--base') (a.base = v), i++;
    else if (k === '--concurrency') (a.concurrency = Number(v)), i++;
    else if (k === '--routes') (a.routes = v.split(',')), i++;
    else if (k === '--widths') (a.widths = v.split(',').map(Number)), i++;
    else if (k === '--out') (a.out = v), i++;
    else if (k === '--reduced') a.reduced = true;
    else if (k === '--half') a.half = true;
    else if (k === '--engine') (a.engine = v), i++;
  }
  return a;
}

/* ---------------- 页面内执行的函数（自包含，不引用外部变量） ---------------- */

async function pageScrollPass({ stepRatio, stepWaitMs, endWaitMs }) {
  const sleep = ms => new Promise(r => setTimeout(r, ms));
  let y = 0;
  let steps = 0;
  while (steps < 800) {
    const max = Math.max(0, document.documentElement.scrollHeight - innerHeight);
    window.scrollTo({ top: Math.min(y, max), behavior: 'instant' });
    await sleep(stepWaitMs);
    steps++;
    if (y >= max) break;
    y += Math.max(200, Math.round(innerHeight * stepRatio));
  }
  await sleep(endWaitMs);
  return { steps, height: document.documentElement.scrollHeight };
}

function pageListSvgs() {
  const all = [...document.querySelectorAll('svg')].filter(s => !s.parentElement || !s.parentElement.closest('svg'));
  const secs = [...document.querySelectorAll('main section')];
  return all.map((s, i) => {
    s.setAttribute('data-sweep-id', String(i));
    const r = s.getBoundingClientRect();
    const li = s.closest('li');
    const sec = s.closest('section');
    return {
      id: i,
      nText: s.querySelectorAll('text').length,
      displayed: s.getClientRects().length > 0 && r.width > 0 && r.height > 0,
      viewBox: s.getAttribute('viewBox'),
      aria: s.getAttribute('aria-label'),
      inFigure: !!s.closest('figure'),
      inProcess: !!s.closest('#process'),
      sectionId: sec ? sec.id || null : null,
      sectionIndex: sec ? secs.indexOf(sec) : -1,
      liIndex: li && li.parentElement ? [...li.parentElement.children].indexOf(li) : -1,
      liTag: li ? (li.querySelector('.font-mono')?.textContent || '').trim() || null : null,
      cssW: Math.round(r.width * 10) / 10,
      cssH: Math.round(r.height * 10) / 10
    };
  });
}

async function pageScrollToSvg({ id, settleMs }) {
  const svg = document.querySelector(`svg[data-sweep-id="${id}"]`);
  if (!svg) return false;
  svg.scrollIntoView({ block: 'center', inline: 'nearest', behavior: 'instant' });
  await new Promise(r => setTimeout(r, settleMs));
  return true;
}

async function pageFontsReady({ id, zh, waitCjkMs }) {
  const sleep = ms => new Promise(r => setTimeout(r, ms));
  const hasCjkFace = () => [...document.fonts].some(f => /Noto Sans SC/.test(f.family));
  if (zh) {
    const t0 = performance.now();
    while (!hasCjkFace() && performance.now() - t0 < waitCjkMs) await sleep(100);
  }
  const svg = document.querySelector(`svg[data-sweep-id="${id}"]`);
  const els = svg ? [...svg.querySelectorAll('text, tspan')] : [];
  await Promise.all(
    els.map(t => {
      const cs = getComputedStyle(t);
      return document.fonts.load(`${cs.fontStyle} ${cs.fontWeight} ${cs.fontSize} ${cs.fontFamily}`, t.textContent || ' ').catch(() => null);
    })
  );
  await document.fonts.ready;
  await new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r)));
  return { cjkFace: hasCjkFace(), cjkLoaded: [...document.fonts].some(f => /Noto Sans SC/.test(f.family) && f.status === 'loaded'), status: document.fonts.status };
}

function pageMeasureSvg(p) {
  const { id, asc, desc, shapeDepthMin, stepAlong, stepGrid, fullVbRatio, halfState, nearShapeGap } = p;
  const svg = document.querySelector(`svg[data-sweep-id="${id}"]`);
  if (!svg) return { error: 'svg-missing' };
  if (!svg.getClientRects().length) return { error: 'display-none-at-measure' };
  const screen = svg.getScreenCTM();
  if (!screen) return { error: 'no-screen-ctm' };
  const rootInv = screen.inverse();
  const scale = Math.hypot(screen.a, screen.b); // 每个用户单位多少 CSS px
  const vbv = svg.viewBox && svg.viewBox.baseVal;
  const vb = vbv && vbv.width > 0 ? { x: vbv.x, y: vbv.y, w: vbv.width, h: vbv.height } : null;
  const cr = svg.getBoundingClientRect();
  const corner = [[cr.left, cr.top], [cr.right, cr.bottom]].map(([x, y]) => new DOMPoint(x, y).matrixTransform(rootInv));
  const r2 = v => Math.round(v * 100) / 100;
  const visible = { x0: r2(Math.min(corner[0].x, corner[1].x)), y0: r2(Math.min(corner[0].y, corner[1].y)), x1: r2(Math.max(corner[0].x, corner[1].x)), y1: r2(Math.max(corner[0].y, corner[1].y)) };
  const scs = getComputedStyle(svg);
  const overflow = `${scs.overflowX}/${scs.overflowY}`;
  const own = el => el.closest('svg') === svg;
  const inDefs = el => !!el.closest('defs,marker,pattern,clipPath,mask,symbol');
  const toRoot = el => rootInv.multiply(el.getScreenCTM());
  const boxTo = (m, x0, y0, x1, y1) => {
    const q = [[x0, y0], [x1, y0], [x0, y1], [x1, y1]].map(([x, y]) => new DOMPoint(x, y).matrixTransform(m));
    return { x0: Math.min(...q.map(v => v.x)), y0: Math.min(...q.map(v => v.y)), x1: Math.max(...q.map(v => v.x)), y1: Math.max(...q.map(v => v.y)) };
  };
  const rb = b => ({ x0: r2(b.x0), y0: r2(b.y0), x1: r2(b.x1), y1: r2(b.y1) });
  const num = v => {
    const f = parseFloat(v);
    return Number.isFinite(f) ? f : 0;
  };
  const groupOf = el => {
    const v = el.closest('[data-visual]');
    return { visual: v ? v.getAttribute('data-visual') : null, phase: el.closest('[data-before]') ? 'before' : el.closest('[data-after]') ? 'after' : null };
  };
  // 互斥：不同 data-visual 的帧（GSAP 交替显隐），或一个在 data-before、一个在 data-after（渐变替换的两态）
  // half：处理前 / 处理后两态同时可见（半程），不再互斥
  const exclusive = (a, b) => (a.visual !== null && b.visual !== null && a.visual !== b.visual) || (!halfState && a.phase !== null && b.phase !== null && a.phase !== b.phase);
  const vbArea = vb ? vb.w * vb.h : Infinity;
  const tagOf = el => el.tagName.toLowerCase();

  /* 文字 → 行（多行 tspan 按基线分行，各行单独量） */
  const textEls = [...svg.querySelectorAll('text')].filter(t => own(t) && !inDefs(t) && t.checkVisibility() && t.getNumberOfChars() > 0);
  const lines = [];
  textEls.forEach((t, ti) => {
    const cs = getComputedStyle(t);
    const tsp = [...t.children].filter(c => tagOf(c) === 'tspan' && c.getNumberOfChars() > 0);
    const direct = [...t.childNodes].some(nd => nd.nodeType === 3 && nd.textContent.trim());
    const parts =
      tsp.length && !direct
        ? tsp.map(s => ({ el: s, base: s.getStartPositionOfChar(0).y, bb: s.getBBox(), text: s.textContent }))
        : [{ el: t, base: t.getStartPositionOfChar(0).y, bb: t.getBBox(), text: t.textContent }];
    const groups = [];
    for (const q of parts) {
      let g = groups.find(x => Math.abs(x.base - q.base) < 0.5);
      if (!g) groups.push((g = { base: q.base, parts: [] }));
      g.parts.push(q);
    }
    const parent = t.parentElement;
    let role = 'label';
    if (parent && parent.hasAttribute('data-visual')) {
      const tt = [...parent.children].filter(c => tagOf(c) === 'text');
      role = tt[0] === t ? 'frame-title' : tt[1] === t ? 'frame-note' : 'label';
    } else if (parent && tagOf(parent) === 'g' && parent.firstElementChild && tagOf(parent.firstElementChild) === 'rect') {
      const tt = [...parent.children].filter(c => tagOf(c) === 'text');
      role = tt[0] === t ? 'box-title' : 'box-sub';
    } else if (tsp.length > 1 && groups.length === 1) role = 'readout';
    const halo = cs.stroke !== 'none' && num(cs.strokeWidth) >= 2 && /^stroke/.test(cs.paintOrder);
    const m = toRoot(t);
    const grp = groupOf(t);
    groups.forEach((g, li) => {
      const fs = Math.max(...g.parts.map(q => num(getComputedStyle(q.el).fontSize)));
      const last = g.parts.reduce((a, b) => (b.bb.x + b.bb.width > a.bb.x + a.bb.width ? b : a));
      const lsRaw = getComputedStyle(last.el).letterSpacing;
      const ls = lsRaw === 'normal' ? 0 : num(lsRaw); // 末字后面的字距不是字形，右沿扣掉
      const bx0 = Math.min(...g.parts.map(q => q.bb.x));
      const bx1 = Math.max(...g.parts.map(q => q.bb.x + q.bb.width));
      const by0 = Math.min(...g.parts.map(q => q.bb.y));
      const by1 = Math.max(...g.parts.map(q => q.bb.y + q.bb.height));
      const glyph = boxTo(m, bx0, g.base - asc * fs, bx1 - ls, g.base + desc * fs);
      const bbox = boxTo(m, bx0, by0, bx1, by1);
      lines.push({
        ti,
        li,
        nLines: groups.length,
        text: g.parts.map(q => q.text).join(''),
        role,
        fs,
        ls: r2(ls),
        family: cs.fontFamily.split(',')[0].replace(/["']/g, '').trim(),
        weight: cs.fontWeight,
        anchor: cs.textAnchor,
        halo,
        visibleNow: t.checkVisibility({ visibilityProperty: true, opacityProperty: true }),
        visual: grp.visual,
        phase: grp.phase,
        xAttr: t.getAttribute('x'),
        yAttr: t.getAttribute('y'),
        base: r2(g.base),
        glyph: rb(glyph),
        bbox: rb(bbox)
      });
    });
  });

  /* 图形（压字检查用） */
  const shapeEls = [...svg.querySelectorAll('rect,circle,ellipse,line,polyline,polygon,path')].filter(s => own(s) && !inDefs(s) && s.checkVisibility());
  const shapes = [];
  shapeEls.forEach((s, si) => {
    let bb;
    try {
      bb = s.getBBox();
    } catch {
      return;
    }
    const cs = getComputedStyle(s);
    const tag = tagOf(s);
    const m = toRoot(s);
    const mScale = Math.hypot(m.a, m.b);
    const sw = cs.stroke !== 'none' && num(cs.strokeOpacity) > 0 ? num(cs.strokeWidth) : 0;
    const hasFill = tag !== 'line' && cs.fill !== 'none' && num(cs.fillOpacity) > 0;
    if (!sw && !hasFill) return;
    const box = boxTo(m, bb.x - sw / 2, bb.y - sw / 2, bb.x + bb.width + sw / 2, bb.y + bb.height + sw / 2);
    const background = tag === 'rect' && bb.width * bb.height >= fullVbRatio * vbArea;
    const attrs = ['x', 'y', 'width', 'height', 'x1', 'y1', 'x2', 'y2', 'cx', 'cy', 'r', 'rx', 'ry']
      .filter(k => s.hasAttribute(k))
      .map(k => `${k}=${s.getAttribute(k)}`)
      .join(' ');
    const geo = tag === 'path' ? `d=${(s.getAttribute('d') || '').slice(0, 48)}` : tag === 'polyline' || tag === 'polygon' ? `points=${(s.getAttribute('points') || '').slice(0, 48)}` : attrs;
    shapes.push({
      si,
      el: s,
      tag,
      m,
      mScale,
      sw,
      hasFill,
      box,
      background,
      grp: groupOf(s),
      desc: `${tag} ${geo}`.trim(),
      paint: `${sw ? `stroke=${cs.stroke} w=${sw}${cs.strokeDasharray !== 'none' ? ` dash=${cs.strokeDasharray}` : ''}` : 'stroke=none'}${hasFill ? ` fill=${cs.fill}` : ''} opacity=${cs.opacity}`
    });
  });
  const rects = shapes.filter(s => s.tag === 'rect' && !s.background);

  /* 每行：所在的框（Box 一类）与离框边的余量；压在别的图形上的深度 */
  const fits = [];
  const shapeHits = [];
  const nearShapes = [];
  lines.forEach((L, idx) => {
    const G = L.glyph;
    const lg = { visual: L.visual, phase: L.phase };
    const cx = (G.x0 + G.x1) / 2;
    const cy = (G.y0 + G.y1) / 2;
    let container = null;
    if (!L.halo) {
      for (const r of rects) {
        if (exclusive(lg, r.grp)) continue;
        const b = r.box;
        if (cx < b.x0 || cx > b.x1 || cy < b.y0 || cy > b.y1) continue;
        if (b.y1 - b.y0 < L.fs) continue;
        const area = (b.x1 - b.x0) * (b.y1 - b.y0);
        if (!container || area < container.area) container = { r, area };
      }
      if (container) {
        // 框的内沿：描边一半算进框边
        const b = container.r.box;
        const half = container.r.sw / 2;
        const mg = { l: G.x0 - (b.x0 + half * 2), r: b.x1 - half * 2 - G.x1, t: G.y0 - (b.y0 + half * 2), b: b.y1 - half * 2 - G.y1 };
        fits.push({ line: idx, rect: container.r.desc, rectBox: rb(b), margins: { l: r2(mg.l), r: r2(mg.r), t: r2(mg.t), b: r2(mg.b) } });
      }
    }
    if (L.halo) return; // 带底色描边的标签是有意压在虚线框上的（把线断开），不查压图形
    const Gs = { x0: G.x0 + 0.5, y0: G.y0 + 0.5, x1: G.x1 - 0.5, y1: G.y1 - 0.5 };
    const Gn = { x0: G.x0 - nearShapeGap, y0: G.y0 - nearShapeGap, x1: G.x1 + nearShapeGap, y1: G.y1 + nearShapeGap }; // 贴近判定的外扩盒
    for (const s of shapes) {
      if (s.background) continue;
      if (container && s === container.r) continue;
      if (exclusive(lg, s.grp)) continue;
      const b = s.box;
      if (b.x1 < Gn.x0 || b.x0 > Gn.x1 || b.y1 < Gn.y0 || b.y0 > Gn.y1) continue;
      let depth = -Infinity;
      let strokePts = 0;
      let fillArea = 0;
      if (s.sw > 0) {
        let len = 0;
        try {
          len = s.el.getTotalLength();
        } catch {
          len = 0;
        }
        if (len > 0) {
          const n = Math.min(6000, Math.ceil(len / stepAlong));
          const half = (s.sw * s.mScale) / 2;
          for (let k = 0; k <= n; k++) {
            const q = s.el.getPointAtLength((k * len) / n).matrixTransform(s.m);
            const d = Math.min(q.x - G.x0, G.x1 - q.x, q.y - G.y0, G.y1 - q.y) + half;
            if (d > 0) strokePts++;
            if (d > depth) depth = d; // 负值 = 在盒子外面，绝对值是离得多远（四边取最大偏离）
          }
        }
      }
      if (s.hasFill) {
        const inv = s.m.inverse();
        const ix0 = Math.max(b.x0, Gn.x0);
        const ix1 = Math.min(b.x1, Gn.x1);
        const iy0 = Math.max(b.y0, Gn.y0);
        const iy1 = Math.min(b.y1, Gn.y1);
        for (let x = ix0 + stepGrid / 2; x < ix1; x += stepGrid) {
          for (let y = iy0 + stepGrid / 2; y < iy1; y += stepGrid) {
            const q = new DOMPoint(x, y).matrixTransform(inv);
            if (s.el.isPointInFill(q)) {
              const d = Math.min(x - G.x0, G.x1 - x, y - G.y0, G.y1 - y);
              if (d > 0) fillArea += stepGrid * stepGrid;
              if (d > depth) depth = d;
            }
          }
        }
      }
      if (depth >= shapeDepthMin) shapeHits.push({ line: idx, shape: s.desc, paint: s.paint, shapeVisual: s.grp.visual, shapePhase: s.grp.phase, depth: r2(depth), strokePts, fillArea: r2(fillArea) });
      else if (depth > -nearShapeGap) nearShapes.push({ line: idx, shape: s.desc, paint: s.paint, shapeVisual: s.grp.visual, shapePhase: s.grp.phase, gap: r2(Math.max(0, -depth)) });
    }
  });

  return { scale: r2(scale * 1000) / 1000, vb, visible, overflow, lines, fits, shapeHits, nearShapes, nShapes: shapes.length };
}

/* ---------------- 纯逻辑：判定（不碰浏览器，可单独测） ---------------- */

const isExclusive = (a, b, half = false) => (a.visual !== null && b.visual !== null && a.visual !== b.visual) || (!half && a.phase !== null && b.phase !== null && a.phase !== b.phase);
const round2 = v => Math.round(v * 100) / 100;

/** 一个 SVG 的量值 → 本页命中（重叠、贴近、越界、压图形、框内太挤）。 */
function judgeSvg(meas, cfg) {
  const out = [];
  const L = meas.lines;
  for (let i = 0; i < L.length; i++) {
    for (let j = i + 1; j < L.length; j++) {
      const a = L[i];
      const b = L[j];
      if (a.ti === b.ti) continue; // 同一段 <text> 的上下行
      if (isExclusive(a, b, cfg.half)) continue;
      const oy = Math.min(a.glyph.y1, b.glyph.y1) - Math.max(a.glyph.y0, b.glyph.y0);
      if (oy <= 0) continue;
      const ox = Math.min(a.glyph.x1, b.glyph.x1) - Math.max(a.glyph.x0, b.glyph.x0);
      const [left, right] = a.glyph.x0 <= b.glyph.x0 ? [a, b] : [b, a];
      if (ox > 0 && ox * oy > cfg.overlapMinArea) {
        out.push({ kind: 'overlap', a: left, b: right, value: round2(ox), overlapY: round2(oy), area: round2(ox * oy) });
      } else if (-ox < cfg.nearGap) {
        out.push({ kind: 'near-touch', a: left, b: right, value: round2(Math.max(0, -ox)), overlapY: round2(oy) });
      }
    }
  }
  if (meas.vb) {
    const { x, y, w, h } = meas.vb;
    for (const a of L) {
      const over = { left: x - a.glyph.x0, right: a.glyph.x1 - (x + w), top: y - a.glyph.y0, bottom: a.glyph.y1 - (y + h) };
      const worstSide = Object.entries(over).sort((p, q) => q[1] - p[1])[0];
      if (worstSide[1] > cfg.oobTol) {
        const vis = meas.visible;
        const clippedBy = { left: vis.x0 - a.glyph.x0, right: a.glyph.x1 - vis.x1, top: vis.y0 - a.glyph.y0, bottom: a.glyph.y1 - vis.y1 }[worstSide[0]];
        const edge = { left: `viewBox 左边界 x=${x}`, right: `viewBox 右边界 x=${x + w}`, top: `viewBox 上边界 y=${y}`, bottom: `viewBox 下边界 y=${y + h}` }[worstSide[0]];
        out.push({ kind: 'out-of-bounds', a, edge, side: worstSide[0], value: round2(worstSide[1]), clippedAmount: round2(clippedBy), clipped: meas.overflow.includes('hidden') || meas.overflow.includes('clip') });
      }
    }
  }
  for (const f of meas.fits) {
    const a = L[f.line];
    const mh = Math.min(f.margins.l, f.margins.r);
    const mv = Math.min(f.margins.t, f.margins.b);
    if (mh < 0 || mv < 0) out.push({ kind: 'text-over-shape', sub: 'crosses-container', a, rect: f.rect, rectBox: f.rectBox, margins: f.margins, value: round2(-Math.min(mh, mv)) });
    else if (mh < cfg.fitTight) out.push({ kind: 'too-tight-in-box', a, rect: f.rect, rectBox: f.rectBox, margins: f.margins, value: round2(mh) });
  }
  for (const s of meas.nearShapes ?? []) {
    out.push({ kind: 'near-shape', a: L[s.line], shape: s.shape, paint: s.paint, shapeVisual: s.shapeVisual, shapePhase: s.shapePhase, value: s.gap });
  }
  for (const s of meas.shapeHits) {
    out.push({ kind: 'text-over-shape', sub: 'shape', a: L[s.line], shape: s.shape, paint: s.paint, shapeVisual: s.shapeVisual, shapePhase: s.shapePhase, value: s.depth, strokePts: s.strokePts, fillArea: s.fillArea });
  }
  return out;
}

/** 这个 SVG 是哪个组件、哪张图。 */
function describeSvg(info, figLabelToId) {
  if (info.inProcess) return { component: 'ProcessSection', name: 'ProcessSection 四道工序示意图' };
  if (info.inFigure && info.aria) {
    const id = figLabelToId[info.aria] || info.aria;
    const vbW = Number((info.viewBox || '').split(/\s+/)[2]);
    const layout = vbW === 720 ? 'wide' : vbW === 340 ? 'narrow' : `vb${vbW}`;
    return { component: 'Figures', figure: id, layout, name: `figure:${id} ${layout === 'wide' ? '横排' : layout === 'narrow' ? '竖排' : layout}` };
  }
  if (info.viewBox === '0 0 200 72') {
    const art = ['ArtAB', 'ArtDiff', 'ArtRef'][info.liIndex] || `art${info.liIndex}`;
    return { component: 'Bento', art, name: `Bento 伍·判断 ${art}` };
  }
  if (info.viewBox === '0 0 280 120') return { component: 'AlgoTeaser', art: info.liTag, name: `AlgoTeaser 陆·公开算法 ${info.liTag}` };
  if (info.viewBox === '0 0 400 440') return { component: 'GlazeKnob', name: `GlazeKnob（${info.sectionIndex === 0 ? 'Hero 首屏' : 'GlazeSection 贰·施釉'}）` };
  return { component: 'unknown', name: `svg#${info.id} viewBox=${info.viewBox} section=${info.sectionId ?? info.sectionIndex}` };
}

function frameLabel(desc, line) {
  if (desc.component === 'ProcessSection' && line.visual !== null) return `帧 ${FRAME_NAMES[Number(line.visual)] ?? line.visual}`;
  return null;
}

/* ---------------- 源码对位（读文件，纯查表） ---------------- */

const COMPONENT_FILE = {
  ProcessSection: 'src/components/celadon/ProcessSection.tsx',
  Figures: 'src/components/docs/Figures.tsx',
  Bento: 'src/components/celadon/Bento.tsx',
  AlgoTeaser: 'src/components/celadon/AlgoTeaser.tsx',
  GlazeKnob: 'src/components/celadon/GlazeKnob.tsx'
};
const COMPONENT_NS = { ProcessSection: 'home', Bento: 'home', AlgoTeaser: 'home', Figures: 'figures' };
// 每类 <text> 真正画出来的那一行（按角色）
const RENDER_LINE_RE = {
  ProcessSection: { 'frame-title': /<text x="40" y="44"/, 'frame-note': /<text x=\{W - 40\} y="44"/ },
  Figures: { 'box-title': /<text x=\{r1\(cx\)\} y=\{r1\(top \+ v\.T \* 0\.85\)\}/, 'box-sub': /<text key=\{i\} x=\{r1\(cx\)\}/, label: /^\s*<text\s*$/ },
  GlazeKnob: { readout: /<text y=\{CY \+ R_DISC \+ 60\}/, label: /<text x=\{CX\} y=\{CY \+ R_DISC \+ 84\}/ }
};

function flattenJson(obj, prefix = '') {
  const out = [];
  for (const [k, v] of Object.entries(obj)) {
    const key = prefix ? `${prefix}.${k}` : k;
    if (v && typeof v === 'object') out.push(...flattenJson(v, key));
    else if (typeof v === 'string') out.push([key, v]);
  }
  return out;
}

function loadSourceIndex() {
  const files = {};
  for (const [c, rel] of Object.entries(COMPONENT_FILE)) files[c] = fs.readFileSync(path.join(SITE, rel), 'utf8').split('\n');
  const i18n = {};
  for (const lang of ['en', 'zh']) {
    for (const ns of ['home', 'figures']) {
      const rel = `src/i18n/locales/${lang}/${ns}.json`;
      const raw = fs.readFileSync(path.join(SITE, rel), 'utf8');
      i18n[`${lang}/${ns}`] = { rel, lines: raw.split('\n'), entries: flattenJson(JSON.parse(raw)) };
    }
  }
  return { files, i18n };
}

function lookupI18n(idx, lang, ns, text) {
  const pack = idx.i18n[`${lang}/${ns}`];
  if (!pack) return [];
  const hits = [];
  for (const [key, val] of pack.entries) {
    if (val === text) hits.push({ key, how: 'exact', val });
    else if (val.includes('\n') && val.split('\n').includes(text)) hits.push({ key, how: 'line', val });
    else if (val.includes('{{')) {
      const re = new RegExp(`^${val.replace(/[.*+?^${}()|[\]\\]/g, '\\$&').replace(/\\\{\\\{\w+\\\}\\\}/g, '.+?')}$`);
      if (re.test(text)) hits.push({ key, how: 'template', val });
    }
  }
  if (!hits.length) {
    const m = /^\d+ = (.+)$/.exec(text); // 延迟图：`${total} = ${t(note)}`
    if (m) return lookupI18n(idx, lang, ns, m[1]).map(h => ({ ...h, how: `${h.how}+prefix` }));
  }
  return hits;
}

function enclosingFunction(lines, lineIdx) {
  for (let i = lineIdx; i >= 0; i--) {
    const m = /^(?:export\s+)?function\s+(\w+)/.exec(lines[i]);
    if (m) return m[1];
  }
  return null;
}

function pickByLayout(lines, cands, layout) {
  if (cands.length <= 1 || !layout) return cands;
  const want = layout === 'wide' ? /Wide$/ : /Narrow$/;
  const avoid = layout === 'wide' ? /Narrow$/ : /Wide$/;
  const preferred = cands.filter(i => want.test(enclosingFunction(lines, i) || ''));
  if (preferred.length) return preferred;
  const neutral = cands.filter(i => !avoid.test(enclosingFunction(lines, i) || ''));
  return neutral.length ? neutral : cands;
}

function grepLines(lines, needle) {
  const out = [];
  lines.forEach((l, i) => {
    if (l.includes(needle)) out.push(i);
  });
  return out;
}

/** 一段文字 → 源码位置描述（file:line）。 */
function sourceOf(idx, desc, line, lang) {
  const comp = desc.component;
  const rel = COMPONENT_FILE[comp];
  if (!rel) return { text: '（未识别组件）' };
  const lines = idx.files[comp];
  const relRepo = `site/${rel}`;
  const parts = [];
  const ns = COMPONENT_NS[comp];
  const keys = ns ? lookupI18n(idx, lang, ns, line.text) : [];
  let callLines = [];
  for (const k of keys) {
    const short = k.key;
    let c = [...grepLines(lines, `'${short}'`), ...grepLines(lines, `"${short}"`)];
    if (!c.length && comp === 'AlgoTeaser' && /^algorithms\.items\.\d+\.art$/.test(short)) {
      const order = { BALANCE: 0, LIMIT: 1, COLOR: 2 }[desc.art];
      const capLines = grepLines(lines, '{caption}');
      if (order !== undefined && capLines[order] !== undefined) c = [capLines[order]];
    }
    callLines.push(...c);
    const pack = idx.i18n[`${lang}/${ns}`];
    const jl = pack.lines.findIndex(l => l.includes(JSON.stringify(k.val)));
    parts.push(`${pack.rel.replace(/^src/, 'site/src')}:${jl + 1}（${short}${k.how !== 'exact' ? `，${k.how}` : ''}）`);
  }
  if (!callLines.length) {
    // 字面量：标题、读数、两种语言相同的字
    const lit = [...grepLines(lines, `'${line.text}'`), ...grepLines(lines, `"${line.text}"`), ...grepLines(lines, `>${line.text}<`)];
    const litLoose = lit.length ? lit : grepLines(lines, line.text.trim());
    callLines.push(...litLoose);
  }
  callLines = [...new Set(callLines)].sort((p, q) => p - q);
  callLines = pickByLayout(lines, callLines, desc.layout);
  const roleRe = RENDER_LINE_RE[comp]?.[line.role];
  let renderLine = null;
  if (roleRe) {
    if (comp === 'Figures' && line.role === 'label') {
      const labelFn = lines.findIndex(l => /^function Label\(/.test(l));
      const i = lines.findIndex((l, k) => k > labelFn && roleRe.test(l));
      renderLine = i >= 0 ? i : null;
    } else {
      const i = lines.findIndex(l => roleRe.test(l));
      renderLine = i >= 0 ? i : null;
    }
  }
  const call = callLines.length ? callLines.map(i => `${i + 1}`).join('/') : '?';
  const fn = callLines.length ? enclosingFunction(lines, callLines[0]) : null;
  let text = `${relRepo}:${call}${fn ? `（${fn}）` : ''}`;
  if (renderLine !== null && !callLines.includes(renderLine)) text += ` → <text> 在 :${renderLine + 1}`;
  if (parts.length) text += `；文案 ${parts.join('，')}`;
  return { text, callLines: callLines.map(i => i + 1), renderLine: renderLine === null ? null : renderLine + 1, keys: keys.map(k => k.key) };
}

/* ---------------- 一页：打开、滚一遍、逐个 SVG 量 ---------------- */

async function measurePage(browser, route, width, cfg) {
  const lang = route === '/zh' || route.startsWith('/zh/') ? 'zh' : 'en';
  const t0 = Date.now();
  const ctx = await browser.newContext({ viewport: { width, height: cfg.height }, deviceScaleFactor: 1, reducedMotion: cfg.reduced ? 'reduce' : 'no-preference' });
  const page = await ctx.newPage();
  const errors = [];
  page.on('pageerror', e => errors.push(String(e.message || e).slice(0, 200)));
  try {
    const resp = await page.goto(cfg.base + route, { waitUntil: 'networkidle', timeout: cfg.gotoTimeoutMs });
    const status = resp ? resp.status() : null;
    if (route.includes('/docs') || route.includes('/algorithms')) {
      await page.waitForSelector('.prose-celadon', { timeout: 15000 }).catch(() => errors.push('prose-celadon 未出现'));
    }
    await page.evaluate(() => document.fonts.ready);
    // 半程：处理前 / 处理后各 50 % 同时可见（GSAP 的 scrub 在这两组上设的是内联 opacity，!important 压过它）
    if (cfg.half) await page.addStyleTag({ content: '[data-before],[data-after]{opacity:.5 !important}' });
    const scroll = await page.evaluate(pageScrollPass, { stepRatio: cfg.scrollStepRatio, stepWaitMs: cfg.scrollStepWaitMs, endWaitMs: cfg.scrollEndWaitMs });
    const pageInfo = await page.evaluate(() => ({
      htmlLang: document.documentElement.lang,
      title: document.title,
      notFound: !!document.querySelector('main .display') && /404/.test(document.querySelector('main')?.textContent || ''),
      path: location.pathname
    }));
    const svgs = await page.evaluate(pageListSvgs);
    const measured = [];
    for (const info of svgs) {
      if (info.nText === 0) continue;
      if (!info.displayed) {
        measured.push({ info, skipped: 'display:none（当前屏宽不显示）' });
        continue;
      }
      await page.evaluate(pageScrollToSvg, { id: info.id, settleMs: cfg.settleMs });
      const fontState = await page.evaluate(pageFontsReady, { id: info.id, zh: lang === 'zh', waitCjkMs: cfg.waitCjkMs });
      const args = { id: info.id, asc: cfg.glyphAscent, desc: cfg.glyphDescent, shapeDepthMin: cfg.shapeDepthMin, stepAlong: cfg.stepAlong, stepGrid: cfg.stepGrid, fullVbRatio: cfg.fullVbRatio, halfState: !!cfg.half, nearShapeGap: cfg.nearShapeGap };
      let m1 = await page.evaluate(pageMeasureSvg, args);
      await page.waitForTimeout(cfg.restableMs);
      let m2 = await page.evaluate(pageMeasureSvg, args);
      let drift = maxDrift(m1, m2);
      let restabilized = false;
      if (drift > cfg.stableTol) {
        await page.waitForTimeout(1500);
        await page.evaluate(pageFontsReady, { id: info.id, zh: lang === 'zh', waitCjkMs: 2000 });
        m1 = m2;
        m2 = await page.evaluate(pageMeasureSvg, args);
        drift = maxDrift(m1, m2);
        restabilized = true;
      }
      measured.push({ info, fontState, drift: round2(drift), restabilized, m: m2 });
    }
    return { route, width, lang, ok: true, status, ms: Date.now() - t0, scroll, pageInfo, nSvg: svgs.length, nSvgWithText: svgs.filter(s => s.nText > 0).length, measured, errors };
  } catch (e) {
    return { route, width, lang, ok: false, ms: Date.now() - t0, error: String(e.message || e).slice(0, 300), errors };
  } finally {
    await ctx.close().catch(() => null);
  }
}

function maxDrift(a, b) {
  if (!a || !b || a.error || b.error || a.lines.length !== b.lines.length) return Infinity;
  let d = 0;
  a.lines.forEach((l, i) => {
    const q = b.lines[i];
    d = Math.max(d, Math.abs(l.glyph.x0 - q.glyph.x0), Math.abs(l.glyph.x1 - q.glyph.x1), Math.abs(l.glyph.y0 - q.glyph.y0), Math.abs(l.glyph.y1 - q.glyph.y1));
  });
  return d;
}

/* ---------------- 汇总：跨屏宽去重、对源码、ProcessSection 对照表 ---------------- */

function pairKey(o, desc) {
  const fa = frameLabel(desc, o.a);
  const base = `${desc.name}${fa ? ` ${fa}` : ''}`;
  if (o.kind === 'out-of-bounds') return `${base}|${o.kind}|${o.a.text}|${o.side}`;
  if (o.kind === 'too-tight-in-box' || (o.kind === 'text-over-shape' && o.sub === 'crosses-container')) return `${base}|${o.kind}|${o.a.text}|${o.rect}`;
  if (o.kind === 'text-over-shape' || o.kind === 'near-shape') return `${base}|${o.kind}|${o.a.text}|${o.shape}`;
  return `${base}|${o.kind}|${o.a.text}|${o.b.text}`;
}

function summarize(pages, idx) {
  const groups = new Map();
  const figLabelToId = buildFigLabelMap(idx);
  for (const pg of pages) {
    if (!pg.ok) continue;
    for (const s of pg.measured) {
      if (!s.m || s.m.error) continue;
      const desc = describeSvg(s.info, figLabelToId);
      for (const o of judgeSvg(s.m, CFG)) {
        const key = `${pg.lang}|${pairKey(o, desc)}`;
        let g = groups.get(key);
        if (!g) {
          g = { key, kind: o.kind, sub: o.sub ?? null, lang: pg.lang, desc, frame: frameLabel(desc, o.a), textA: o.a.text, textB: textBOf(o), roleA: o.a.role, roleB: o.b?.role ?? null, routes: new Set(), widths: new Set(), occurrences: [], worst: null };
          g.sourceA = sourceOf(idx, desc, o.a, pg.lang);
          if (o.b) g.sourceB = sourceOf(idx, desc, o.b, pg.lang);
          groups.set(key, g);
        }
        g.routes.add(pg.route);
        g.widths.add(pg.width);
        const occ = { route: pg.route, width: pg.width, scale: s.m.scale, value: o.value, a: o.a.glyph, b: o.b?.glyph, overlapY: o.overlapY, margins: o.margins, clipped: o.clipped, clippedAmount: o.clippedAmount, strokePts: o.strokePts, fillArea: o.fillArea, aBBox: o.a.bbox };
        g.occurrences.push(occ);
        const better = (v, w) => (NEAR_KINDS.has(o.kind) ? v < w : v > w);
        if (!g.worst || better(o.value, g.worst.value)) g.worst = { value: o.value, width: pg.width, route: pg.route, scale: s.m.scale };
      }
    }
  }
  const list = [...groups.values()].map(g => ({ ...g, routes: [...g.routes].sort(), widths: [...g.widths].sort((a, b) => a - b) }));
  list.sort((a, b) => kindRank(a.kind) - kindRank(b.kind) || (NEAR_KINDS.has(a.kind) ? a.worst.value - b.worst.value : b.worst.value - a.worst.value));
  return list;
}

// 贴近类：数值越小越严重（离得越近）
const NEAR_KINDS = new Set(['near-touch', 'too-tight-in-box', 'near-shape']);
const kindRank = k => ({ overlap: 0, 'out-of-bounds': 1, 'text-over-shape': 2, 'too-tight-in-box': 3, 'near-touch': 4, 'near-shape': 5 })[k] ?? 9;

function textBOf(o) {
  if (o.kind === 'out-of-bounds') return o.edge;
  if (o.kind === 'too-tight-in-box' || o.sub === 'crosses-container') return `所在框 ${o.rect}`;
  if (o.kind === 'text-over-shape' || o.kind === 'near-shape') return `图形 ${o.shape}（${o.paint}）`;
  return o.b.text;
}

function buildFigLabelMap(idx) {
  const map = {};
  for (const lang of ['en', 'zh']) {
    for (const [k, v] of idx.i18n[`${lang}/figures`].entries) {
      const m = /^(\w+)\.label$/.exec(k);
      if (m) map[v] = m[1];
    }
  }
  return map;
}

/** 首页四帧的标题 / 说明 x 区间：每个语言 × 屏宽一行（对照已知问题）。 */
function processFrameTable(pages) {
  const rows = [];
  for (const pg of pages) {
    if (!pg.ok) continue;
    // 正常模式四帧叠在一张 SVG 里；减少动效时每帧一张 SVG（几何相同）
    const svgs = pg.measured.filter(x => x.info.inProcess && x.m && !x.m.error);
    for (const s of svgs) {
      for (let v = 0; v < 4; v++) {
        const title = s.m.lines.find(l => l.visual === String(v) && l.role === 'frame-title');
        const note = s.m.lines.find(l => l.visual === String(v) && l.role === 'frame-note');
        if (!title || !note) continue;
        const ox = round2(title.glyph.x1 - note.glyph.x0);
        rows.push({ lang: pg.lang, width: pg.width, scale: s.m.scale, frame: FRAME_NAMES[v], title: [title.glyph.x0, title.glyph.x1], note: [note.glyph.x0, note.glyph.x1], titleBBoxX1: title.bbox.x1, overlap: ox > 0 ? ox : 0, gap: ox > 0 ? null : -ox });
      }
    }
  }
  return rows.sort((a, b) => a.lang.localeCompare(b.lang) || FRAME_NAMES.indexOf(a.frame) - FRAME_NAMES.indexOf(b.frame) || a.width - b.width);
}

function coverageOf(pages, plannedRoutes, plannedWidths) {
  const ok = pages.filter(p => p.ok);
  const failed = pages.filter(p => !p.ok).map(p => ({ route: p.route, width: p.width, error: p.error }));
  let svgTotal = 0;
  let svgWithText = 0;
  let svgMeasured = 0;
  let svgSkipped = 0;
  let textEls = 0;
  let textLines = 0;
  const skippedSamples = new Map();
  const unstable = [];
  const notes = [];
  for (const p of ok) {
    svgTotal += p.nSvg;
    svgWithText += p.nSvgWithText;
    for (const s of p.measured) {
      if (s.skipped) {
        svgSkipped++;
        const k = `${s.info.aria || s.info.viewBox}`;
        skippedSamples.set(k, (skippedSamples.get(k) || 0) + 1);
        continue;
      }
      if (!s.m || s.m.error) {
        notes.push({ route: p.route, width: p.width, svg: s.info.id, error: s.m?.error });
        continue;
      }
      svgMeasured++;
      textEls += new Set(s.m.lines.map(l => l.ti)).size;
      textLines += s.m.lines.length;
      if (s.restabilized || s.drift > CFG.stableTol) unstable.push({ route: p.route, width: p.width, svg: s.info.aria || s.info.viewBox, drift: s.drift, restabilized: s.restabilized });
    }
    if (p.errors.length) notes.push({ route: p.route, width: p.width, pageErrors: p.errors });
    if (p.pageInfo?.notFound) notes.push({ route: p.route, width: p.width, warn: '页面是 404' });
    if (p.lang === 'zh') {
      const noCjk = p.measured.filter(s => s.fontState && !s.fontState.cjkLoaded);
      if (noCjk.length) notes.push({ route: p.route, width: p.width, warn: `中文页思源未加载完就量了 ${noCjk.length} 个 SVG` });
    }
  }
  return {
    routesPlanned: plannedRoutes.length,
    widthsPlanned: plannedWidths.length,
    pagesPlanned: plannedRoutes.length * plannedWidths.length,
    pagesOk: ok.length,
    pagesFailed: failed,
    svgTotal,
    svgWithText,
    svgMeasured,
    svgSkippedDisplayNone: svgSkipped,
    skippedByFigure: Object.fromEntries(skippedSamples),
    textElementsMeasured: textEls,
    textLinesMeasured: textLines,
    unstable,
    notes,
    routesWithTextSvg: [...new Set(ok.filter(p => p.measured.some(s => s.m && !s.m.error)).map(p => p.route))].sort()
  };
}

/* ---------------- 主流程 ---------------- */

async function runPool(jobs, n, worker) {
  const results = [];
  let next = 0;
  let done = 0;
  const lanes = Array.from({ length: n }, async () => {
    while (next < jobs.length) {
      const job = jobs[next++];
      const r = await worker(job);
      results.push(r);
      done++;
      if (done % 20 === 0 || done === jobs.length) console.log(`[${new Date().toISOString().slice(11, 19)}] ${done}/${jobs.length}`);
    }
  });
  await Promise.all(lanes);
  return results;
}

async function main() {
  const args = parseArgs(process.argv);
  Object.assign(CFG, Object.fromEntries(Object.entries({ base: args.base, concurrency: args.concurrency, reduced: args.reduced, half: args.half }).filter(([, v]) => v !== undefined)));
  const engine = args.engine ?? 'chromium';
  if (!pw[engine]) throw new Error(`未知引擎 ${engine}（chromium / firefox / webkit）`);
  const routes = args.routes ?? ROUTES;
  const widths = args.widths ?? WIDTHS;
  const out = path.resolve(HERE, args.out ?? 'result.json');
  const jobs = routes.flatMap(r => widths.map(w => ({ route: r, width: w })));
  if (args.dryRun) {
    console.log(`预演：${routes.length} 路由 × ${widths.length} 屏宽 = ${jobs.length} 页，基址 ${CFG.base}，并发 ${CFG.concurrency}，输出 ${out}`);
    return;
  }
  const t0 = Date.now();
  const browser = await pw[engine].launch();
  const version = browser.version();
  let pages = await runPool(jobs, CFG.concurrency, job => measurePage(browser, job.route, job.width, CFG));
  // 失败的页再试一次（串行）
  const failed = pages.filter(p => !p.ok);
  for (const f of failed) {
    const r = await measurePage(browser, f.route, f.width, CFG);
    r.retried = true;
    pages = pages.map(p => (p === f ? r : p));
  }
  await browser.close();
  pages.sort((a, b) => routes.indexOf(a.route) - routes.indexOf(b.route) || a.width - b.width);

  const idx = loadSourceIndex();
  const groups = summarize(pages, idx);
  const coverage = coverageOf(pages, routes, widths);
  const frames = processFrameTable(pages);
  const strip = g => ({ ...g, occurrences: g.occurrences.sort((a, b) => a.width - b.width || a.route.localeCompare(b.route)) });
  const result = {
    meta: { base: CFG.base, generatedAt: new Date().toISOString(), engine, browserVersion: version, chromium: engine === 'chromium' ? version : undefined, half: !!CFG.half, playwright: '1.62.1', dpr: 1, height: CFG.height, reducedMotion: !!CFG.reduced, durationSec: Math.round((Date.now() - t0) / 1000), config: CFG },
    coverage,
    hits: groups.filter(g => ['overlap', 'out-of-bounds', 'text-over-shape'].includes(g.kind)).map(strip),
    nearMisses: groups.filter(g => NEAR_KINDS.has(g.kind)).map(strip),
    processFrames: frames,
    perPage: pages.map(p => ({ route: p.route, width: p.width, lang: p.lang, ok: p.ok, ms: p.ms, error: p.error, retried: p.retried, nSvg: p.nSvg, nSvgWithText: p.nSvgWithText, svgs: (p.measured || []).map(s => ({ id: s.info.id, viewBox: s.info.viewBox, aria: s.info.aria, skipped: s.skipped, scale: s.m?.scale, nLines: s.m?.lines?.length, drift: s.drift })) }))
  };
  fs.writeFileSync(out, JSON.stringify(result, null, 1));
  const rawOut = out.replace(/\.json$/, '-raw.json');
  fs.writeFileSync(rawOut, JSON.stringify(pages.map(p => ({ ...p, measured: (p.measured || []).map(s => ({ ...s, m: s.m && !s.m.error ? { ...s.m } : s.m })) }))));
  console.log(`完成：${coverage.pagesOk}/${coverage.pagesPlanned} 页，量了 ${coverage.svgMeasured} 个 SVG、${coverage.textElementsMeasured} 段 <text>（${coverage.textLinesMeasured} 行），用时 ${result.meta.durationSec} s`);
  console.log(`命中 ${result.hits.length} 组，贴近 / 太挤 ${result.nearMisses.length} 组 → ${out}`);
  for (const g of [...result.hits, ...result.nearMisses]) {
    console.log(`  [${g.kind}${g.sub ? `/${g.sub}` : ''}] ${g.lang} ${g.desc.name}${g.frame ? ` ${g.frame}` : ''}：「${g.textA}」×「${g.textB}」 最坏 ${g.worst.value} @${g.worst.width}（${g.widths.join(',')}）`);
  }
}

export { judgeSvg, describeSvg, isExclusive, lookupI18n, sourceOf, summarize, processFrameTable, loadSourceIndex, buildFigLabelMap, CFG };

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main().catch(e => {
    console.error(e);
    process.exit(1);
  });
}
