import { Link } from 'react-router-dom';
import Mark from './Mark';
import Seal from './Seal';
import { PLUGIN_VERSION } from '@/lib/site';

/** 页脚：落款（竖排「青瓷」+ 印章，全站唯一一枚）+ 三列链接。同一张纸，只用一根细线隔开，不换底色。 */
export default function Footer() {
  return (
    <footer className="relative border-t border-line">
      <div className="mx-auto max-w-[1280px] px-5 sm:px-8 py-16 sm:py-20 grid gap-12 md:grid-cols-12">
        <div className="md:col-span-6 flex gap-6">
          <div className="vertical display text-[56px] leading-none text-c800/85 select-none" aria-hidden="true">
            青瓷
          </div>
          <div>
            <div className="flex items-center gap-3">
              <Mark className="size-9" />
              <div>
                <div className="font-display text-xl tracking-tight text-ink">Celadon</div>
                <div className="font-mono text-[12px] tracking-[0.08em] text-ink-4">v{PLUGIN_VERSION}</div>
              </div>
            </div>
            <p className="mt-6 max-w-sm text-sm leading-relaxed text-ink-3">
              自用母带插件，Max 和朋友在用，不商用、不售卖。四道工序按固定顺序走：平衡 → 修坯 → 染色 → 限幅；GLAZE 先听这首歌，再决定推多远。
            </p>
            <Seal className="mt-8" />
          </div>
        </div>
        <FooterCol
          title="使用说明"
          links={[
            ['快速上手', '/docs'],
            ['GLAZE 与风格', '/docs/glaze'],
            ['监听方式', '/docs/listening'],
            ['常见问题', '/docs/faq']
          ]}
        />
        <FooterCol
          title="公开算法"
          links={[
            ['信号链总览', '/algorithms'],
            ['GLAZE 决策与校准', '/algorithms/glaze'],
            ['LIMIT 真峰值限幅', '/algorithms/limit'],
            ['性能与验证', '/algorithms/verification']
          ]}
        />
        <FooterCol
          title="获取"
          links={[
            [`下载 ${PLUGIN_VERSION}`, '/download'],
            ['更新日志', '/changelog'],
            ['安装与兼容', '/docs/install']
          ]}
        />
      </div>
      <div className="mx-auto max-w-[1280px] px-5 sm:px-8 pb-10 flex flex-col sm:flex-row gap-3 justify-between text-[12px] text-ink-4">
        <p>© 2026 Max Song · VST3 · AU · macOS 12+ · Windows 10 / 11</p>
        <p>
          站点动效 GSAP；部分组件改自{' '}
          <a className="underline decoration-ink/20 hover:text-ink-2" href="https://reactbits.dev" target="_blank" rel="noreferrer">
            React Bits
          </a>
          （MIT + Commons Clause）
        </p>
      </div>
    </footer>
  );
}

function FooterCol({ title, links }: { title: string; links: [string, string][] }) {
  return (
    <nav aria-label={title} className="md:col-span-2">
      <div className="text-[12px] tracking-[0.22em] text-ink-4 mb-4">{title}</div>
      <ul className="space-y-2.5">
        {links.map(([label, to]) => (
          <li key={to}>
            <Link to={to} className="text-sm text-ink-2 no-underline transition-colors duration-300 hover:text-c700">
              {label}
            </Link>
          </li>
        ))}
      </ul>
    </nav>
  );
}
