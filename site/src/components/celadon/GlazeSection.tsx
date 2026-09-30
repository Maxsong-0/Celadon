import { useRef, useState } from 'react';
import { Link } from 'react-router-dom';
import GlazeKnob from './GlazeKnob';
import ParticleField from './ParticleField';
import { Em, SectionHead } from './Eyebrow';
import { gsap, useGSAP } from '@/lib/gsap';
import { clamp, cn, prefersReducedMotion } from '@/lib/utils';

const phases = ['未学习', '聆听中', '校准中', '已锁定'] as const;

// GLAZE 联动的 7 项参数（GlazeTypes.h AutoIncrements）。满刻度增量只是示意比例，真值由方案按歌决定。
const driven: { name: string; unit: string; manual: number; full: number; max: number }[] = [
  { name: 'Recover', unit: '', manual: 0, full: 24, max: 100 },
  { name: 'Tame', unit: '', manual: 0, full: 30, max: 100 },
  { name: 'Tone', unit: 'dB', manual: 0, full: 0.6, max: 3 },
  { name: 'Glue', unit: '', manual: 12, full: 22, max: 100 },
  { name: 'Heat', unit: '', manual: 0, full: 28, max: 100 },
  { name: 'Gain', unit: 'dB', manual: 0, full: 6.4, max: 24 },
  { name: 'Clip', unit: '', manual: 0, full: 18, max: 100 }
];

/** 互动段：拧 GLAZE，看开片生长、七项参数的紫色加量、以及「先听后决策」的状态走一遍。 */
export default function GlazeSection() {
  const root = useRef<HTMLElement>(null);
  const [value, setValue] = useState(35);
  const [phase, setPhase] = useState(0);
  const [seconds, setSeconds] = useState(0);
  const v01 = clamp(value / 100, 0, 1);

  useGSAP(
    () => {
      if (!root.current) return;
      // 进入视口后模拟一次「播放」：0 → 聆听 22 s → 校准 → 锁定，循环。
      const state = { t: 0 };
      const tl = gsap.timeline({
        repeat: -1,
        repeatDelay: 2.2,
        paused: true,
        onRepeat: () => {
          setPhase(0);
          setSeconds(0);
        }
      });
      tl.call(() => setPhase(0));
      tl.to({}, { duration: 0.9 });
      tl.call(() => setPhase(1));
      tl.to(state, {
        t: 22,
        duration: prefersReducedMotion() ? 0.01 : 4.4,
        ease: 'none',
        onUpdate: () => setSeconds(Math.floor(state.t))
      });
      tl.call(() => setPhase(2));
      tl.to({}, { duration: 2.4 });
      tl.call(() => setPhase(3));
      tl.to({}, { duration: 3.2 });

      gsap.timeline({
        scrollTrigger: {
          trigger: root.current,
          start: 'top 70%',
          end: 'bottom 30%',
          onEnter: () => tl.play(),
          onEnterBack: () => tl.play(),
          onLeave: () => tl.pause(),
          onLeaveBack: () => tl.pause()
        }
      });

      const q = gsap.utils.selector(root);
      gsap.from(q('[data-reveal]'), {
        y: 40,
        opacity: 0,
        duration: 1,
        stagger: 0.08,
        ease: 'power3.out',
        scrollTrigger: { trigger: root.current, start: 'top 75%', once: true }
      });
    },
    { scope: root }
  );

  const locked = phase === 3;
  const targetLufs = -14 + v01 * 6.2; // 示意：风格与 GLAZE 想要的响度
  const styleWeights = [0.28 + 0.1 * Math.sin(v01 * 3), 0.42 - 0.12 * v01, 0.3 + 0.02 * v01];
  const wSum = styleWeights.reduce((a, b) => a + b, 0);

  return (
    <section id="glaze" ref={root} className="relative py-28 sm:py-36">
      <div className="mx-auto max-w-[1280px] px-5 sm:px-8">
        <SectionHead
          chapter={{ n: '贰', label: '施釉', en: 'One knob' }}
          title={
            <>
              先听这首歌，
              <br className="hidden sm:block" />
              再决定<Em>推多远</Em>。
            </>
          }
          lead="GLAZE 不是把所有旋钮同时调大。播放后它先听 15 ～ 30 秒，量这首歌的响度、动态、频谱与起音，得出画像与方案；旋钮只决定朝「这首歌的理想母带」推多远。0 = 完全不动，你的手动设置永远是底。"
        />

        <div className="mt-16 grid gap-10 lg:grid-cols-12 lg:gap-8 items-start">
          <div data-reveal className="lg:col-span-6">
            <div className="bezel">
              <div className="bezel-core relative overflow-hidden px-4 py-6 sm:px-8">
                <div className="relative mx-auto max-w-[460px] aspect-[1/1.1]">
                  <ParticleField value={value} maxPerSide={110} className="absolute inset-0 h-full w-full" />
                  <GlazeKnob value={value} onChange={setValue} autoIncrement={locked ? value * 0.22 : 0} size={460} className="relative h-full w-full" />
                </div>
                <div className="mt-2 flex flex-wrap items-center justify-center gap-2">
                  {phases.map((p, i) => (
                    <span
                      key={p}
                      className={cn(
                        'rounded-full px-3 py-1 text-[12px] ring-1 transition-all duration-500 ease-glaze',
                        i === phase ? (i === 3 ? 'bg-c700 text-paper ring-c700' : 'bg-violet-soft text-violet ring-violet/30') : 'bg-white/50 text-ink-4 ring-ink/8'
                      )}
                    >
                      {p}
                      {i === 1 && phase === 1 ? <span className="font-mono"> {seconds} / 30 s</span> : null}
                    </span>
                  ))}
                </div>
                <p className="mt-5 text-center text-[13px] text-ink-3">上下拖动旋钮 · ⌘ / Ctrl 精调 · 滚轮微调 · 双击回 35</p>
              </div>
            </div>
          </div>

          <div className="lg:col-span-6 space-y-6">
            <div data-reveal className="bezel">
              <div className="bezel-core p-6 sm:p-8">
                <div className="flex items-baseline justify-between">
                  <h3 className="font-serif text-xl font-semibold text-ink">联动 7 项参数</h3>
                  <span className="text-[11px] uppercase tracking-[0.2em] text-ink-4">青 = 手动 · 紫 = 自动加量</span>
                </div>
                <p className="mt-2 text-[14px] text-ink-3">实际值 = clamp(手动值 + 自动增量)。自动增量随 GLAZE 单调、永不超过各模块上限，也绝不抵消你的修改。</p>
                <ul className="mt-6 space-y-3.5">
                  {driven.map(d => {
                    const inc = locked ? d.full * v01 : d.full * v01 * 0.35;
                    const m = d.manual / d.max;
                    const a = inc / d.max;
                    return (
                      <li key={d.name} className="grid grid-cols-[72px_1fr_84px] items-center gap-3 text-[13px]">
                        <span className="font-medium text-ink-2">{d.name}</span>
                        <div className="relative h-2 rounded-full bg-paper-3 overflow-hidden">
                          <div className="absolute inset-y-0 left-0 rounded-full bg-c500 transition-[width] duration-500 ease-glaze" style={{ width: `${m * 100}%` }} />
                          <div className="absolute inset-y-0 rounded-full bg-violet transition-all duration-500 ease-glaze" style={{ left: `${m * 100}%`, width: `${a * 100}%` }} />
                        </div>
                        <span className="text-right font-mono text-[12px] text-ink-3 tabular-nums">
                          {d.manual}
                          <span className="text-violet"> +{inc.toFixed(d.unit ? 1 : 0)}</span>
                          <span className="text-ink-4"> {d.unit}</span>
                        </span>
                      </li>
                    );
                  })}
                </ul>
              </div>
            </div>

            <div className="grid gap-6 sm:grid-cols-2">
              <div data-reveal className="bezel">
                <div className="bezel-core p-6">
                  <div className="text-[11px] uppercase tracking-[0.2em] text-ink-4">目标响度（示意）</div>
                  <div className="mt-3 display text-[40px] leading-none text-ink tabular-nums">
                    {targetLufs.toFixed(1)}
                    <span className="text-[16px] text-ink-3 ml-1">LUFS</span>
                  </div>
                  <p className="mt-3 text-[13px] leading-relaxed text-ink-3">目标由风格与 GLAZE 共同决定，再经各模块的上限收窄；到了动态保护的边界就不再加压，状态显示「受限」。</p>
                </div>
              </div>
              <div data-reveal className="bezel">
                <div className="bezel-core p-6">
                  <div className="text-[11px] uppercase tracking-[0.2em] text-ink-4">风格 · 自动按歌混合</div>
                  <ul className="mt-4 space-y-3">
                    {(['温润', '冲击', '通透'] as const).map((s, i) => (
                      <li key={s} className="grid grid-cols-[40px_1fr_40px] items-center gap-3 text-[13px]">
                        <span className="text-ink-2">{s}</span>
                        <div className="h-1.5 rounded-full bg-paper-3 overflow-hidden">
                          <div className="h-full rounded-full bg-c600 transition-[width] duration-700 ease-glaze" style={{ width: `${(styleWeights[i] / wSum) * 100}%` }} />
                        </div>
                        <span className="text-right font-mono text-[12px] text-ink-3 tabular-nums">{Math.round((styleWeights[i] / wSum) * 100)}%</span>
                      </li>
                    ))}
                  </ul>
                  <p className="mt-4 text-[13px] leading-relaxed text-ink-3">也可以手选：温润厚圆不刺、冲击鼓点有力更响、通透清楚少压缩。</p>
                </div>
              </div>
            </div>

            <p data-reveal className="text-[14px] text-ink-3">
              想知道画像量什么、方案怎么算、校准何时结束：<Link to="/algorithms/glaze" className="text-c700 underline decoration-c300 hover:decoration-c600">GLAZE 决策与校准</Link>。
            </p>
          </div>
        </div>
      </div>
    </section>
  );
}
