import { useRef, type ReactNode } from 'react';
import { Em, LinkArrow, SectionHead } from './Eyebrow';
import { useReveal } from '@/lib/reveal';

interface Row {
  title: string;
  en: string;
  body: string;
  art: ReactNode;
}

// 左栏：三种「听清楚差别」的手段，细线分行的纯排版（设计规格-官网 第 4 节「伍 · 判断」）。
const rows: Row[] = [
  {
    title: '等响 A / B',
    en: 'Loudness-matched A / B',
    body: '测整条链的实测响度差，把较响的一边衰减到另一边；只衰减、不放大，表仍显示衰减前的母带。判断「变好了」还是「只是变响了」。',
    art: <ArtAB />
  },
  {
    title: '试听差值',
    en: 'Listen to the difference',
    body: 'LIMIT 卡片上的耳机：只听被削掉的那部分。鼓点有没有变扁、低音有没有发闷，一听就知道。',
    art: <ArtDiff />
  },
  {
    title: '参考学习',
    en: 'Reference from sidechain',
    body: '把参考曲接进宿主的外部侧链，累计 10 秒有效声音，得到 27 段相对频谱目标；参考曲不进主输出。',
    art: <ArtRef />
  }
];

/** 伍 · 判断：左三行手段，右两块承诺。 */
export default function Bento() {
  const root = useRef<HTMLElement>(null);
  useReveal(root);

  return (
    <section ref={root} className="section relative">
      <div className="mx-auto max-w-[1280px] px-5 sm:px-8">
        <SectionHead
          chapter={{ n: '伍', label: '判断', en: 'Judgement' }}
          title={
            <>
              让你能判断，<Em>而不是只能相信</Em>。
            </>
          }
          lead="母带最难的不是处理，是判断。Celadon 把「听清楚差别」当成功能来做：等响、差值、参考，外加一条写进测试里的逐位承诺。"
        />

        <div className="mt-16 grid gap-12 lg:grid-cols-12 lg:gap-10">
          <ul className="rule-list lg:col-span-7">
            {rows.map((r, i) => (
              <li key={r.title} data-reveal className="grid gap-6 py-8 sm:grid-cols-[minmax(0,1fr)_200px] sm:items-center">
                <div>
                  <div className="flex items-baseline gap-3">
                    <span className="font-mono text-[12px] text-c700">0{i + 1}</span>
                    <h3 className="font-serif text-[24px] font-semibold text-ink">{r.title}</h3>
                    <span className="display text-[13px] italic text-ink-4 hidden sm:inline">{r.en}</span>
                  </div>
                  <p className="mt-3 measure text-[15px] leading-[1.85] text-ink-2">{r.body}</p>
                </div>
                <div className="text-ink-3">{r.art}</div>
              </li>
            ))}
            <li data-reveal className="py-6">
              <LinkArrow to="/docs/listening">监听方式的完整说明</LinkArrow>
            </li>
          </ul>

          <div className="lg:col-span-5 grid content-start gap-5">
            <div data-reveal className="biscuit p-7 sm:p-8">
              <div className="display text-[13px] italic text-ink-4">Bit-exact contract</div>
              <h3 className="mt-1 font-serif text-[24px] font-semibold text-ink">逐位契约</h3>
              <p className="mt-3 text-[15px] leading-[1.85] text-ink-2">GLAZE 为 0、模块归零或旁通时，输出与输入零差；任意块长、SIMD 与标量、空闲跳过与一直在跑——逐位相同。</p>
              <ArtBits />
            </div>
            <div data-reveal className="biscuit p-7 sm:p-8">
              <div className="display text-[13px] italic text-ink-4">Release safety</div>
              <h3 className="mt-1 font-serif text-[24px] font-semibold text-ink">发行安全</h3>
              <p className="mt-3 text-[15px] leading-[1.85] text-ink-2">归一化预听（只衰减）、编码余量、TPDF 抖动。导出前该关的，界面一直提醒。</p>
              <ArtRelease />
            </div>
          </div>
        </div>
      </div>
    </section>
  );
}

/* ---------------- 行内小图：只用细线与单色，数据色（紫 / 橙）只表达它们的本义 ---------------- */

function ArtAB() {
  return (
    <svg viewBox="0 0 200 72" className="w-full h-auto" aria-hidden="true">
      {Array.from({ length: 24 }, (_, i) => {
        const h = 10 + 26 * Math.abs(Math.sin(i * 0.7)) + (i % 5 === 0 ? 12 : 0);
        const after = i >= 12;
        // 右半 = 处理后，已衰减到与左半同响：峰被收住，但整体不比左边高
        const hh = after ? Math.min(h * 1.08, 40) : h;
        return <rect key={i} x={4 + i * 8.2} y={32 - hh / 2} width="3.6" height={hh} rx="1.8" fill={after ? '#127F90' : '#A9C3B7'} />;
      })}
      <line x1="100" y1="2" x2="100" y2="62" stroke="#0D1B21" strokeOpacity="0.35" strokeDasharray="2 3" />
      <text x="4" y="70" style={{ fontFamily: 'var(--font-mono)', fontSize: 8.5, fill: '#6E8189' }}>B 原声</text>
      <text x="196" y="70" textAnchor="end" style={{ fontFamily: 'var(--font-mono)', fontSize: 8.5, fill: '#0E6473' }}>A 等响后 −4.8 dB</text>
    </svg>
  );
}
function ArtDiff() {
  return (
    <svg viewBox="0 0 200 72" className="w-full h-auto" aria-hidden="true">
      <line x1="4" y1="32" x2="196" y2="32" stroke="#DCE6E9" />
      {Array.from({ length: 27 }, (_, i) => {
        const h = Math.max(0, 22 * Math.abs(Math.sin(i * 0.9)) - 12) * 2;
        return <rect key={i} x={4 + i * 7.2} y={32 - Math.max(1, h) / 2} width="2.6" height={Math.max(1, h)} rx="1.3" fill="#E8590C" opacity="0.8" />;
      })}
      <text x="4" y="70" style={{ fontFamily: 'var(--font-mono)', fontSize: 8.5, fill: '#C2410C' }}>只听被削掉的部分</text>
    </svg>
  );
}
function ArtRef() {
  const pts = Array.from({ length: 27 }, (_, i) => [6 + i * 7.1, 34 - 16 * Math.sin((i / 26) * Math.PI)] as const);
  return (
    <svg viewBox="0 0 200 72" className="w-full h-auto" aria-hidden="true">
      <path d={pts.map(([x, y], i) => `${i ? 'L' : 'M'}${x.toFixed(1)} ${y.toFixed(1)}`).join(' ')} fill="none" stroke="#127F90" strokeWidth="1.2" />
      {pts.map(([x, y], i) => (
        <circle key={i} cx={x} cy={y + ((i % 4) - 1.5) * 1.6} r="1.7" fill="#0D1B21" opacity="0.45" />
      ))}
      <text x="4" y="70" style={{ fontFamily: 'var(--font-mono)', fontSize: 8.5, fill: '#6E8189' }}>侧链 10 / 10 s · 参考就绪</text>
    </svg>
  );
}
function ArtBits() {
  return (
    <dl className="mt-6 space-y-1.5 border-t border-line pt-5 font-mono text-[12px] text-ink-3">
      {[
        ['GLAZE 0 vs 参照链', 'Δ = 0 bit'],
        ['SIMD vs 标量', 'Δ = 0 bit'],
        ['块长 17 vs 2048', 'Δ = 0 bit']
      ].map(([k, v]) => (
        <div key={k} className="flex justify-between gap-4">
          <dt>{k}</dt>
          <dd className="text-c700">{v}</dd>
        </div>
      ))}
    </dl>
  );
}
function ArtRelease() {
  return (
    <dl className="mt-6 grid grid-cols-3 border-t border-line pt-5">
      {[
        ['−14', 'LUFS 预听'],
        ['1.5', 'dB 编码余量上限'],
        ['24', 'bit TPDF']
      ].map(([v, l]) => (
        // dt 在前（合法的 dl 结构），视觉上数字在上
        <div key={l} className="flex flex-col-reverse">
          <dt className="mt-2 text-[11px] leading-snug text-ink-4">{l}</dt>
          <dd className="display text-[26px] leading-none text-ink tabular-nums">{v}</dd>
        </div>
      ))}
    </dl>
  );
}
