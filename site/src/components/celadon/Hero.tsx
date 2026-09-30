import { lazy, Suspense, useEffect, useRef, useState } from 'react';
import { Link } from 'react-router-dom';
import SplitText from '@/components/react-bits/SplitText';
import Magnet from '@/components/react-bits/Magnet';
import GlazeKnob from './GlazeKnob';
import ParticleField from './ParticleField';
import { Eyebrow } from './Eyebrow';
import { gsap, useGSAP } from '@/lib/gsap';
import { prefersReducedMotion } from '@/lib/utils';

const Topography = lazy(() => import('@/components/react-bits/Topography'));

/** 首屏：釉面等高线（WebGL）+ 逐字浮现的标题 + 会呼吸的青瓷旋钮。 */
export default function Hero() {
  const root = useRef<HTMLElement>(null);
  const [value, setValue] = useState(35);
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
      if (!root.current) return;
      const q = gsap.utils.selector(root);
      gsap.from(q('[data-rise]'), { y: 28, opacity: 0, filter: 'blur(6px)', duration: 1.1, stagger: 0.12, delay: 0.35, ease: 'power3.out' });
      gsap.from(q('[data-knob]'), { scale: 0.92, opacity: 0, duration: 1.4, delay: 0.2, ease: 'power3.out' });
      if (!prefersReducedMotion()) {
        // 旋钮慢慢呼吸：30 ～ 46，像正在聆听一首歌
        const obj = { v: 35 };
        gsap.to(obj, {
          v: 46,
          duration: 7,
          ease: 'sine.inOut',
          yoyo: true,
          repeat: -1,
          onUpdate: () => setValue(obj.v)
        });
      }
    },
    { scope: root }
  );

  return (
    <section ref={root} className="relative min-h-[100dvh] overflow-hidden pt-28 sm:pt-32">
      {/* 釉面等高线 */}
      <div className="absolute inset-0 -z-10 mask-fade-b" aria-hidden="true">
        {webgl ? (
          <Suspense fallback={null}>
            <div className="absolute inset-0 opacity-70 mix-blend-multiply">
              <Topography
                lowColor="#D5F1F5"
                midColor="#67CAD7"
                highColor="#1799AB"
                bands={2.4}
                thickness={0.011}
                glow={0.25}
                speed={0.22}
                morphAmount={2.4}
                morphSpeed={0.035}
                contrast={2.2}
                grain={false}
                mouseRadius={0.28}
                mouseStrength={0.35}
                opacity={0.85}
              />
            </div>
          </Suspense>
        ) : (
          <div className="absolute inset-0 bg-[radial-gradient(60%_50%_at_70%_30%,rgba(163,224,232,0.45),transparent_70%)]" />
        )}
        <div className="absolute inset-0 bg-[radial-gradient(70%_60%_at_30%_20%,rgba(243,241,234,0.9),transparent_65%)]" />
      </div>

      <div className="mx-auto grid max-w-[1280px] grid-cols-1 items-center gap-12 px-5 sm:px-8 lg:grid-cols-12 lg:gap-6 pb-20">
        <div className="lg:col-span-7 relative z-10">
          <div data-rise>
            <Eyebrow>Celadon · 青瓷 · 智能母带 · 0.1.9.0</Eyebrow>
          </div>
          <h1 className="mt-8 font-serif text-[clamp(2.6rem,7vw,6.2rem)] font-semibold leading-[1.05] tracking-[-0.02em] text-ink">
            <SplitText text="一挂总轨，" tag="span" className="block" splitType="chars" delay={45} duration={1.1} from={{ opacity: 0, y: 46, rotateX: -40 }} to={{ opacity: 1, y: 0, rotateX: 0 }} threshold={0} rootMargin="0px" textAlign="left" />
            <SplitText text="就好听。" tag="span" className="block glaze-text" splitType="chars" delay={45} duration={1.1} from={{ opacity: 0, y: 46, rotateX: -40 }} to={{ opacity: 1, y: 0, rotateX: 0 }} threshold={0} rootMargin="0px" textAlign="left" />
          </h1>
          <p data-rise className="display mt-5 text-[clamp(1.1rem,2vw,1.6rem)] italic text-ink-3" style={{ fontVariationSettings: '"opsz" 96, "SOFT" 60, "WONK" 1' }}>
            Mastering, glazed — four steps, one knob.
          </p>
          <p data-rise className="mt-8 max-w-xl text-[17px] leading-[1.85] text-ink-2">
            <b className="font-medium text-ink">BALANCE</b> 找回被遮住的频段，<b className="font-medium text-ink">CLAY</b> 把坯体修干净，<b className="font-medium text-ink">COLOR</b> 添一层温度，<b className="font-medium text-ink">LIMIT</b> 推响不破。
            <br className="hidden sm:block" />
            <b className="font-medium text-ink">GLAZE</b> 先听这首歌十几秒，再决定往它的理想母带推多远。
          </p>
          <div data-rise className="mt-10 flex flex-wrap items-center gap-3">
            <Magnet padding={40} magnetStrength={14}>
              <Link to="/docs" className="group btn-island-primary no-underline">
                阅读使用说明
                <span className="btn-island-icon bg-white/15">
                  <Arrow />
                </span>
              </Link>
            </Magnet>
            <Magnet padding={40} magnetStrength={14}>
              <Link to="/algorithms" className="group btn-island-ghost no-underline">
                看公开算法
                <span className="btn-island-icon bg-ink/6">
                  <Arrow />
                </span>
              </Link>
            </Magnet>
          </div>
          <dl data-rise className="mt-14 grid max-w-xl grid-cols-3 gap-6 text-[13px] text-ink-3">
            <div>
              <dt className="uppercase tracking-[0.18em] text-[10px] text-ink-4">格式</dt>
              <dd className="mt-1 text-ink-2">VST3 · AU · 独立运行</dd>
            </div>
            <div>
              <dt className="uppercase tracking-[0.18em] text-[10px] text-ink-4">平台</dt>
              <dd className="mt-1 text-ink-2">macOS 12+ 通用 · Windows x64</dd>
            </div>
            <div>
              <dt className="uppercase tracking-[0.18em] text-[10px] text-ink-4">真峰值</dt>
              <dd className="mt-1 text-ink-2">8× 检测 · 1× 施加</dd>
            </div>
          </dl>
        </div>

        <div className="lg:col-span-5 relative z-10 flex justify-center">
          <div data-knob className="relative w-full max-w-[520px] aspect-[1/1.1]">
            <ParticleField value={value} className="absolute inset-0 h-full w-full" />
            <GlazeKnob value={value} onChange={setValue} autoIncrement={value * 0.22} size={520} className="relative h-full w-full" />
          </div>
        </div>
      </div>

      <div data-rise className="absolute bottom-6 left-1/2 -translate-x-1/2 flex flex-col items-center gap-2 text-[10px] uppercase tracking-[0.3em] text-ink-4">
        <span>向下滚动 · 四道工序</span>
        <span className="block h-8 w-px bg-linear-to-b from-ink/40 to-transparent" />
      </div>
    </section>
  );
}

function Arrow() {
  return (
    <svg viewBox="0 0 16 16" className="size-3.5" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round">
      <path d="M4 12 12 4M6 4h6v6" />
    </svg>
  );
}
