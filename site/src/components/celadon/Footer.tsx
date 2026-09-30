import { Link } from 'react-router-dom';
import Mark from './Mark';
import Seal from './Seal';
import { PLUGIN_VERSION } from '@/lib/site';

export default function Footer() {
  return (
    <footer className="relative mt-32 border-t border-line bg-paper-2/60">
      <div className="mx-auto max-w-[1280px] px-5 sm:px-8 py-16 sm:py-20 grid gap-12 md:grid-cols-12">
        <div className="md:col-span-5 flex gap-6">
          <div className="vertical display text-[56px] leading-none text-c800/90 select-none">青瓷</div>
          <div>
            <div className="flex items-center gap-3">
              <Mark className="size-9" />
              <div>
                <div className="font-display text-xl tracking-tight text-ink">Celadon</div>
                <div className="text-[12px] tracking-[0.2em] text-ink-3">MASTERING · {PLUGIN_VERSION}</div>
              </div>
            </div>
            <p className="mt-6 max-w-sm text-sm leading-relaxed text-ink-3">
              自用母带插件，Max 和朋友用，不商用、不售卖。四道工序按固定顺序走：BALANCE → CLAY → COLOR → LIMIT；GLAZE 先听这首歌，再决定推多远。
            </p>
            <Seal className="mt-8" />
          </div>
        </div>
        <FooterCol
          title="使用说明"
          links={[
            ['快速上手', '/docs'],
            ['安装与兼容', '/docs/install'],
            ['GLAZE 与风格', '/docs/glaze'],
            ['监听方式', '/docs/listening'],
            ['常见问题', '/docs/faq']
          ]}
        />
        <FooterCol
          title="公开算法"
          links={[
            ['信号链总览', '/algorithms'],
            ['LIMIT 真峰值限幅', '/algorithms/limit'],
            ['BALANCE 动态频谱平衡', '/algorithms/balance'],
            ['GLAZE 决策与校准', '/algorithms/glaze'],
            ['性能与验证', '/algorithms/verification']
          ]}
        />
        <div className="md:col-span-3 text-sm text-ink-3 leading-relaxed">
          <div className="text-[12px] uppercase tracking-[0.22em] text-ink-4 mb-4">格式与平台</div>
          <ul className="space-y-1.5">
            <li>VST3 · AU · Standalone</li>
            <li>macOS 12+，Apple Silicon 与 Intel 各一个安装包</li>
            <li>Windows 10 / 11，x64（VST3 · Standalone）</li>
            <li>FL Studio 2026 · Ableton Live 12 实测</li>
          </ul>
        </div>
      </div>
      <div className="mx-auto max-w-[1280px] px-5 sm:px-8 pb-10 flex flex-col sm:flex-row gap-3 justify-between text-[12px] text-ink-4">
        <p>© 2026 Max Song · JUCE 9.0.2 · 参数 ID 一经发布不改</p>
        <p>
          站点动效：GSAP 3 与 <a className="underline decoration-ink/20 hover:text-ink-2" href="https://reactbits.dev" target="_blank" rel="noreferrer">React Bits</a>（MIT + Commons Clause）
        </p>
      </div>
    </footer>
  );
}

function FooterCol({ title, links }: { title: string; links: [string, string][] }) {
  return (
    <div className="md:col-span-2">
      <div className="text-[12px] uppercase tracking-[0.22em] text-ink-4 mb-4">{title}</div>
      <ul className="space-y-2">
        {links.map(([label, to]) => (
          <li key={to}>
            <Link to={to} className="text-sm text-ink-2 no-underline hover:text-c700 transition-colors">
              {label}
            </Link>
          </li>
        ))}
      </ul>
    </div>
  );
}
