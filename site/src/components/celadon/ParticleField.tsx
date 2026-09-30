import { useEffect, useRef } from 'react';
import { prefersReducedMotion, seeded } from '@/lib/utils';

/**
 * 主旋钮两侧的流线粒子：左边汇向旋钮、右边流出（声音从左进 GLAZE、从右出）。
 * 数量 = 上限 × GLAZE^0.8，约 15 % 是模糊大颗粒；「减少动效」时不画。只在可见时跑 rAF。
 */
export default function ParticleField({ value, maxPerSide = 90, className }: { value: number; maxPerSide?: number; className?: string }) {
  const ref = useRef<HTMLCanvasElement>(null);
  const valueRef = useRef(value);
  valueRef.current = value;

  useEffect(() => {
    const canvas = ref.current;
    if (!canvas || prefersReducedMotion()) return;
    const ctx = canvas.getContext('2d');
    if (!ctx) return;

    const rnd = seeded(23);
    type P = { side: -1 | 1; t: number; lane: number; speed: number; size: number; blur: boolean; alpha: number };
    const particles: P[] = Array.from({ length: maxPerSide * 2 }, (_, i) => ({
      side: i % 2 === 0 ? -1 : 1,
      t: rnd(),
      lane: rnd(),
      speed: 0.12 + rnd() * 0.22,
      size: 1 + rnd() * 1.6,
      blur: rnd() < 0.15,
      alpha: 0.35 + rnd() * 0.5
    }));

    let raf = 0;
    let visible = true;
    let last = performance.now();
    const dpr = Math.min(2, window.devicePixelRatio || 1);

    const resize = () => {
      const r = canvas.getBoundingClientRect();
      canvas.width = Math.max(1, Math.floor(r.width * dpr));
      canvas.height = Math.max(1, Math.floor(r.height * dpr));
    };
    const ro = new ResizeObserver(resize);
    ro.observe(canvas);
    resize();

    const io = new IntersectionObserver(([e]) => {
      visible = e.isIntersecting;
      if (visible && !raf) raf = requestAnimationFrame(loop);
    });
    io.observe(canvas);

    function loop(now: number) {
      raf = 0;
      if (!visible || !ctx) return;
      const dt = Math.min(0.05, (now - last) / 1000);
      last = now;
      const W = canvas!.width;
      const H = canvas!.height;
      ctx.clearRect(0, 0, W, H);
      const v = Math.max(0, Math.min(1, valueRef.current / 100));
      const count = Math.round(maxPerSide * Math.pow(v, 0.8));
      const cx = W / 2;
      const cy = H / 2;
      const knobR = Math.min(W, H) * 0.24;
      for (let i = 0; i < particles.length; i++) {
        const p = particles[i];
        // 每边前 count 个活着
        if (Math.floor(i / 2) >= count) continue;
        p.t += dt * p.speed;
        if (p.t > 1) {
          p.t -= 1;
          p.lane = rnd();
        }
        // 流线：从边缘出发，沿一条弧汇向旋钮（左）或从旋钮流出（右）
        const laneY = (p.lane - 0.5) * H * 0.9;
        const prog = p.side < 0 ? p.t : 1 - p.t; // 左：0 边缘 → 1 旋钮
        const edgeX = p.side < 0 ? 0 : W;
        const targetX = cx + p.side * (knobR + 10 * dpr);
        const x = edgeX + (targetX - edgeX) * (1 - Math.pow(1 - prog, 1.8));
        const y = cy + laneY * Math.pow(1 - prog, 1.4) + Math.sin(prog * 6 + i) * 6 * dpr * (1 - prog);
        const fade = Math.sin(Math.min(1, Math.max(0, prog)) * Math.PI);
        const a = p.alpha * fade * (0.35 + v * 0.65);
        ctx.beginPath();
        ctx.arc(x, y, p.size * dpr * (p.blur ? 2.6 : 1), 0, Math.PI * 2);
        if (p.blur) {
          ctx.fillStyle = `rgba(23, 153, 171, ${a * 0.35})`;
          ctx.shadowColor = 'rgba(23,153,171,0.6)';
          ctx.shadowBlur = 10 * dpr;
        } else {
          ctx.fillStyle = `rgba(14, 100, 115, ${a})`;
          ctx.shadowBlur = 0;
        }
        ctx.fill();
      }
      ctx.shadowBlur = 0;
      raf = requestAnimationFrame(loop);
    }
    raf = requestAnimationFrame(loop);

    return () => {
      if (raf) cancelAnimationFrame(raf);
      ro.disconnect();
      io.disconnect();
    };
  }, [maxPerSide]);

  return <canvas ref={ref} className={className} aria-hidden="true" />;
}
