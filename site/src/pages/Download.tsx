import { useEffect, useRef, useState, type ReactNode } from 'react';
import { useLocation } from 'react-router-dom';
import { Chapter, Em, LinkArrow } from '@/components/celadon/Eyebrow';
import { CHECKSUMS_FILE, RELEASE, downloadHref, type PackageId, type ReleasePackage } from '@/content/release';
import { ScrollTrigger } from '@/lib/gsap';
import { onFontsLoaded } from '@/lib/fonts';
import { scrollToAnchor } from '@/lib/anchor';
import { useReveal } from '@/lib/reveal';
import { PLUGIN_VERSION } from '@/lib/site';
import { cn } from '@/lib/utils';

/*
  /download：三张平台卡 + 第一次打开的放行步骤 + 装好之后的注意事项。
  安装包信息只从 content/release.ts 读；它与插件版本（PLUGIN_VERSION）不一致时不给旧文件，只提示还没更新。
  按设计规格：猜得到这台电脑用哪个包时，那一张是瓷板 + 主按钮，其余是素坯 + 文字链接（同一行最多一块瓷板）。
*/

interface Platform {
  /** 页内锚点：/download#mac-apple-silicon 等 */
  anchor: string;
  os: string;
  chip: string;
  system: string;
  formats: string;
  how: ReactNode;
}

const PLATFORMS: Record<PackageId, Platform> = {
  'mac-arm64': {
    anchor: 'mac-apple-silicon',
    os: 'Mac',
    chip: 'Apple 芯片',
    system: 'M1 及以后 · macOS 12 及以上',
    formats: 'VST3 · AU · 独立运行',
    how: <>苹果菜单 → 关于本机，「芯片」一栏写 Apple M…</>
  },
  'mac-x64': {
    anchor: 'mac-intel',
    os: 'Mac',
    chip: 'Intel 处理器',
    system: 'macOS 12 及以上',
    formats: 'VST3 · AU · 独立运行',
    how: <>「关于本机」里「处理器」写 Intel。Apple 芯片的 Mac 上用 Rosetta 打开的宿主，也装这个。</>
  },
  'win-x64': {
    anchor: 'windows',
    os: 'Windows',
    chip: '64 位',
    system: 'Windows 10 / 11 · 不用另装运行库',
    formats: 'VST3 · 独立运行',
    how: <>Windows 11 都是 64 位；Windows 10 在「设置 → 系统 → 关于」看「系统类型」。</>
  }
};

export default function Download() {
  const root = useRef<HTMLElement>(null);
  useReveal(root);
  usePageSetup('下载 · Celadon');
  const pick = usePlatformGuess();
  // 安装包与插件版本对不上（改了 .jucer 还没出包）时，宁可不给，也不给错文件。
  const ready = RELEASE.version === PLUGIN_VERSION;

  return (
    <main ref={root}>
      <section className="section pt-32 sm:pt-40">
        <div className="mx-auto max-w-[1280px] px-5 sm:px-8">
          <header className="max-w-3xl">
            <div data-reveal>
              <Chapter n="取" label="下载" en="Download" />
            </div>
            <h1 data-reveal className="mt-8 font-serif text-[clamp(2rem,4.2vw,3.5rem)] font-medium leading-[1.12] tracking-tight text-ink">
              按电脑，<Em>选一个</Em>。
            </h1>
            <p data-reveal className="mt-6 measure text-[17px] leading-[1.85] text-ink-2">
              {ready ? (
                <>
                  Celadon {RELEASE.version}，{formatDate(RELEASE.date)}发布。Mac 按芯片分两个包，Windows 一个；装好后在宿主里重新扫描插件。
                </>
              ) : (
                <>Celadon {PLUGIN_VERSION}：Mac 按芯片分两个包，Windows 一个。</>
              )}
            </p>
          </header>

          {ready ? (
            <div className="mt-16 grid gap-5 xl:grid-cols-3">
              {RELEASE.packages.map(pkg => (
                <PackageCard key={pkg.id} pkg={pkg} platform={PLATFORMS[pkg.id]} badge={pick?.id === pkg.id ? badgeFor(pick) : null} />
              ))}
            </div>
          ) : (
            <StaleNotice />
          )}
        </div>
      </section>

      <section className="section-last">
        <div className="mx-auto max-w-[1280px] px-5 sm:px-8">
          <div className="max-w-3xl">
            <div data-reveal>
              <Kicker label="第一次打开" en="First launch" />
            </div>
            <h2 data-reveal className="mt-8 font-serif text-[clamp(1.75rem,3.4vw,2.75rem)] font-medium leading-[1.15] tracking-tight text-ink">
              系统会拦一下，<Em>放行一次</Em>就好。
            </h2>
            <p data-reveal className="mt-6 measure text-[17px] leading-[1.85] text-ink-2">
              安装包没有 Apple 开发者签名，也没有 Windows 代码签名，第一次打开会被系统拦下。照下面放行即可，不用关掉系统的安全设置。
            </p>
          </div>

          <div className="mt-16 grid gap-14 md:grid-cols-2 md:gap-16">
            <Steps title="Mac" steps={MAC_STEPS} />
            <Steps title="Windows" steps={WIN_STEPS} />
          </div>

          <div className="mt-24 sm:mt-28">
            <div data-reveal>
              <Kicker label="装好之后" en="After installing" />
            </div>
            <ul className="rule-list mt-10">
              <AfterRow label="重新扫描">在宿主里重新扫描插件；Logic 会自动验证 AU。宿主一直开着的，保存工程后重启一次，免得它还用着旧版本。</AfterRow>
              <AfterRow label="别装两份">
                自己从 Xcode 编过、装在个人插件目录 <Code>~/Library/Audio/Plug-Ins/</Code> 的，不要再装安装包——宿主里会出现两个 Celadon。
              </AfterRow>
              {ready ? (
                <AfterRow label="核对文件">
                  可选。Mac 在终端里用 <Code>shasum -a 256 文件</Code>，Windows 在 PowerShell 里用 <Code>Get-FileHash 文件</Code>，算出的值与卡片上的 SHA-256 相同（不分大小写）就是完整的。三个包的校验值也合在一份{' '}
                  <a href={downloadHref(CHECKSUMS_FILE)} download className="font-mono text-[0.92em] text-c700 underline decoration-c300 decoration-1 underline-offset-4 transition-colors duration-300 ease-glaze hover:decoration-c600">
                    {CHECKSUMS_FILE}
                  </a>{' '}
                  里。
                </AfterRow>
              ) : null}
              <AfterRow label="卸载">
                Windows 在「设置 → 应用」里卸载。Mac 删掉 <Code>/Library/Audio/Plug-Ins/</Code> 下 Components 与 VST3 里的 Celadon，以及「应用程序」里的 Celadon。
              </AfterRow>
            </ul>
          </div>

          <div data-reveal className="mt-14 flex flex-wrap gap-x-10 gap-y-4">
            <LinkArrow to="/docs/install">安装与兼容说明</LinkArrow>
            <LinkArrow to="/changelog">这一版改了什么</LinkArrow>
          </div>
        </div>
      </section>
    </main>
  );
}

function PackageCard({ pkg, platform, badge }: { pkg: ReleasePackage; platform: Platform; badge: string | null }) {
  const featured = badge !== null;
  const href = downloadHref(pkg.file);
  // 两种状态的 DOM 结构完全一样，只换类名：异步猜出芯片后切换不会重建节点（入场动画的状态留在原节点上）。
  // 素坯卡也留同样 6px 的外框（透明），三张卡的瓷面上下左右对齐，瓷板只多出一圈托。
  return (
    <article id={platform.anchor} data-reveal aria-labelledby={`${platform.anchor}-title`} className={cn('scroll-mt-28 p-1.5', featured && 'slab')}>
      <div className={cn('@container h-full', featured ? 'slab-core' : 'biscuit')}>
        <div className="flex h-full flex-col gap-8 p-7 sm:p-8 @2xl:grid @2xl:grid-cols-2 @2xl:gap-12">
          <div>
            <div className="flex flex-wrap items-baseline justify-between gap-x-4 gap-y-2">
              <h2 id={`${platform.anchor}-title`} className="flex items-baseline gap-3">
                <span className="display text-[clamp(2rem,3vw,2.5rem)] leading-none text-ink">{platform.os}</span>{' '}
                <span className="font-serif text-[17px] text-ink-2">{platform.chip}</span>
              </h2>
              {featured ? (
                <p className="flex items-center gap-2 text-[12px] tracking-[0.12em] text-c700">
                  <span className="size-1.5 rounded-full bg-c500" aria-hidden="true" />
                  {badge}
                </p>
              ) : null}
            </div>
            <p className="mt-4 text-[14px] leading-relaxed text-ink-3">{platform.system}</p>
            <p className="mt-1 text-[14px] leading-relaxed text-ink-3">{platform.formats}</p>
            <div className="mt-8 flex flex-wrap items-center gap-x-6 gap-y-4">
              {/* 三张卡的按钮字一样，读屏时补上平台；下载图标悬停时往下沉 2px（其余链接的 ↗ 是斜移） */}
              {featured ? (
                <a href={href} download className="group btn-island-primary no-underline">
                  下载安装包
                  <span className="sr-only">（{platform.os} {platform.chip}）</span>
                  <span className="btn-island-icon bg-white/15 group-hover:translate-x-0 group-hover:translate-y-0.5">
                    <DownloadGlyph />
                  </span>
                </a>
              ) : (
                <a href={href} download className="link-arrow">
                  <span>
                    下载安装包
                    <span className="sr-only">（{platform.os} {platform.chip}）</span>
                  </span>
                  <DownloadGlyph className="[.link-arrow:hover>&]:[transform:translateY(2px)]" />
                </a>
              )}
              <Size bytes={pkg.bytes} />
            </div>
          </div>

          <div className="flex flex-1 flex-col">
            <dl className="mb-6 border-t border-line pt-5 text-[13px]">
              <div className="grid grid-cols-[4.5rem_1fr] items-baseline gap-3">
                <dt className="text-[12px] tracking-[0.2em] text-ink-4">文件</dt>
                <dd className="font-mono text-[11.5px] leading-[1.7] text-ink-2" translate="no">
                  <FileName name={pkg.file} />
                </dd>
              </div>
              <div className="mt-4">
                <dt className="flex items-center justify-between gap-3">
                  <span className="text-[12px] tracking-[0.2em] text-ink-4">SHA-256</span>
                  <CopyButton value={pkg.sha256} what={`${platform.os} ${platform.chip}安装包的 SHA-256`} />
                </dt>
                <dd className="mt-2">
                  <Hash value={pkg.sha256} />
                </dd>
              </div>
            </dl>
            <p className="mt-auto border-t border-line pt-5 text-[13px] leading-[1.75] text-ink-3">
              <span className="mr-2 text-[12px] tracking-[0.2em] text-ink-4">怎么认</span>
              {platform.how}
            </p>
          </div>
        </div>
      </div>
    </article>
  );
}

function StaleNotice() {
  return (
    <div data-reveal className="biscuit mt-16 max-w-3xl p-8 sm:p-10">
      <p className="font-serif text-[clamp(1.375rem,2.4vw,1.75rem)] font-medium leading-snug text-ink">安装包还没更新到 {PLUGIN_VERSION}。</p>
      <p className="mt-4 measure text-[15.5px] leading-[1.8] text-ink-2">新包放上来之前，这里不给旧版本的文件，免得装错。想先看看这一版改了什么：</p>
      <div className="mt-6">
        <LinkArrow to="/changelog">更新日志</LinkArrow>
      </div>
    </div>
  );
}

const MAC_STEPS: ReactNode[] = [
  <>双击 .pkg。系统说无法打开时，先关掉提示——别点<Ui>移到废纸篓</Ui>。</>,
  <>
    打开<Ui>系统设置 → 隐私与安全性</Ui>，往下找到 Celadon 那一条，点<Ui>仍要打开</Ui>，再输入密码。macOS 12 里这一项叫<Ui>系统偏好设置 → 安全性与隐私</Ui>。
  </>,
  <>安装器里点<Ui>自定</Ui>，可以只装需要的格式：VST3、AU、独立运行版。</>
];

const WIN_STEPS: ReactNode[] = [
  <>双击 .exe，出现<Ui>Windows 已保护你的电脑</Ui>。</>,
  <>
    点<Ui>更多信息</Ui>，再点<Ui>仍要运行</Ui>。
  </>,
  <>安装向导的<Ui>组件</Ui>页可以只勾 VST3；独立运行版默认也装，开始菜单里叫 Celadon。</>
];

function Steps({ title, steps }: { title: string; steps: ReactNode[] }) {
  return (
    <div data-reveal>
      <h3 className="display text-[28px] leading-none text-ink">{title}</h3>
      <ol className="rule-list mt-6">
        {steps.map((step, i) => (
          <li key={i} className="grid grid-cols-[2.5rem_1fr] items-baseline gap-3 py-5">
            <span className="font-mono text-[12px] tracking-wider text-c700" aria-hidden="true">
              {String(i + 1).padStart(2, '0')}
            </span>
            <p className="text-[15.5px] leading-[1.8] text-ink-2">{step}</p>
          </li>
        ))}
      </ol>
    </div>
  );
}

function AfterRow({ label, children }: { label: string; children: ReactNode }) {
  return (
    <li data-reveal className="grid gap-2 py-6 md:grid-cols-12 md:items-baseline md:gap-8">
      <p className="font-serif text-[17px] font-medium text-ink md:col-span-3">{label}</p>
      <p className="measure text-[15.5px] leading-[1.8] text-ink-2 md:col-span-9">{children}</p>
    </li>
  );
}

/** 章节标记去掉序号：细线 + 中文标签 + 英文斜体，给页内的小节用。 */
function Kicker({ label, en }: { label: string; en: string }) {
  return (
    <p className="flex items-center gap-4 text-ink-3">
      <span className="h-px w-12 bg-ink/20" aria-hidden="true" />
      <span className="text-[13px] tracking-[0.2em]">{label}</span>
      <span className="display text-[13px] italic text-ink-4" style={{ fontVariationSettings: '"opsz" 14, "SOFT" 50' }}>
        {en}
      </span>
    </p>
  );
}

/** 界面上的字（按钮名、菜单路径）：加引号、加深，一眼能对上屏幕。 */
function Ui({ children }: { children: ReactNode }) {
  return <span className="font-medium text-ink">「{children}」</span>;
}

function Code({ children }: { children: ReactNode }) {
  return (
    <code translate="no" className="rounded bg-c100/60 px-1.5 py-0.5 font-mono text-[0.86em] text-c800">
      {children}
    </code>
  );
}

/** 大小：1 MB = 10^6 字节（与 macOS 访达一致），一位小数；悬停看精确字节数。 */
function Size({ bytes }: { bytes: number }) {
  return (
    <span className="font-display text-[22px] leading-none tabular-nums text-ink" title={`${bytes.toLocaleString('zh-CN')} 字节`}>
      {(bytes / 1e6).toFixed(1)}
      <span className="ml-1 text-[0.45em] tracking-[0.12em] text-ink-3">MB</span>
    </span>
  );
}

/** 文件名在连字符后允许换行，窄卡片里不从单词中间断开。 */
function FileName({ name }: { name: string }) {
  const parts = name.split('-');
  return (
    <>
      {parts.map((part, i) => (
        <span key={i}>
          {part}
          {i < parts.length - 1 ? (
            <>
              -<wbr />
            </>
          ) : null}
        </span>
      ))}
    </>
  );
}

const HEX8 = /[0-9a-f]{8}/g;

/**
 * 64 位十六进制按 8 位一组排，每组定宽 9ch → 一行 4 组、两行排完（窄处自动 3 组一行）。
 * 组与组之间没有真空格：点一下全选、手动复制出来仍是连续的一串。
 */
function Hash({ value }: { value: string }) {
  const groups = value.match(HEX8) ?? [value];
  return (
    <code translate="no" title="点一下全选" className="block max-w-[36.5ch] select-all font-mono text-[11.5px] leading-[1.75] text-ink-2">
      {groups.map((g, i) => (
        <span key={i} className="inline-block w-[9ch]">
          {g}
        </span>
      ))}
    </code>
  );
}

function CopyButton({ value, what }: { value: string; what: string }) {
  const [state, setState] = useState<'idle' | 'done' | 'failed'>('idle');
  const timer = useRef(0);
  useEffect(() => () => window.clearTimeout(timer.current), []);

  const onClick = async () => {
    const ok = await copyText(value);
    setState(ok ? 'done' : 'failed');
    window.clearTimeout(timer.current);
    timer.current = window.setTimeout(() => setState('idle'), 2000);
  };

  return (
    <>
      <button
        type="button"
        onClick={onClick}
        aria-label={`复制 ${what}`}
        className={cn(
          'inline-flex items-center gap-1.5 rounded-full px-2 py-0.5 -mr-2 text-[12px] tracking-[0.12em] transition-colors duration-300 ease-glaze',
          state === 'done' ? 'text-c700' : state === 'failed' ? 'text-warn' : 'text-ink-3 hover:text-c700'
        )}
      >
        {state === 'done' ? <CheckGlyph /> : <CopyGlyph />}
        {state === 'done' ? '已复制' : state === 'failed' ? '复制失败' : '复制'}
      </button>
      <span className="sr-only" aria-live="polite">
        {state === 'done' ? '已复制' : state === 'failed' ? '复制失败' : ''}
      </span>
    </>
  );
}

/** 先用剪贴板接口；不在安全上下文（如内网 http 验收）或被拒绝时，退回选中文本框再复制。 */
async function copyText(text: string): Promise<boolean> {
  try {
    if (window.isSecureContext && navigator.clipboard) {
      await navigator.clipboard.writeText(text);
      return true;
    }
  } catch {
    // 落到下面的旧办法
  }
  const area = document.createElement('textarea');
  area.value = text;
  area.setAttribute('readonly', '');
  area.style.cssText = 'position:fixed;top:0;left:0;width:1px;height:1px;opacity:0;pointer-events:none';
  document.body.appendChild(area);
  area.select();
  let ok = false;
  try {
    ok = document.execCommand('copy');
  } catch {
    ok = false;
  }
  area.remove();
  return ok;
}

interface Guess {
  id: PackageId;
  /** false = 只知道是 Mac、不知道芯片（按多数给 Apple 芯片） */
  sure: boolean;
}

function badgeFor(guess: Guess): string {
  if (!guess.sure) return '多数 Mac 选这个';
  return guess.id === 'win-x64' ? '适合这台电脑' : '适合这台 Mac';
}

interface UADataLike {
  platform?: string;
  mobile?: boolean;
  getHighEntropyValues?: (hints: string[]) => Promise<{ architecture?: string }>;
}

function uaData(): UADataLike | undefined {
  return (navigator as unknown as { userAgentData?: UADataLike }).userAgentData;
}

/**
 * 猜这台电脑该用哪个包（只用来决定哪张卡是主角，三张都一直能下）。
 * Windows 直接认；Mac 先按「多数 Mac 是 Apple 芯片」给出，Chromium 系浏览器再异步问 CPU 架构改准；手机、平板、Linux 不猜。
 */
function guessFromUserAgent(): Guess | null {
  if (typeof navigator === 'undefined') return null;
  const data = uaData();
  if (data?.mobile) return null;
  const platform = data?.platform || navigator.platform || '';
  const ua = navigator.userAgent;
  if (/^Win/i.test(platform) || /Windows NT/.test(ua)) return { id: 'win-x64', sure: true };
  // iPad 的桌面版 Safari 也自称 Mac：多点触控就不是 Mac。
  const isMac = (/^Mac/i.test(platform) || /Macintosh/.test(ua)) && navigator.maxTouchPoints < 2;
  return isMac ? { id: 'mac-arm64', sure: false } : null;
}

function usePlatformGuess(): Guess | null {
  const [guess, setGuess] = useState(guessFromUserAgent);
  const macUnsure = guess !== null && !guess.sure;

  useEffect(() => {
    if (!macUnsure) return;
    const data = uaData();
    if (!data?.getHighEntropyValues) return;
    let alive = true;
    data.getHighEntropyValues(['architecture']).then(
      v => {
        if (!alive) return;
        if (v.architecture === 'arm') setGuess({ id: 'mac-arm64', sure: true });
        else if (v.architecture === 'x86') setGuess({ id: 'mac-x64', sure: true });
      },
      () => {}
    );
    return () => {
      alive = false;
    };
  }, [macUnsure]);

  return guess;
}

/** 页面标题、带 #锚点进来时滚到位置、字体到位后让滚动触发器重新量一遍位置。 */
function usePageSetup(title: string) {
  const { hash, key } = useLocation();

  useEffect(() => {
    const prev = document.title;
    document.title = title;
    return () => {
      document.title = prev;
    };
  }, [title]);

  // 平滑滚动会被字体到位后的 ScrollTrigger 重新测量打断（原来带锚点进来停在页首），由 lib/anchor 处理。
  useEffect(() => scrollToAnchor(hash), [hash, key]);

  // 字体换上后触发器位置会变，重新量（思源的声明是异步插入的，见 lib/fonts）。
  useEffect(() => onFontsLoaded(() => ScrollTrigger.refresh()), []);
}

function formatDate(iso: string): string {
  const [y, m, d] = iso.split('-');
  return `${y} 年 ${Number(m)} 月 ${Number(d)} 日`;
}

function DownloadGlyph({ className }: { className?: string }) {
  return (
    <svg viewBox="0 0 16 16" className={cn('size-3.5', className)} fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
      <path d="M8 2.5v8M4.5 7.5 8 11l3.5-3.5M3 13.5h10" />
    </svg>
  );
}

function CopyGlyph() {
  return (
    <svg viewBox="0 0 16 16" className="size-3" fill="none" stroke="currentColor" strokeWidth="1.4" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
      <rect x="5.5" y="5.5" width="8" height="8" rx="1.8" />
      <path d="M10.5 3.5v-.3A1.7 1.7 0 0 0 8.8 1.5H4.2a1.7 1.7 0 0 0-1.7 1.7v4.6a1.7 1.7 0 0 0 1.7 1.7h.3" />
    </svg>
  );
}

function CheckGlyph() {
  return (
    <svg viewBox="0 0 16 16" className="size-3" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
      <path d="m3.5 8.5 3 3 6-7" />
    </svg>
  );
}
