import { Link } from 'react-router-dom';
import GlareHover from '@/components/react-bits/GlareHover';
import { Em, SectionHead } from './Eyebrow';
import Seal from './Seal';

const platforms = [
  {
    name: 'macOS',
    req: 'macOS 12 Monterey 及以上',
    arch: 'Apple Silicon（arm64）/ Intel（x86_64）各一个安装包',
    formats: ['AU', 'VST3', 'Standalone'],
    note: '按芯片选包，两种芯片的 AU 都过 auval。安装包没有公证：第一次打开到「隐私与安全性」点「仍要打开」。'
  },
  {
    name: 'Windows',
    req: 'Windows 10 / 11，64 位',
    arch: 'x64 · 静态运行库，不用装 VC++',
    formats: ['VST3', 'Standalone'],
    note: '安装包无代码签名：SmartScreen 里点「更多信息 → 仍要运行」。'
  }
];

export default function Platforms() {
  return (
    <section id="get" className="relative py-28 sm:py-36">
      <div className="mx-auto max-w-[1280px] px-5 sm:px-8">
        <SectionHead align="center" chapter={{ n: '捌', label: '获取', en: 'Get Celadon' }} title={<>自用之作，<Em>与朋友分享</Em>。</>} lead="Celadon 不商用、不售卖。Max 和朋友在用；想试听，直接找 Max 拿对应平台的包，装好后在宿主里重新扫描插件。" />
        <div className="mt-16 grid gap-6 md:grid-cols-2">
          {platforms.map(p => (
            <GlareHover key={p.name} width="100%" height="auto" background="transparent" borderRadius="2rem" borderColor="transparent" glareColor="#67CAD7" glareOpacity={0.22} glareAngle={-35} glareSize={220} transitionDuration={900} className="bezel">
              <div className="bezel-core w-full p-8 sm:p-10">
                <div className="flex flex-col gap-4 sm:flex-row sm:items-start sm:justify-between">
                  <div>
                    <div className="display text-4xl text-ink">{p.name}</div>
                    <div className="mt-2 text-[14px] text-ink-3">{p.req}</div>
                  </div>
                  <div className="flex flex-wrap gap-2 sm:justify-end">
                    {p.formats.map(f => (
                      <span key={f} className="rounded-full bg-c100/70 px-3 py-1 text-[11px] font-medium text-c800 ring-1 ring-c200">{f}</span>
                    ))}
                  </div>
                </div>
                <dl className="mt-8 grid gap-4 text-[14px] sm:grid-cols-2">
                  <div>
                    <dt className="text-[10px] uppercase tracking-[0.2em] text-ink-4">架构</dt>
                    <dd className="mt-1 text-ink-2">{p.arch}</dd>
                  </div>
                  <div>
                    <dt className="text-[10px] uppercase tracking-[0.2em] text-ink-4">备注</dt>
                    <dd className="mt-1 text-ink-2">{p.note}</dd>
                  </div>
                </dl>
              </div>
            </GlareHover>
          ))}
        </div>
        <div className="mt-12 flex flex-col items-center gap-6">
          <Link to="/docs/install" className="group btn-island-primary no-underline">
            安装与兼容说明
            <span className="btn-island-icon bg-white/15">
              <svg viewBox="0 0 16 16" className="size-3.5" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round"><path d="M4 12 12 4M6 4h6v6" /></svg>
            </span>
          </Link>
          <Seal />
        </div>
      </div>
    </section>
  );
}
