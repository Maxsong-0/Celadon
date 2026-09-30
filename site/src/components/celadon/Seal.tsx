import { cn } from '@/lib/utils';

/** 一枚朱文印章：右列「青瓷」、左列「施釉」，各自竖排、自上而下读。窑口落款的意思，只做装饰。 */
export default function Seal({ className, text = ['青瓷', '施釉'] }: { className?: string; text?: [string, string] }) {
  return (
    <div
      className={cn('inline-flex size-14 flex-row-reverse items-center justify-center gap-1 rounded-[6px] bg-seal text-paper shadow-[inset_0_0_0_1.5px_rgba(243,241,234,0.85),inset_0_0_0_3px_#b8412e]', className)}
      aria-hidden="true"
    >
      {text.map(t => (
        <span key={t} className="font-serif text-[15px] font-semibold leading-none tracking-[0.05em] [writing-mode:vertical-rl] [text-orientation:upright]">
          {t}
        </span>
      ))}
    </div>
  );
}
