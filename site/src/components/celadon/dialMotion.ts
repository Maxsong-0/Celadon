/**
 * 旋钮的「彗尾 + 弹簧读数 + 外圈手感」纯计算部分（T-101）：逐条移植插件 Source/ui/DialMotion.{h,cpp}（T-100，ADR-037），
 * 插件又是照 React Bits 的 CometDial 移植的。常数名、公式、判停条件与插件一一对应，注释里写插件的名字；
 * 插件改了 DialMotion.h 的手感常数，这里同步改，再跑 `pnpm run check:dial`（复现插件 tests/DialMotionTests.cpp）。
 *
 * 不碰 DOM、不碰 React：控件行为在 useDialControl.ts（↔ 插件 Dial.cpp 的 DialControl），画法在 GlazeKnob.tsx。
 *
 * 单位：位置用 0 ～ 1 比例，速度用「量程 / 秒」，时间用秒（甩动历史用毫秒，与原版一致）。
 * 角度用弧度：0 = 12 点方向、顺时针为正（屏幕 y 向下），270° 轨道从 −135° 到 +135°——与网站 GlazeKnob 的
 * START −135 / SWEEP 270（度）是同一个约定。
 */

// ---- 几何（DialMotion.h startAngle / endAngle / sweep）：270° 轨道 ----
export const startAngle = -0.75 * Math.PI;
export const endAngle = 0.75 * Math.PI;
export const sweep = endAngle - startAngle;

// ---- 手感常数（DialMotion.h 同名常数，原版 CometDial 的默认值；调手感只改这里）----
/** K：彗尾分几段 */
export const cometSegmentCount = 12;
/** V_FULL：量程 / 秒，彗尾到满长满宽 */
export const fullSpeed = 4;
/** V_FLICK：松手速度到这里回弹系数取 flickBounce */
export const flickSpeed = 2;
/** TAU_FOLLOW：拖动时读数追手的时间常数（秒） */
export const followSeconds = 0.06;
/** TAU_V：彗尾用的速度平滑时间常数（秒） */
export const speedSmoothSeconds = 0.05;
/** DECEL：惯性滑行的衰减（每毫秒） */
export const deceleration = 0.99;
/** R：原版旋钮的半径（DRAG_PX、DEAD 都是按它定的） */
export const originalRadius = 80;
/** DRAG_PX：外圈按下后移动超过它才算拖（之前是点按）；原版在 R 80 上，约 7° */
export const dragThresholdPixels = 10;
/** 按半径换算出来的门槛最少这么多：手点一下的抖动不算拖 */
export const minimumDragPixels = 4;
/** STALE_MS：最后一次移动距松手超过它就不算甩 */
export const staleReleaseMs = 80;
/** speed（0 ～ 100）：弹簧多快到位 */
export const responsiveness = 25;
/** 点按 / 外部改值后的回弹 */
export const tapBounce = 0.2;
/** 满速甩之后的回弹 */
export const flickBounce = 0.1;
/** 甩出去能多滑多远（0 = 松手处停下） */
export const momentum = 1;
/** cometReach 180°：满速时尾巴的角长 */
export const cometReach = Math.PI;
/** DEAD 44 / R 80：中间多大一块不算外圈 */
export const deadZoneRatio = 0.55;
/** 一帧最多推进这么久（卡顿后不一下子跳完） */
export const maxFrameSeconds = 0.05;
/** 平滑后的速度低于它（量程 / 秒）算停下 */
export const restingSpeed = 0.002;

/** 弹簧：stiffness = 200 + speed × 7（speed 25 → 375），质量 1。 */
export const springStiffness = 200 + (responsiveness / 100) * 700;

/** 弹簧停稳的判据（原版 motion 的 restDelta 0.01 / restSpeed 0.1 在 0 ～ 100 量程上，换成比例）。 */
export const settleDistance = 1.0e-4;
export const settleSpeed = 1.0e-3;

// ---- DialMotion.cpp 文件内常数 ----
/** 原版按「度」判 0.01：短于这个角长的彗尾段不画。 */
const minimumSegmentAngle = (0.01 * Math.PI) / 180;
/** 弹簧推进的最大步长：显式积分步子小，回弹的形状才与解析解一致（60 帧一帧拆成 10 步）。 */
const maxSpringStep = 1 / 600;

const twoPi = 2 * Math.PI;
const clamp = (v: number, lo: number, hi: number) => Math.min(hi, Math.max(lo, v));

/** 阻尼 = 临界阻尼 ×（1 − bounce），bounce 最多 0.9（DialMotion.cpp springDamping）。 */
export function springDamping(bounce: number): number {
  return 2 * Math.sqrt(springStiffness) * (1 - clamp(bounce, 0, 0.9));
}

/** 比例 → 角度（先钳到 0 ～ 1）。 */
export function angleFor(proportion: number): number {
  return startAngle + clamp(proportion, 0, 1) * sweep;
}

/** 外圈按下后移动多少像素才算拖：按原版的角度（R 80 上 10 px）换算到这颗旋钮的轨道半径，最少 minimumDragPixels。 */
export function dragThreshold(trackRadius: number): number {
  return Math.max(minimumDragPixels, (dragThresholdPixels * trackRadius) / originalRadius);
}

/**
 * to − from 折到 (−π, π]。C++ 用 std::remainder（结果在 [−π, π]，正好半圈时 n 取偶数）再把 −π 挪成 π；
 * 这里 n 用 Math.round（半圈时进位）——两种取法只在正好半圈时不同，而那时结果都是 ±π，挪完同为 π。
 */
export function wrappedAngleDelta(from: number, to: number): number {
  const x = to - from;
  let delta = x - Math.round(x / twoPi) * twoPi;
  if (delta <= -Math.PI) delta += twoPi;
  return delta;
}

/** 拖进底部缺口时锁哪一端：none = 还没落过（按下就在缺口里：取近的一端，并锁在这一端）。 */
export type Side = 'none' | 'low' | 'high';

/**
 * 相对圆心的偏移（屏幕坐标，y 向下）→ 比例 0 ～ 1（原版 angleAt）。返回比例和更新后的 side：
 * 在弧上时记下落在前半还是后半，之后拖进缺口就锁在那一端；还没记过（按下就在缺口里）时取缺口里近的一端并记下，
 * 之后在缺口里拖过正下方也不翻到另一端。
 */
export function proportionAtOffset(dx: number, dy: number, side: Side): { proportion: number; side: Side } {
  // atan2 (dx, −dy)：0 = 12 点、顺时针为正。rel = 从轨道起点顺时针量过去的角度，0 ～ 2π。
  const angle = Math.atan2(dx, -dy);
  const rel = (angle - startAngle + 2 * twoPi) % twoPi;
  const gap = twoPi - sweep;

  if (rel > sweep) {
    if (side === 'high') return { proportion: 1, side };
    if (side === 'low') return { proportion: 0, side };
    const nearer: Side = rel < sweep + 0.5 * gap ? 'high' : 'low';
    return { proportion: nearer === 'high' ? 1 : 0, side: nearer };
  }

  return { proportion: rel / sweep, side: rel > 0.5 * sweep ? 'high' : 'low' };
}

/**
 * 读数：画出来的位置（DialMotion.h Reading）。jump = 直接落下；launch = 从当前位置按弹簧去 target（velocity：量程 / 秒）；
 * follow = 拖动时按 followSeconds 追手（同时记下速度，松手后接弹簧用）；advance = 弹簧推进 seconds 秒。
 */
export class Reading {
  position = 0;
  velocity = 0;
  target = 0;
  animating = false;
  private damping = 0;

  jump(proportion: number) {
    this.position = this.target = proportion;
    this.velocity = 0;
    this.animating = false;
  }

  /** 不给 initialVelocity 时保留读数自己的速度（C++ 的两参数重载）。 */
  launch(newTarget: number, bounce: number, initialVelocity: number = this.velocity) {
    this.target = newTarget;
    this.velocity = Number.isFinite(initialVelocity) ? initialVelocity : 0;
    this.damping = springDamping(bounce);
    this.animating = true;
  }

  follow(newTarget: number, seconds: number) {
    this.animating = false;
    this.target = newTarget;

    if (!(seconds > 0)) return;

    const before = this.position;
    this.position += (newTarget - this.position) * (1 - Math.exp(-seconds / followSeconds));

    // 追到这么近就落在目标上：按住不动时位置不再每帧挪一点点（不然每帧都要重画）。
    if (Math.abs(newTarget - this.position) < settleDistance) this.position = newTarget;

    this.velocity = (this.position - before) / seconds;
  }

  advance(seconds: number) {
    if (!this.animating || !(seconds > 0)) return;

    // 以目标为原点：x'' = −k x − c x'（质量 1），半隐式欧拉。
    const steps = Math.max(1, Math.ceil(seconds / maxSpringStep));
    const h = seconds / steps;
    let x = this.position - this.target;
    let v = this.velocity;

    for (let i = 0; i < steps; i++) {
      v += (-springStiffness * x - this.damping * v) * h;
      x += v * h;
    }

    // 停稳时精确落在目标上：静止画面与没有弹簧时逐像素相同。
    if (Math.abs(x) < settleDistance && Math.abs(v) < settleSpeed) {
      this.position = this.target;
      this.velocity = 0;
      this.animating = false;
      return;
    }

    this.position = this.target + x;
    this.velocity = v;
  }
}

/**
 * 彗尾（DialMotion.h Comet）：读数的速度（量程 / 秒）按 speedSmoothSeconds 平滑；强度 = |速度| / fullSpeed（钳到 1）。
 * restart = 动画从静止重新开始（原版 wake 把上一帧位置清空：这一帧不算速度）。
 */
export class Comet {
  private previous: number | null = null;
  speed = 0;

  restart() {
    this.previous = null;
  }

  clear() {
    this.previous = null;
    this.speed = 0;
  }

  update(readingPosition: number, seconds: number) {
    const raw = this.previous !== null && seconds > 0 ? (readingPosition - this.previous) / seconds : 0;
    this.previous = readingPosition;
    this.speed += (raw - this.speed) * (1 - Math.exp(-Math.max(0, seconds) / speedSmoothSeconds));
  }

  get strength() {
    return clamp(Math.abs(this.speed) / fullSpeed, 0, 1);
  }

  get direction(): 1 | -1 {
    return this.speed < 0 ? -1 : 1;
  }

  get resting() {
    return Math.abs(this.speed) <= restingSpeed;
  }
}

/**
 * 彗尾的一段：从 fromAngle 到 toAngle（fromAngle < toAngle），weight = 1 − j / K（头部 1，越往后越小）。
 * 画的时候：线宽 = 轨道宽 + 额外宽 × weight × 强度，不透明度 = weight × 强度。
 */
export interface CometSegment {
  fromAngle: number;
  toAngle: number;
  weight: number;
}

/** 指示点在 headAngle、强度 strength、方向 direction（+1 = 往大走）：尾巴拖在身后，钳在轨道两端之间。 */
export function cometSegments(headAngle: number, strength: number, direction: number, reach: number = cometReach): CometSegment[] {
  const segment = (reach * clamp(strength, 0, 1)) / cometSegmentCount;
  const out: CometSegment[] = [];

  if (segment < minimumSegmentAngle) return out;

  for (let j = 0; j < cometSegmentCount; j++) {
    // 尾巴拖在运动方向的反面：往大走（顺时针）时在指示点之前的角度上。
    let from = direction > 0 ? headAngle - (j + 1) * segment : headAngle + j * segment;
    let to = direction > 0 ? headAngle - j * segment : headAngle + (j + 1) * segment;
    from = clamp(from, startAngle, endAngle);
    to = clamp(to, startAngle, endAngle);

    if (to - from < minimumSegmentAngle) continue;

    out.push({ fromAngle: from, toAngle: to, weight: 1 - j / cometSegmentCount });
  }

  return out;
}

/** 甩动（DialMotion.h FlickTracker）：拖动时记最近 4 个（时间毫秒，位置）；松手时算速度（量程 / 秒）。 */
export class FlickTracker {
  private samples: { timeMs: number; proportion: number }[] = [];

  reset() {
    this.samples.length = 0;
  }

  add(timeMs: number, proportion: number) {
    if (this.samples.length === 4) this.samples.shift();
    this.samples.push({ timeMs, proportion });
  }

  /** 最后一次移动距 releaseTimeMs 超过 staleReleaseMs，或不到两个点：0（停住再松手不甩）。 */
  releaseVelocity(releaseTimeMs: number): number {
    if (this.samples.length < 2) return 0;

    const first = this.samples[0];
    const last = this.samples[this.samples.length - 1];

    if (releaseTimeMs - last.timeMs > staleReleaseMs) return 0;

    return ((last.proportion - first.proportion) / Math.max(1, last.timeMs - first.timeMs)) * 1000;
  }
}

/** 惯性落点：at + 速度 × (DECEL / (1 − DECEL)) / 1000 × momentum（原版公式；没钳到 0 ～ 1）。 */
export function momentumTarget(at: number, velocity: number): number {
  return at + (velocity / 1000) * (deceleration / (1 - deceleration)) * momentum;
}

/** 松手后的回弹：点按 0.2 → 满速（flickSpeed）甩 0.1 线性插值。 */
export function releaseBounce(velocity: number): number {
  return tapBounce + (flickBounce - tapBounce) * clamp(Math.abs(velocity) / flickSpeed, 0, 1);
}
