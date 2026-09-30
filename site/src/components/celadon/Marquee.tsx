import ScrollVelocity from '@/components/react-bits/ScrollVelocity';

/** 模块名跑马灯：随滚动速度加速、反向。用 Fraunces 大字，青釉与纸色交替。 */
export default function Marquee() {
  const line = (
    <span className="display text-[clamp(2.4rem,6vw,5.5rem)] leading-none tracking-[-0.01em] text-c800/90">
      Balance <span className="font-serif text-ink-4 text-[0.5em] align-middle mx-3">平衡</span>
      <span className="text-c300 mx-4">·</span>
      Clay <span className="font-serif text-ink-4 text-[0.5em] align-middle mx-3">修坯</span>
      <span className="text-c300 mx-4">·</span>
      Color <span className="font-serif text-ink-4 text-[0.5em] align-middle mx-3">染色</span>
      <span className="text-c300 mx-4">·</span>
      Limit <span className="font-serif text-ink-4 text-[0.5em] align-middle mx-3">限幅</span>
      <span className="text-c300 mx-4">·</span>
      <span className="italic text-c500">Glaze</span> <span className="font-serif text-ink-4 text-[0.5em] align-middle mx-3">施釉</span>
      <span className="text-c300 mx-4">·</span>
    </span>
  );
  return (
    <div className="relative py-10 sm:py-14 mask-fade-x select-none" aria-hidden="true">
      <div className="hairline mb-8" />
      <ScrollVelocity texts={[line]} velocity={55} numCopies={4} className="py-2" />
      <div className="hairline mt-8" />
    </div>
  );
}
