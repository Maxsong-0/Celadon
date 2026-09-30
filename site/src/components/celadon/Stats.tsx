import CountUp from '@/components/react-bits/CountUp';
import { Eyebrow } from './Eyebrow';

const stats: { to: number; from?: number; unit: string; label: string; note: string; separator?: string }[] = [
  { to: 27, unit: '段', label: '动态频谱平衡', note: '1/3 倍频程，40 Hz ～ 16 kHz' },
  { to: 8, unit: '×', label: '真峰值插值', note: '8 相 × 64 抽头，限的峰 = 量的峰' },
  { to: 1.3, unit: '%', label: '音频线程预算', note: '48 kHz · M2 Pro · Release · 界面关' },
  { to: 132981, unit: '', label: '自动测试断言', note: 'Linux GCC 与 MSVC + Wine 全套 · 0 失败', separator: ',' },
  { to: 0.35, unit: 'dB', label: '真峰值重建余量', note: '16× 参考检测最坏仍低于 Ceiling' },
  { to: 28.8, unit: 'ms', label: '总延迟', note: '48 kHz · Lookahead 2 ms · 宿主自动补偿' }
];

export default function Stats() {
  return (
    <section className="relative py-24 sm:py-32">
      <div className="mx-auto max-w-[1280px] px-5 sm:px-8">
        <div className="flex flex-col gap-6 sm:flex-row sm:items-end sm:justify-between">
          <Eyebrow>数字 · 全部来自验证记录</Eyebrow>
          <p className="max-w-md text-[13px] text-ink-4">百分比 = 单个音频线程「处理耗时 / 这一块音频的播放时长」，不是整机 CPU；测试数是 main 当前提交的全套结果。</p>
        </div>
        <div className="mt-12 grid grid-cols-2 gap-x-6 gap-y-12 md:grid-cols-3 lg:grid-cols-6">
          {stats.map(s => (
            <div key={s.label} className="border-t border-line pt-5">
              <div className="display text-[clamp(2.2rem,4vw,3.4rem)] leading-none text-ink tabular-nums">
                <CountUp to={s.to} from={0} duration={1.6} separator={s.separator} />
                <span className="ml-1 text-[0.45em] text-ink-3">{s.unit}</span>
              </div>
              <div className="mt-3 text-[14px] font-medium text-ink-2">{s.label}</div>
              <div className="mt-1 text-[12px] leading-relaxed text-ink-4">{s.note}</div>
            </div>
          ))}
        </div>
      </div>
    </section>
  );
}
