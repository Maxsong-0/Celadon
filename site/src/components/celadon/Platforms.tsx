import { useRef } from 'react';
import { Link } from 'react-router-dom';
import Magnet from '@/components/react-bits/Magnet';
import { ArrowUpRight, Em, LinkArrow, SectionHead } from './Eyebrow';
import { useReveal } from '@/lib/reveal';
import { PLUGIN_VERSION } from '@/lib/site';

const platforms = [
  {
    name: 'macOS',
    req: 'macOS 12 Monterey 及以上',
    formats: 'AU · VST3 · 独立运行',
    arch: 'Apple Silicon 与 Intel 各一个安装包',
    note: '看「关于本机 → 芯片」选包。安装包没有公证，第一次打开到「隐私与安全性」点「仍要打开」。'
  },
  {
    name: 'Windows',
    req: 'Windows 10 / 11，64 位',
    formats: 'VST3 · 独立运行',
    arch: 'x64 · 静态运行库，不用另装 VC++',
    note: '安装包没有代码签名：SmartScreen 里点「更多信息 → 仍要运行」。'
  }
];

/** 捌 · 获取：两栏纯排版 + 一个主按钮。页面最后一节，底部多留一点。 */
export default function Platforms() {
  const root = useRef<HTMLElement>(null);
  useReveal(root);

  return (
    <section id="get" ref={root} className="section-last relative">
      <div className="mx-auto max-w-[1280px] px-5 sm:px-8">
        <SectionHead
          align="center"
          chapter={{ n: '捌', label: '获取', en: 'Get Celadon' }}
          title={
            <>
              自用之作，<Em>与朋友分享</Em>。
            </>
          }
          lead="Celadon 不商用、不售卖。Max 和朋友在用；按电脑选一个安装包，装好后在宿主里重新扫描插件。"
        />

        <div className="mx-auto mt-16 grid max-w-5xl gap-x-16 md:grid-cols-2">
          {platforms.map(p => (
            <div key={p.name} data-reveal className="border-t border-line py-8">
              <div className="flex items-baseline justify-between gap-4">
                <h3 className="display text-[40px] leading-none text-ink">{p.name}</h3>
                <span className="text-[13px] text-ink-3">{p.formats}</span>
              </div>
              <dl className="mt-6 space-y-3 text-[15px]">
                <div className="grid grid-cols-[64px_1fr] gap-4">
                  <dt className="text-[12px] tracking-[0.2em] text-ink-4 pt-0.5">系统</dt>
                  <dd className="text-ink-2">{p.req}</dd>
                </div>
                <div className="grid grid-cols-[64px_1fr] gap-4">
                  <dt className="text-[12px] tracking-[0.2em] text-ink-4 pt-0.5">架构</dt>
                  <dd className="text-ink-2">{p.arch}</dd>
                </div>
                <div className="grid grid-cols-[64px_1fr] gap-4">
                  <dt className="text-[12px] tracking-[0.2em] text-ink-4 pt-0.5">首次</dt>
                  <dd className="text-ink-3 leading-relaxed">{p.note}</dd>
                </div>
              </dl>
            </div>
          ))}
        </div>

        <div data-reveal className="mt-14 flex flex-wrap items-center justify-center gap-x-8 gap-y-5">
          <Magnet padding={40} magnetStrength={14}>
            <Link to="/download" className="group btn-island-primary no-underline">
              下载 {PLUGIN_VERSION}
              <span className="btn-island-icon bg-white/15">
                <ArrowUpRight />
              </span>
            </Link>
          </Magnet>
          <LinkArrow to="/docs/install">安装与兼容说明</LinkArrow>
          <LinkArrow to="/changelog">更新日志</LinkArrow>
        </div>
      </div>
    </section>
  );
}
