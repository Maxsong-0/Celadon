import { lazy, Suspense, useCallback, useEffect, useRef, useState } from 'react';
import { Link } from 'react-router-dom';
import SplitText from '@/components/react-bits/SplitText';
import Magnet from '@/components/react-bits/Magnet';
import GlazeKnob from './GlazeKnob';
import ParticleField from './ParticleField';
import { ArrowUpRight, LinkArrow } from './Eyebrow';
import { gsap, useGSAP } from '@/lib/gsap';
import { prefersReducedMotion } from '@/lib/utils';
import { PLUGIN_VERSION } from '@/lib/site';

const Topography = lazy(() => import('@/components/react-bits/Topography'));

/** 首屏：一件器物（青瓷旋钮）+ 一句话。等高线只在旋钮一侧、压得很淡。 */
export default function Hero() {
  const root = useRef<HTMLElement>(null);
  const [webgl, setWebgl] = useState(false);

  useEffect(() => {
    if (prefersReducedMotion()) return;
    try {
      const c = document.createElement('canvas');
      setWebgl(!!(c.getContext('webgl2') || c.getContext('webgl')));
    } catch {
      setWebgl(false);
    }
  }, []);

  useGSAP(
    () => {
      if (!root.current || prefersReducedMotion()) return;
      const q = gsap.utils.selector(root);
      gsap.from(q('[data-rise]'), { y: 24, autoAlpha: 0, duration: 1.1, stagger: 0.1, delay: 0.35, ease: 'power3.out' });
      gsap.from(q('[data-knob]'), { y: 16, scale: 0.96, autoAlpha: 0, duration: 1.4, delay: 0.15, ease: 'power3.out' });
    },
    { scope: root }
  );

  return (
    <section ref={root} className="relative min-h-[100dvh] overflow-hidden pt-28 sm:pt-32">
      {/* 釉面等高线：只在旋钮那一侧，径向淡出 */}
      <div
        className="absolute inset-0 -z-10 [mask-image:radial-gradient(52%_60%_at_74%_46%,black,transparent_78%)]"
        aria-hidden="true"
      >
        {webgl ? (
          <Suspense fallback={null}>
            <div className="absolute inset-0 opacity-45 mix-blend-multiply">
              <Topography
                lowColor="#E6EEE9"
                midColor="#A9C3B7"
                highColor="#5C8579"
                bands={2.2}
                thickness={0.009}
                glow={0.18}
                speed={0.18}
                morphAmount={2.2}
                morphSpeed={0.03}
                contrast={2}
                grain={false}
                mouseRadius={0.26}
                mouseStrength={0.3}
                opacity={0.8}
              />
            </div>
          </Suspense>
        ) : (
          <div className="absolute inset-0 bg-[radial-gradient(50%_50%_at_74%_46%,rgba(169,195,183,0.35),transparent_70%)]" />
        )}
      </div>

      <div className="mx-auto grid max-w-[1280px] grid-cols-1 items-center gap-10 px-5 sm:px-8 lg:grid-cols-12 lg:gap-6 pb-24">
        <div className="lg:col-span-7 relative z-10">
          <p data-rise className="flex items-center gap-4 text-[13px] tracking-[0.2em] text-ink-3">
            <span>青瓷 · 智能母带</span>
            <span className="h-px w-10 bg-ink/20" aria-hidden="true" />
            <span className="font-mono text-[12px] tracking-[0.08em] text-ink-4">v{PLUGIN_VERSION}</span>
          </p>
          <h1 className="mt-8 font-serif text-[clamp(2.8rem,7.4vw,6.6rem)] font-semibold leading-[1.04] tracking-[-0.02em] text-ink">
            <SplitText text="一挂总轨，" tag="span" className="block" splitType="chars" delay={45} duration={1.1} from={{ opacity: 0, y: 46 }} to={{ opacity: 1, y: 0 }} threshold={0} rootMargin="0px" textAlign="left" force3D={false} />
            <SplitText text="就好听。" tag="span" className="block glaze-text" splitType="chars" delay={45} duration={1.1} from={{ opacity: 0, y: 46 }} to={{ opacity: 1, y: 0 }} threshold={0} rootMargin="0px" textAlign="left" force3D={false} />
          </h1>
          <p data-rise className="display mt-6 text-[clamp(1.1rem,2vw,1.55rem)] italic text-ink-3" style={{ fontVariationSettings: '"opsz" 96, "SOFT" 60, "WONK" 1' }}>
            Mastering, glazed — four steps, one knob.
          </p>
          <p data-rise className="mt-8 measure text-[17px] leading-[1.85] text-ink-2">
            平衡、修坯、染色、限幅，四道工序按固定顺序走完。<b className="font-medium text-ink">GLAZE</b> 先听这首歌十几秒，再决定往它的理想母带推多远——你设的每个旋钮都是底，它只往上加。
          </p>
          <div data-rise className="mt-11 flex flex-wrap items-center gap-x-8 gap-y-5">
            <Magnet padding={40} magnetStrength={14}>
              <Link to="/download" className="group btn-island-primary no-underline">
                下载 {PLUGIN_VERSION}
                <span className="btn-island-icon bg-white/15">
                  <ArrowUpRight />
                </span>
              </Link>
            </Magnet>
            <LinkArrow to="/#listen">先听一听</LinkArrow>
          </div>
          <dl data-rise className="mt-16 flex flex-wrap gap-x-8 gap-y-3 text-[13px] text-ink-3">
            {[
              ['格式', 'VST3 · AU · 独立运行'],
              ['系统', 'macOS 12+ · Windows 10 / 11'],
              ['延迟', '28.8 ms（48 kHz）']
            ].map(([k, v]) => (
              <div key={k} className="flex items-baseline gap-3 border-l border-ink/15 pl-4 first:border-0 first:pl-0">
                <dt className="text-[11px] tracking-[0.2em] text-ink-4">{k}</dt>
                <dd className="text-ink-2">{v}</dd>
              </div>
            ))}
          </dl>
        </div>

        <div className="lg:col-span-5 relative z-10 flex justify-center">
          <div data-knob className="relative w-full max-w-[520px] aspect-[1/1.1]">
            <BreathingKnob />
          </div>
        </div>
      </div>

      <div data-rise className="absolute bottom-6 left-1/2 -translate-x-1/2 flex flex-col items-center gap-2 text-[11px] tracking-[0.3em] text-ink-4" aria-hidden="true">
        <span>向下 · 四道工序</span>
        <span className="block h-8 w-px bg-linear-to-b from-ink/35 to-transparent" />
      </div>
    </section>
  );
}

/**
 * 首屏旋钮：慢慢「呼吸」（35 ～ 46，像正在听一首歌），用户一碰就停下交给手。
 * 数值只在这个小组件里变，且按 0.25 量化再进状态——每秒十来次小更新，首屏其余部分不跟着重渲染。
 */
function BreathingKnob() {
  const [value, setValue] = useState(35);
  const tween = useRef<gsap.core.Tween | null>(null);

  useGSAP(() => {
    if (prefersReducedMotion()) return;
    const obj = { v: 35 };
    let last = 35;
    tween.current = gsap.to(obj, {
      v: 46,
      duration: 7,
      ease: 'sine.inOut',
      yoyo: true,
      repeat: -1,
      onUpdate: () => {
        const q = Math.round(obj.v * 4) / 4;
        if (q !== last) {
          last = q;
          setValue(q);
        }
      }
    });
  });

  const onChange = useCallback((v: number) => {
    tween.current?.kill();
    tween.current = null;
    setValue(v);
  }, []);

  return (
    <>
      <ParticleField value={value} className="absolute inset-0 h-full w-full" />
      <GlazeKnob value={value} onChange={onChange} autoIncrement={value * 0.22} size={520} className="relative h-full w-full" />
    </>
  );
}
