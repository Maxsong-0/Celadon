import type { ReactNode } from 'react';
import { LocaleLink } from '@/i18n/locale';
import { cn } from '@/lib/utils';

/** 小胶囊标签：只留给说明页侧栏这类小场合；首页各节用 <Chapter>（设计规格-官网 第 1、3 节）。 */
export function Eyebrow({ children, className }: { children: ReactNode; className?: string }) {
  return (
    <p className={cn('eyebrow', className)}>
      <span className="eyebrow-dot" />
      {children}
    </p>
  );
}

export interface ChapterProps {
  /** 中文大写序号：壹 贰 叁 肆 伍 陆 柒 捌 */
  n: string;
  label: string;
  /** 标签下的英文斜体；英文页传空串（标签本身就是英文） */
  en: string;
  align?: 'left' | 'center';
  className?: string;
}

/**
 * 章节标记：衬线大写数字 + 细线 + 标签 + 英文斜体。像书的章首，不像按钮。
 * 中文页：中文标签 + 英文斜体；英文页：英文标签（大写）、斜体为空不画。大写序号「壹贰叁」两种语言都保留（品牌装饰，Max 2026-10-01 定）。
 */
export function Chapter({ n, label, en, align = 'left', className }: ChapterProps) {
  return (
    <p className={cn('flex items-center gap-4 text-ink-3', align === 'center' && 'justify-center', className)}>
      <span className="font-serif text-[28px] leading-none text-c600" aria-hidden="true" lang="zh-CN">
        {n}
      </span>
      <span className="h-px w-12 bg-ink/20" aria-hidden="true" />
      <span className="text-[13px] tracking-[0.2em] [&:lang(en)]:uppercase">{label}</span>
      {en ? (
        <span className="display text-[13px] italic text-ink-4" style={{ fontVariationSettings: '"opsz" 14, "SOFT" 50' }}>
          {en}
        </span>
      ) : null}
    </p>
  );
}

export function SectionHead({
  chapter,
  title,
  lead,
  align = 'left',
  className
}: {
  chapter: Omit<ChapterProps, 'align' | 'className'>;
  title: ReactNode;
  lead?: ReactNode;
  align?: 'left' | 'center';
  className?: string;
}) {
  return (
    <div className={cn('max-w-3xl', align === 'center' && 'mx-auto text-center', className)}>
      <Chapter {...chapter} align={align} />
      <h2 className="mt-8 font-serif text-[clamp(2rem,4.2vw,3.5rem)] font-medium leading-[1.12] tracking-tight text-ink">{title}</h2>
      {lead ? <p className={cn('mt-6 measure text-[17px] leading-[1.85] text-ink-2', align === 'center' && 'mx-auto')}>{lead}</p> : null}
    </div>
  );
}

/** 标题里的强调词：实色釉青，不用渐变（渐变字全站只留首屏一处）。 */
export function Em({ children }: { children?: ReactNode }) {
  return <span className="text-c700">{children}</span>;
}

export function ArrowUpRight({ className }: { className?: string }) {
  return (
    <svg viewBox="0 0 16 16" className={cn('size-3.5', className)} fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
      <path d="M4 12 12 4M6 4h6v6" />
    </svg>
  );
}

/** 次要动作：文字链接 + 斜箭头（主按钮一处只有一个，其余用它）。站内路径走路由，其余走 <a>。 */
export function LinkArrow({ to, children, className }: { to: string; children: ReactNode; className?: string }) {
  const inner = (
    <>
      <span>{children}</span>
      <ArrowUpRight />
    </>
  );
  // 站内路径（不带语言前缀）按当前语言补 /zh；站外地址原样
  if (to.startsWith('/')) {
    return (
      <LocaleLink to={to} className={cn('link-arrow', className)}>
        {inner}
      </LocaleLink>
    );
  }
  return (
    <a href={to} className={cn('link-arrow', className)}>
      {inner}
    </a>
  );
}
