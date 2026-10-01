import { Fragment, useRef } from 'react';
import { gsap, ScrollTrigger, useGSAP } from '@/lib/gsap';
import { prefersReducedMotion } from '@/lib/utils';

const modules = [
  { en: 'Balance', zh: '平衡' },
  { en: 'Clay', zh: '修坯' },
  { en: 'Color', zh: '染色' },
  { en: 'Limit', zh: '限幅' },
  { en: 'Glaze', zh: '施釉', glaze: true }
];

/** 轨道上并排几份同样的一行。一份约 1900 px（桌面）/ 1000 px（手机），4 份在任何屏宽下都铺得满，绕回时不露空。 */
const COPIES = 4;
/** 不滚动时走完一份要几秒。按份计速：手机上字小，像素速度跟着慢，读起来一样快。 */
const CYCLE = 36;
/** 页面每滚 200 px/s 加一倍速，最多 8 倍（再快字就糊了）。 */
const BOOST_PER = 200;
const BOOST_MAX = 8;
/** 速度跟随的时间常数（秒）：加速与换向快、回落慢——起得快、收得慢（停手后约 1 s 回到常速）。 */
const ATTACK = 0.15;
const RELEASE = 0.4;
/** 停手多久（秒）后回到平时的方向（向左）：只有正在往上滚时才倒着走，停下来不会一直倒着走。 */
const SETTLE = 0.4;

// 一份：英文模块名用 Fraunces 大字，中文小字，青釉圆点隔开（每份以圆点收尾，份与份之间的间距和份内一样）。
const line = (
  <span className="display text-[clamp(2.4rem,6vw,5.5rem)] leading-none tracking-[-0.01em] text-c800/90">
    {modules.map(m => (
      <Fragment key={m.en}>
        <span className={m.glaze ? 'italic text-c500' : undefined}>{m.en}</span>{' '}
        <span className="mx-3 align-middle font-serif text-[0.5em] text-ink-4">{m.zh}</span>
        <span className="mx-4 text-c300">·</span>
      </Fragment>
    ))}
  </span>
);

/**
 * 模块名跑马灯：平时向左慢走；页面滚得越快走得越快，向上滚就倒着走。
 * 只在进入视口时往 GSAP 的 ticker 上挂每帧回调，离开就摘掉；减少动效时静止。
 */
export default function Marquee() {
  const root = useRef<HTMLDivElement>(null);
  const track = useRef<HTMLDivElement>(null);

  useGSAP(
    () => {
      const el = track.current;
      if (!el || prefersReducedMotion()) return;
      // 位移用 xPercent：轨道宽度正好是 COPIES 份，走 100 / COPIES % 就是一整份，不用量宽度，改窗口大小也不错位。
      const setX = gsap.quickSetter(el, 'xPercent') as (value: number) => void;
      const wrap = gsap.utils.wrap(0, 1);
      let pos = 0; // 当前走过的份数（0～1）
      let speed = 1; // 当前速度：每 CYCLE 秒走几份；正 = 向左
      let dir = 1; // 当前方向：1 = 向左（平时与向下滚）、-1 = 向右（正在向上滚）
      let idle = 0; // 距最近一次明显滚动过了几秒
      let lastY = 0;
      let running = false;

      const tick = (_time: number, deltaMs: number) => {
        // 切回标签页后的第一帧可能隔了很久，封顶 0.1 s，免得一下蹿出去。
        const dt = Math.min(deltaMs, 100) / 1000;
        if (dt <= 0) return;
        const y = window.scrollY;
        const scrollV = (y - lastY) / dt;
        lastY = y;
        if (scrollV > 20) {
          dir = 1;
          idle = 0;
        } else if (scrollV < -20) {
          dir = -1;
          idle = 0;
        } else if ((idle += dt) > SETTLE) {
          dir = 1;
        }
        const target = dir * Math.min(1 + Math.abs(scrollV) / BOOST_PER, BOOST_MAX);
        const rising = Math.sign(target) !== Math.sign(speed) || Math.abs(target) > Math.abs(speed);
        speed += (target - speed) * (1 - Math.exp(-dt / (rising ? ATTACK : RELEASE)));
        pos = wrap(pos + (speed * dt) / CYCLE);
        setX((-pos * 100) / COPIES);
      };
      const start = () => {
        if (running) return;
        running = true;
        lastY = window.scrollY;
        gsap.ticker.add(tick);
      };
      const stop = () => {
        if (!running) return;
        running = false;
        gsap.ticker.remove(tick);
      };

      ScrollTrigger.create({
        trigger: root.current,
        start: 'top bottom',
        end: 'bottom top',
        onToggle: self => (self.isActive ? start() : stop())
      });
      return stop;
    },
    { scope: root }
  );

  return (
    <div ref={root} className="relative select-none py-10 mask-fade-x sm:py-14" aria-hidden="true">
      <div className="hairline mb-8" />
      <div className="overflow-hidden">
        <div ref={track} className="flex w-max whitespace-nowrap py-2 will-change-transform">
          {Array.from({ length: COPIES }, (_, i) => (
            <span key={i} className="shrink-0">
              {line}
            </span>
          ))}
        </div>
      </div>
      <div className="hairline mt-8" />
    </div>
  );
}
