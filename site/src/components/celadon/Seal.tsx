import { cn } from '@/lib/utils';

/** 一枚朱文印章：「青瓷 · 施釉」。窑口落款的意思，只做装饰。 */
export default function Seal({ className, text = ['青瓷', '施釉'] }: { className?: string; text?: [string, string] }) {
  return (
    <div className={cn('inline-grid size-14 place-items-center rounded-[6px] bg-seal text-paper shadow-[inset_0_0_0_1.5px_rgba(243,241,234,0.85),inset_0_0_0_3px_#b8412e]', className)} aria-hidden="true">
      <div className="grid grid-cols-2 gap-x-1 font-serif text-[15px] font-semibold leading-[1.05] tracking-tight [writing-mode:vertical-rl]">
        <span>{text[1]}</span>
        <span>{text[0]}</span>
      </div>
    </div>
  );
}
