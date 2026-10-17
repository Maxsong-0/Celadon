import { useEffect, useLayoutEffect, useMemo, useRef, useState, type PointerEvent as ReactPointerEvent, type RefObject } from 'react';
import { gsap } from '@/lib/gsap';
import * as dial from './dialMotion';

/**
 * 网站复刻旋钮的控件行为（T-101）：逐条对应插件 Source/ui/Dial.cpp 的 DialControl（T-100，ADR-037）。
 * 计算在 dialMotion.ts（↔ DialMotion），画法在 GlazeKnob.tsx（↔ GlazeKnob.cpp）。
 *
 * 分区（按下点离圆心的距离）：
 *   - 外圈（deadRadius ≤ d ≤ grabRadius）：按下即跳到所按角度——按住 ⌘ / Ctrl 按下不跳，改相对转动；移动超过约 7°
 *     （dialMotion.dragThreshold，按轨道在屏幕上的半径换算）起算拖，值跟着角度走（拖进底部缺口锁在来的那一端），
 *     读数按 60 ms 追手；松手前 80 ms 内还在动就按速度多滑一段，再用弹簧停稳。拖动中按下 ⌘ / Ctrl：从那一刻起改相对转动、
 *     慢 8 倍，到松手都保持相对。
 *   - 其余（中间、下方的读数和名字）：上下拖（左右也算），逐事件累加，⌘ / Ctrl 慢 8 倍、中途按下松开不跳。
 * 读数怎么动：拖动时追手；点外圈、父组件改值（首屏的「呼吸」）、双击回默认从当前位置起弹簧；滚轮和键盘直接落下、不出彗尾（拖动中不收）；
 * 「减少动效」时一律直接落下、没有彗尾、快甩不多滑（中途打开也当场收尾）。动画挂在 gsap.ticker 上，停稳（读数到位——拖动中就是追上了手——且
 * 彗尾速度落到阈值下）就摘掉。静止时读数精确等于参数值，画出来与没有动画时逐像素相同。
 *
 * 和插件不一样的地方只有宿主相关的：网站没有宿主手势，参数就是父组件的受控 value；
 * 网站多了键盘（同滚轮）；pointercancel（手势被系统打断）当松手但不甩。
 */

/** 旋钮的几何。坐标、半径用 SVG 用户坐标（viewBox）；fullDragPixels 用屏幕像素（CSS px）。 */
export interface DialGeometry {
  cx: number;
  cy: number;
  /** 轨道半径（外圈拖动门槛按它在屏幕上的大小换算） */
  trackRadius: number;
  /** 离圆心小于它算中间 */
  deadRadius: number;
  /** 大于它也算中间 */
  grabRadius: number;
  /** 中间上下拖：拖满量程的屏幕像素 */
  fullDragPixels: number;
}

/** 画一帧要的东西。 */
export interface DialFrame {
  /** 读数（比例；弹簧过冲时可能略出 0 ～ 1） */
  reading: number;
  /** 彗尾强度 0 ～ 1 与方向（+1 = 往大走） */
  strength: number;
  direction: 1 | -1;
  /** 读数停稳且精确等于参数值：此时文字、瓷面都用参数值本身 */
  settled: boolean;
  /** 动画在跑 */
  running: boolean;
}

/** 精调慢几倍（插件 ParameterControl::fineFactor 0.125）。 */
const fineFactor = 0.125;

type Grab = 'none' | 'ring' | 'centre';

interface PointerLike {
  clientX: number;
  clientY: number;
  metaKey: boolean;
  ctrlKey: boolean;
  timeStamp: number;
  pointerId: number;
  buttons: number;
}

const clamp01 = (v: number) => Math.min(1, Math.max(0, v));

/** 插件 DialControl 的网站版：不依赖 React，hook 只负责把它接到组件上。 */
class DialController {
  readonly reading = new dial.Reading();
  readonly comet = new dial.Comet();
  private readonly flick = new dial.FlickTracker();
  private readonly reduceQuery: MediaQueryList | null;

  /** 参数值（比例）：父组件的 value；自己设的值在 commit 时就记下（插件里参数是同步改的）。 */
  param: number;
  interactive = true;

  private running = false;
  private lastTickMs = 0;
  private jumpNextChange = false;

  private grab: Grab = 'none';
  private dragging = false;
  private pointerId: number | null = null;
  private dragProportion = 0; // 未经吸附的连续位置（插件 getDragProportion）
  private ringMoved = false;
  private ringRelative = false;
  private side: dial.Side = 'none';
  private down = { x: 0, y: 0 };
  private last = { x: 0, y: 0 };
  private lastPointerAngle = 0;
  private screenTrackRadius = 0;

  constructor(
    initial: number,
    private readonly geometry: DialGeometry,
    private readonly svg: RefObject<SVGSVGElement | null>,
    private readonly commitParam: (proportion: number) => number,
    private readonly publish: (frame: DialFrame) => void
  ) {
    this.param = initial;
    this.reading.jump(initial);
    this.reduceQuery = typeof window !== 'undefined' && window.matchMedia ? window.matchMedia('(prefers-reduced-motion: reduce)') : null;
  }

  get reduceMotion() {
    return this.reduceQuery?.matches ?? false;
  }

  /** 挂上后跟着系统的「减少动效」走（插件 setReduceMotion）：打开的那一刻收尾——读数落到参数值、彗尾清零。
   *  按住不动时动画已经自己停了，不在这里收尾的话，松手后读数停在未吸附的连续位置上，差参数值半格。返回解绑（也停掉动画）。 */
  attach() {
    const query = this.reduceQuery;
    const onChange = () => {
      if (query?.matches) this.finishMotion();
    };
    query?.addEventListener('change', onChange);
    return () => {
      query?.removeEventListener('change', onChange);
      this.stopMotion();
    };
  }

  // ---- 参数 ----

  /** 父组件给的值变了（首屏呼吸、或父组件没接受这次改值）：当成一次外部改值。 */
  syncFromProps(proportion: number) {
    if (proportion === this.param) return;
    this.param = proportion;
    this.parameterValueChanged();
  }

  /** 插件 dragTo：记下连续位置，参数按父组件的吸附设下去；真的变了才算一次改值。 */
  private dragTo(proportion: number) {
    this.dragProportion = clamp01(proportion);
    this.setParam(this.dragProportion);
  }

  private setParam(proportion: number) {
    const actual = this.commitParam(clamp01(proportion));
    if (actual === this.param) return;
    this.param = actual;
    this.parameterValueChanged();
  }

  /** 滚轮、键盘：直接落到新值（插件 mouseWheelMove 的 jumpNextChange）。拖动中不收（插件按住鼠标键时不收滚轮）：
   *  不然参数跳了、读数还追着手，下一次移动又把值拖回去。 */
  jumpTo(proportion: number) {
    if (this.dragging) return;
    this.jumpNextChange = true;
    try {
      this.setParam(proportion);
    } finally {
      this.jumpNextChange = false;
    }
  }

  /** 双击回默认：一次「外部」改值，读数起弹簧（插件 mouseDoubleClick）。 */
  resetTo(proportion: number) {
    this.grab = 'none';
    this.ringMoved = false;
    this.dragProportion = clamp01(proportion);
    this.setParam(proportion);
  }

  // ---- 读数与动画（插件 parameterValueChanged / startMotion / tick / advanceMotion / finishMotion）----

  private isFollowing() {
    return this.grab === 'centre' || (this.grab === 'ring' && this.ringMoved);
  }

  private isReadingSettled() {
    return !this.reading.animating && this.reading.position === this.param;
  }

  private parameterValueChanged() {
    const target = this.param;

    if (this.reduceMotion) {
      this.reading.jump(target);
      this.comet.clear();
      this.emit();
      return;
    }

    // 自己拖的时候读数追手（tick 里），不另起弹簧。
    if (!this.isFollowing()) {
      if (this.jumpNextChange) {
        // 滚轮一下跳过去：动画正在跑时也别把这一跳算成速度（不然下一帧彗尾猛地一闪）。
        this.reading.jump(target);
        this.comet.restart();
      } else this.reading.launch(target, dial.tapBounce);
    }

    this.startMotion();
    this.emit();
  }

  private startMotion() {
    if (this.reduceMotion || this.running) return;

    // 从静止重新开始：这一帧不算速度（原版 wake 把上一帧位置清空），滚轮一下跳过去不会闪出彗尾。
    this.lastTickMs = performance.now();
    this.comet.restart();
    gsap.ticker.add(this.tick);
    this.running = true;
  }

  private stopMotion() {
    if (!this.running) return;
    gsap.ticker.remove(this.tick);
    this.running = false;
  }

  private readonly tick = () => {
    const now = performance.now();
    const seconds = Math.min(dial.maxFrameSeconds, Math.max(0, (now - this.lastTickMs) / 1000));
    this.lastTickMs = now;
    this.advanceMotion(seconds);
  };

  private advanceMotion(seconds: number) {
    if (this.reduceMotion) {
      this.finishMotion();
      return;
    }

    const positionBefore = this.reading.position;
    const strengthBefore = this.comet.strength;

    if (this.isFollowing()) this.reading.follow(this.dragProportion, seconds);
    else this.reading.advance(seconds);

    this.comet.update(this.reading.position, seconds);

    // 停稳 = 读数到位（拖动中 = 追上了手；否则 = 弹簧停了）且彗尾的速度落到阈值下。按住不动也停：平滑后的速度永远到不了 0，
    // 不停的话每帧都差一点点、每帧重画；再动时 pointermove 会重新起动画。
    const readingStill = this.isFollowing() ? this.reading.position === this.reading.target : !this.reading.animating;
    const stopping = readingStill && this.comet.resting;

    if (stopping) {
      this.comet.clear();
      this.stopMotion();
    }

    if (stopping || this.reading.position !== positionBefore || this.comet.strength !== strengthBefore) this.emit();
  }

  /** 读数直接落到参数值、彗尾清零、停掉动画。 */
  finishMotion() {
    this.stopMotion();
    this.reading.jump(this.param);
    this.comet.clear();
    this.emit();
  }

  private settleFromRelease(bounce: number, velocity: number) {
    if (this.reduceMotion) return; // parameterValueChanged 已经让读数落下

    this.reading.launch(this.param, bounce, velocity);
    this.startMotion();
  }

  private emit() {
    this.publish({
      reading: this.reading.position,
      strength: this.comet.strength,
      direction: this.comet.direction,
      settled: this.isReadingSettled(),
      running: this.running
    });
  }

  // ---- 指针（插件 mouseDown / mouseDrag / mouseUp / mouseMove）----

  /** 屏幕坐标 → SVG 用户坐标，并顺带给出屏幕上每单位多少像素。取不到（元素没挂上）返回 null。 */
  private toLocal(e: PointerLike) {
    const ctm = this.svg.current?.getScreenCTM();
    if (!ctm) return null;
    const p = new DOMPoint(e.clientX, e.clientY).matrixTransform(ctm.inverse());
    return { x: p.x - this.geometry.cx, y: p.y - this.geometry.cy, scale: Math.hypot(ctm.a, ctm.b) };
  }

  private isInRing(local: { x: number; y: number }) {
    const d = Math.hypot(local.x, local.y);
    return d >= this.geometry.deadRadius && d <= this.geometry.grabRadius;
  }

  private ringProportion(local: { x: number; y: number }) {
    const result = dial.proportionAtOffset(local.x, local.y, this.side);
    this.side = result.side;
    return result.proportion;
  }

  /** 光标：外圈是手（拖着时握拳），其余交给组件自己的 class（上下箭头）。 */
  private setCursor(cursor: '' | 'grab' | 'grabbing') {
    const el = this.svg.current;
    if (el && el.style.cursor !== cursor) el.style.cursor = cursor;
  }

  pointerDown(e: ReactPointerEvent<SVGSVGElement>) {
    if (!this.interactive || e.button !== 0) return;
    if (this.dragging) {
      // 别的手指 / 笔：不收。同一个指针又按下了：上一次的松手没收到（没拿到捕获、在元素外松开），当那次已松手、不甩。
      if (e.pointerId !== this.pointerId) return;
      this.pointerUp(e, true);
    }

    try {
      e.currentTarget.setPointerCapture(e.pointerId);
    } catch {
      /* 拿不到捕获也照样拖，只是移出元素后收不到移动 */
    }

    const fineAtStart = e.metaKey || e.ctrlKey;
    this.down = this.last = { x: e.clientX, y: e.clientY };
    this.ringMoved = false;
    this.dragging = true;
    this.pointerId = e.pointerId;
    this.dragProportion = this.param; // 插件 beginDrag

    const local = this.toLocal(e);

    // 取不到坐标换算时退回上下拖（改前的手感），不抛错。
    if (!local || !this.isInRing(local)) {
      this.grab = 'centre';
      return;
    }

    this.grab = 'ring';
    this.ringRelative = fineAtStart;
    this.side = 'none';
    this.flick.reset();
    this.lastPointerAngle = Math.atan2(local.x, -local.y);
    this.screenTrackRadius = this.geometry.trackRadius * local.scale;
    this.setCursor('grabbing');

    // 按住 ⌘ / Ctrl 按下：不跳，之后相对转动（精调要的是从当前值慢慢挪）。
    if (this.ringRelative) return;

    // 点外圈：参数直接到所按角度，读数在 parameterValueChanged 里从当前位置起弹簧。
    const at = this.ringProportion(local);
    this.flick.add(e.timeStamp, at);
    this.dragTo(at);
  }

  pointerMove(e: PointerLike) {
    if (!this.dragging || this.grab === 'none') {
      if (this.interactive && !this.dragging) {
        const local = this.toLocal(e);
        this.setCursor(local && this.isInRing(local) ? 'grab' : '');
      }
      return;
    }
    if (e.pointerId !== this.pointerId) return;

    // 键已经松了还在拖：松手没收到（同上），当已松手，不然光标划过就改值。
    if (e.buttons === 0) {
      this.pointerUp(e, true);
      return;
    }

    const fineNow = e.metaKey || e.ctrlKey;

    if (this.grab === 'centre') {
      // 逐事件累加增量（不按相对按下点的总位移）：拖到一半按下 / 松开 ⌘ 不会跳。上下拖为主，左右也算（右 / 上为增）。
      const delta = e.clientX - this.last.x + (this.last.y - e.clientY);
      this.last = { x: e.clientX, y: e.clientY };
      if (delta === 0) return;

      const pixels = this.geometry.fullDragPixels / (fineNow ? fineFactor : 1);
      this.dragTo(this.dragProportion + delta / pixels);
      this.startMotion();
      return;
    }

    // 外圈：移动超过门槛才算拖（之前是点按，读数还在弹簧上）。
    if (!this.ringMoved) {
      if (Math.hypot(e.clientX - this.down.x, e.clientY - this.down.y) < dial.dragThreshold(this.screenTrackRadius)) return;
      this.ringMoved = true;
    }

    const local = this.toLocal(e);
    if (!local) return;

    const angle = Math.atan2(local.x, -local.y);
    const turned = dial.wrappedAngleDelta(this.lastPointerAngle, angle);
    this.lastPointerAngle = angle;

    if (fineNow) this.ringRelative = true;

    // 相对转动：按这次转过的角度挪（精调时慢 8 倍），不看绝对角度；否则值 = 指针所在的角度。
    const at = this.ringRelative
      ? clamp01(this.dragProportion + (turned / dial.sweep) * (fineNow ? fineFactor : 1))
      : this.ringProportion(local);

    this.flick.add(e.timeStamp, at);
    this.dragTo(at);
    this.startMotion();
  }

  /** cancelled：系统打断了手势（pointercancel / 丢了捕获）——当松手，但不甩。 */
  pointerUp(e: PointerLike, cancelled: boolean) {
    if (!this.dragging || e.pointerId !== this.pointerId) return;

    const released = this.grab;
    const moved = this.ringMoved;
    this.grab = 'none'; // 先放开：下面设落点时 parameterValueChanged 不再当成在追手
    this.ringMoved = false;

    if (released === 'ring' && moved) {
      // 松手前 80 ms 内还在动：按速度多滑一段。「减少动效」时不多滑：没有动画交代值为什么跑到了松手处以外。
      const velocity = this.reduceMotion || cancelled ? 0 : this.flick.releaseVelocity(e.timeStamp);
      const landing = clamp01(dial.momentumTarget(this.dragProportion, velocity));

      if (landing !== this.dragProportion) this.dragTo(landing);

      this.endDrag(e);
      this.settleFromRelease(dial.releaseBounce(velocity), velocity);
      return;
    }

    this.endDrag(e);

    // 中间上下拖：读数还落后 60 ms，从当前位置用点按的回弹停稳。外圈点按的弹簧在按下时已经起了。
    if (released === 'centre') this.settleFromRelease(dial.tapBounce, this.reading.velocity);
  }

  private endDrag(e: PointerLike) {
    this.dragging = false;
    this.pointerId = null;
    const local = this.toLocal(e);
    this.setCursor(local && this.isInRing(local) ? 'grab' : '');
  }
}

/**
 * 把 DialController 接到组件上。value：参数比例（受控）；commit：设参数（交给父组件），返回吸附后实际设下的比例。
 * commit 要让父组件在同一批更新里给回新的 value（普通 setState 即可）：放进 transition 之类延后的更新，
 * 中间那次渲染拿到的还是旧 value，会被当成父组件没接受、读数先弹回去再弹过来。
 * geometry 只在第一次渲染时读（网站传的是模块常量）。
 * 返回这一帧要画的东西、指针事件、滚轮 / 键盘 / 双击用的 jumpTo / resetTo，以及「减少动效」。
 */
export function useDialControl({
  value,
  commit,
  geometry,
  interactive,
  svgRef
}: {
  value: number;
  commit: (proportion: number) => number;
  geometry: DialGeometry;
  interactive: boolean;
  svgRef: RefObject<SVGSVGElement | null>;
}) {
  const commitRef = useRef(commit);
  const [frame, setFrame] = useState<DialFrame>(() => ({ reading: value, strength: 0, direction: 1, settled: true, running: false }));
  const controllerRef = useRef<DialController | null>(null);
  if (controllerRef.current === null) {
    controllerRef.current = new DialController(value, geometry, svgRef, p => commitRef.current(p), next =>
      setFrame(prev =>
        prev.reading === next.reading &&
        prev.strength === next.strength &&
        prev.direction === next.direction &&
        prev.settled === next.settled &&
        prev.running === next.running
          ? prev
          : next
      )
    );
  }
  const controller = controllerRef.current;

  useLayoutEffect(() => {
    commitRef.current = commit;
    controller.interactive = interactive;
  });

  // 每次渲染都和父组件给的值对一次（受控）：变了就在画之前起弹簧（不先闪一帧新值）；父组件没接受这次改值时读数弹回去。
  // 自己 commit 的值和父组件的更新在同一批里渲染，到这里已经相等，不会多起一次弹簧。
  useLayoutEffect(() => controller.syncFromProps(value));

  useEffect(() => controller.attach(), [controller]);

  // 这些函数跟着 controller 走、不随每帧重建：滚轮监听器挂一次就够，不在动画的每一帧重挂。
  const api = useMemo(
    () => ({
      /** 参数此刻的比例（滚轮、键盘按它加减） */
      param: () => controller.param,
      jumpTo: (p: number) => controller.jumpTo(p),
      resetTo: (p: number) => controller.resetTo(p),
      handlers: {
        onPointerDown: (e: ReactPointerEvent<SVGSVGElement>) => controller.pointerDown(e),
        onPointerMove: (e: ReactPointerEvent<SVGSVGElement>) => controller.pointerMove(e),
        onPointerUp: (e: ReactPointerEvent<SVGSVGElement>) => controller.pointerUp(e, false),
        onPointerCancel: (e: ReactPointerEvent<SVGSVGElement>) => controller.pointerUp(e, true),
        onLostPointerCapture: (e: ReactPointerEvent<SVGSVGElement>) => controller.pointerUp(e, true)
      }
    }),
    [controller]
  );

  return { frame, reduceMotion: controller.reduceMotion, ...api };
}
