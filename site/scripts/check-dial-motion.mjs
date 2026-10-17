// 官网复刻旋钮的手感计算与插件一致性检查（不开浏览器，随 `pnpm run build` 跑；单独跑 `pnpm run check:dial`）：
// 把插件 tests/DialMotionTests.cpp（T-100）的每组输入与期望值原样搬到网站的移植 src/components/celadon/dialMotion.ts 上，
// 同样的容差——插件改了 Source/ui/DialMotion.h 的手感常数，这里会先红，提醒把 dialMotion.ts 同步过去（T-101）。
// 用 vite 的 ssrLoadModule 直接读 .ts 源码（不依赖 Node 的类型剥离）；失败退出码 1。
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { createServer } from 'vite';

const SITE = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const failures = [];
let checks = 0;
let group = '';
const begin = name => {
  group = name;
};
const expect = (ok, message = '') => {
  checks++;
  if (!ok) failures.push(`${group}${message ? '：' + message : ''}`);
};
const expectEquals = (actual, expected, message = '') => expect(actual === expected, `${message}（期望 ${expected}，实际 ${actual}）`);
const expectNear = (actual, expected, tolerance, message = '') =>
  expect(Math.abs(actual - expected) <= tolerance, `${message}（期望 ${expected} ± ${tolerance}，实际 ${actual}）`);

const server = await createServer({
  root: SITE,
  configFile: false,
  appType: 'custom',
  logLevel: 'silent',
  server: { middlewareMode: true, hmr: false, ws: false, watch: null },
  optimizeDeps: { noDiscovery: true, include: [] }
});
let dial;
try {
  dial = await server.ssrLoadModule('/src/components/celadon/dialMotion.ts');
} finally {
  await server.close();
}

const pi = Math.PI;

// 角度 angle（0 = 12 点、顺时针）方向上、离圆心 radius 的偏移（屏幕坐标，y 向下）。
const offsetX = (angle, radius = 50) => radius * Math.sin(angle);
const offsetY = (angle, radius = 50) => -radius * Math.cos(angle);

// 插件用引用参数改 side；网站版返回新的 side。这里包一层，读法与插件测试一致。
const at = (angle, state) => {
  const r = dial.proportionAtOffset(offsetX(angle), offsetY(angle), state.side);
  state.side = r.side;
  return r.proportion;
};

// 按 60 帧推进弹簧直到停稳（最多 seconds 秒）；返回推进过程中位置的最大值。
const runSpring = (reading, seconds, frame = 1 / 60) => {
  let peak = reading.position;
  for (let t = 0; t < seconds && reading.animating; t += frame) {
    reading.advance(frame);
    peak = Math.max(peak, reading.position);
  }
  return peak;
};

begin("angle -> proportion on the 270 degree track (12 o'clock is the middle)");
{
  let s = { side: 'none' };
  expectNear(at(0, s), 0.5, 1e-5);
  expect(s.side !== 'none', 'a press on the track picks a side');

  s = { side: 'none' };
  expectNear(at(dial.startAngle + 1e-4, s), 0, 1e-4);
  s = { side: 'none' };
  expectNear(at(dial.endAngle - 1e-4, s), 1, 1e-4);
  expect(s.side === 'high');

  s = { side: 'none' };
  expectNear(at(0.5 * pi, s), 5 / 6, 1e-5, "3 o'clock = 225 of 270 degrees");

  for (const p of [0.05, 0.25, 0.5, 0.77, 0.99]) {
    s = { side: 'none' };
    expectNear(at(dial.angleFor(p), s), p, 1e-5, `round trip ${p}`);
  }

  expectEquals(dial.angleFor(-0.3), dial.startAngle, 'angleFor clamps below 0');
  expectEquals(dial.angleFor(1.7), dial.endAngle, 'angleFor clamps above 1');
}

begin('the bottom gap: nearest end when pressed there, locked to the side you came from while dragging');
{
  let s = { side: 'none' };
  expectEquals(at(pi + 0.1, s), 0, 'just left of the bottom -> start');
  expect(s.side === 'low', 'a press in the gap locks the nearer end');
  expectEquals(at(pi - 0.1, s), 0, 'then dragging past the bottom within the gap stays at the start');

  s = { side: 'none' };
  expectEquals(at(pi - 0.1, s), 1, 'just right of the bottom -> end');
  expect(s.side === 'high');
  expectEquals(at(pi + 0.1, s), 1, 'and stays at the end across the bottom');

  expectNear(at(dial.angleFor(0.1), s), 0.1, 1e-5);
  expect(s.side === 'low');

  s = { side: 'none' };
  at(dial.angleFor(0.9), s);
  expect(s.side === 'high');
  expectEquals(at(pi + 0.15, s), 1, 'dragged past the end stays at the maximum');

  s = { side: 'none' };
  at(dial.angleFor(0.1), s);
  expect(s.side === 'low');
  expectEquals(at(pi - 0.15, s), 0, 'dragged past the start stays at the minimum');
}

begin("drag threshold keeps the original's angle (10 px at radius 80), at least 4 px");
{
  expectNear(dial.dragThreshold(dial.originalRadius), 10, 1e-5, 'the original dial: 10 px');
  expectNear(dial.dragThreshold(102), 12.75, 1e-4, 'GLAZE ring (radius 102)');
  expectEquals(dial.dragThreshold(23), dial.minimumDragPixels, 'small knob (radius 23): 2.9 px -> floored at 4 px');
  expect(dial.dragThreshold(23) / 23 < 0.2, 'the small knob no longer waits a quarter turn');
}

begin('wrapped angle delta takes the short way round');
{
  expectNear(dial.wrappedAngleDelta(3, -3), 2 * pi - 6, 1e-5);
  expectNear(dial.wrappedAngleDelta(-3, 3), 6 - 2 * pi, 1e-5);
  expectNear(dial.wrappedAngleDelta(0.2, 0.5), 0.3, 1e-6);
  expect(dial.wrappedAngleDelta(0, pi) > 0 && dial.wrappedAngleDelta(0, -pi) > 0, 'half a turn is +pi');
}

begin('spring: stiffness 375, damping = critical x (1 - bounce), bounce capped at 0.9');
{
  expectNear(dial.springStiffness, 375, 1e-9);
  expectNear(dial.springDamping(0), 2 * Math.sqrt(375), 1e-9);
  expectNear(dial.springDamping(0.2), 0.8 * 2 * Math.sqrt(375), 1e-6);
  expectNear(dial.springDamping(0.95), 0.1 * 2 * Math.sqrt(375), 1e-6);
}

begin('tap spring rings past the target a little, then lands exactly on it');
{
  const reading = new dial.Reading();
  reading.jump(0.2);
  reading.launch(0.8, dial.tapBounce, 0);
  expect(reading.animating);
  const peak = runSpring(reading, 3);
  expect(!reading.animating, 'settles within 3 s');
  expectEquals(reading.position, 0.8, 'lands exactly on the target');
  expectEquals(reading.velocity, 0);
  // 阻尼比 0.8：过冲 exp (−ζπ / √(1 − ζ²)) ≈ 1.5 %（0.6 的距离 ≈ 0.009）。
  const overshoot = (peak - 0.8) / 0.6;
  expect(overshoot > 0.01 && overshoot < 0.02, `overshoot ${overshoot.toFixed(4)}`);
}

begin('no bounce: no overshoot; settle time is about half a second');
{
  const reading = new dial.Reading();
  reading.jump(0);
  reading.launch(1, 0, 0);
  const peak = runSpring(reading, 3);
  expect(peak <= 1 + 1e-6, `critically damped does not overshoot (${peak})`);

  const timed = new dial.Reading();
  timed.jump(0);
  timed.launch(1, dial.tapBounce, 0);
  let elapsed = 0;
  while (timed.animating && elapsed < 3) {
    timed.advance(1 / 60);
    elapsed += 1 / 60;
  }
  expect(elapsed > 0.3 && elapsed < 1, `full-range tap settles in ${elapsed.toFixed(3)} s`);
}

begin('spring is frame-rate independent');
{
  const at60 = new dial.Reading();
  const at120 = new dial.Reading();
  at60.jump(0);
  at120.jump(0);
  at60.launch(1, dial.tapBounce, 0);
  at120.launch(1, dial.tapBounce, 0);
  for (let i = 0; i < 12; i++) at60.advance(1 / 60);
  for (let i = 0; i < 24; i++) at120.advance(1 / 120);
  expectNear(at60.position, at120.position, 1e-3);
}

begin("launch keeps the reading's own velocity unless one is given");
{
  const reading = new dial.Reading();
  reading.jump(0);
  reading.follow(1, 0.06);
  expectNear(reading.position, 1 - Math.exp(-1), 1e-5, 'one time constant = 63 %');
  expectNear(reading.velocity, (1 - Math.exp(-1)) / 0.06, 1e-3);
  expect(!reading.animating, 'following is not the spring');

  const velocity = reading.velocity;
  reading.launch(0.5, dial.tapBounce);
  expectEquals(reading.velocity, velocity);
  reading.launch(0.5, dial.tapBounce, -2);
  expectEquals(reading.velocity, -2);
  reading.launch(0.5, dial.tapBounce, Number.NaN);
  expectEquals(reading.velocity, 0, 'a non-finite velocity starts from rest');
  reading.follow(0.5, 0);
  expect(!reading.animating, 'follow stops the spring even with no time passing');
}

begin('comet strength: smoothed speed / 4 ranges per second');
{
  const comet = new dial.Comet();
  expectEquals(comet.strength, 0);
  expect(comet.resting);

  let position = 0;
  comet.update(position, 1 / 60);
  expectEquals(comet.strength, 0, 'the first frame after a restart has no speed');
  for (let i = 0; i < 30; i++) {
    position += 2 / 60;
    comet.update(position, 1 / 60);
  }
  expectNear(comet.strength, 0.5, 0.01);
  expectEquals(comet.direction, 1);

  for (let i = 0; i < 30; i++) {
    position -= 10 / 60;
    comet.update(position, 1 / 60);
  }
  expectEquals(comet.strength, 1);
  expectEquals(comet.direction, -1);

  for (let i = 0; i < 30; i++) comet.update(position, 1 / 60);
  expect(comet.resting, `speed after stopping ${comet.speed}`);

  comet.clear();
  expectEquals(comet.speed, 0);
  comet.update(position + 0.5, 1 / 60);
  expectEquals(comet.speed, 0, 'after clear the next frame is a fresh start, not a jump');
}

begin('comet segments trail behind the head, fade and stay on the track');
{
  expectEquals(dial.cometSegments(dial.angleFor(0.5), 0, 1).length, 0, 'no strength, no tail');

  const head = dial.angleFor(0.8);
  const up = dial.cometSegments(head, 1, 1);
  expectEquals(up.length, dial.cometSegmentCount);
  expectNear(up[0].toAngle, head, 1e-6, 'moving up: the first segment ends at the head');
  expectNear(up[up.length - 1].fromAngle, head - dial.cometReach, 1e-5, 'full strength = 180 degrees');
  up.forEach((s, j) => {
    expect(s.fromAngle < s.toAngle, `segment ${j} ordered`);
    expectNear(s.weight, 1 - j / 12, 1e-6, `segment ${j} weight`);
  });

  const low = dial.angleFor(0.3);
  const down = dial.cometSegments(low, 0.5, -1);
  expectEquals(down.length, dial.cometSegmentCount);
  expectNear(down[0].fromAngle, low, 1e-6, 'moving down: the tail starts at the head');
  expectNear(down[down.length - 1].toAngle, low + 0.5 * dial.cometReach, 1e-5);

  const nearEnd = dial.cometSegments(head, 0.5, -1);
  expectEquals(nearEnd.length, 8);
  expectEquals(nearEnd[nearEnd.length - 1].toAngle, dial.endAngle, 'clamped at the end of the track');

  const clipped = dial.cometSegments(dial.angleFor(0.05), 1, 1);
  expect(clipped.length > 0 && clipped.length < dial.cometSegmentCount, `clipped tail (${clipped.length} segments)`);
  clipped.forEach((s, j) => expect(s.fromAngle >= dial.startAngle, `clipped segment ${j} stays on the track`));
}

begin('flick velocity from the last four moves; a pause before release is not a flick');
{
  const flick = new dial.FlickTracker();
  expectEquals(flick.releaseVelocity(0), 0, 'nothing recorded');
  flick.add(0, 0.5);
  expectEquals(flick.releaseVelocity(1), 0, 'one sample is not enough');

  flick.reset();
  for (let i = 0; i < 6; i++) flick.add(10 * i, 0.1 + 0.02 * i); // 2 量程 / 秒
  expectNear(flick.releaseVelocity(55), 2, 1e-4);
  expectNear(flick.releaseVelocity(50 + 80), 2, 1e-4, '80 ms is still a flick');
  expectEquals(flick.releaseVelocity(50 + 80.5), 0, 'more than 80 ms after the last move: no flick');

  flick.reset();
  flick.add(100, 0.3);
  flick.add(100, 0.31);
  expectNear(flick.releaseVelocity(100), 10, 1e-3, 'moves within the same millisecond count as 1 ms apart');
}

begin('momentum target and release bounce follow the original formulas');
{
  expectNear(dial.momentumTarget(0.5, 2), 0.5 + 2 * 0.099, 1e-5);
  expectNear(dial.momentumTarget(0.5, -1), 0.5 - 0.099, 1e-5);
  expectEquals(dial.momentumTarget(0.3, 0), 0.3);

  expectNear(dial.releaseBounce(0), 0.2, 1e-6);
  expectNear(dial.releaseBounce(1), 0.15, 1e-6);
  expectNear(dial.releaseBounce(-2), 0.1, 1e-6);
  expectNear(dial.releaseBounce(9), 0.1, 1e-6);
}

if (failures.length) {
  console.error(`check-dial-motion：${failures.length} 项失败（共 ${checks} 项）`);
  for (const f of failures) console.error('  - ' + f);
  process.exit(1);
}
console.log(`check-dial-motion：通过（${checks} 项：复现插件 tests/DialMotionTests.cpp 的全部用例）`);
