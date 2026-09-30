import type { ReactNode } from 'react';
import { cn } from '@/lib/utils';

export function Eyebrow({ children, className }: { children: ReactNode; className?: string }) {
  return (
    <p className={cn('eyebrow', className)}>
      <span className="eyebrow-dot" />
      {children}
    </p>
  );
}

export function SectionHead({
  eyebrow,
  title,
  lead,
  align = 'left',
  className
}: {
  eyebrow: ReactNode;
  title: ReactNode;
  lead?: ReactNode;
  align?: 'left' | 'center';
  className?: string;
}) {
  return (
    <div className={cn('max-w-3xl', align === 'center' && 'mx-auto text-center', className)}>
      <Eyebrow className={cn(align === 'center' && 'mx-auto')}>{eyebrow}</Eyebrow>
      <h2 className="mt-6 font-serif text-[clamp(2rem,4.2vw,3.5rem)] font-medium leading-[1.12] tracking-tight text-ink">{title}</h2>
      {lead ? <p className="mt-6 text-[17px] leading-[1.8] text-ink-2">{lead}</p> : null}
    </div>
  );
}
