import { cn } from '@/lib/utils';

/** 品牌标记：一枚带开片的小青瓷旋钮。 */
export default function Mark({ className }: { className?: string }) {
  return (
    <svg viewBox="0 0 64 64" className={cn('block', className)} aria-hidden="true">
      <defs>
        <radialGradient id="mark-glaze" cx="40%" cy="35%" r="70%">
          <stop offset="0" stopColor="#D5F1F5" />
          <stop offset="0.55" stopColor="#67CAD7" />
          <stop offset="1" stopColor="#0E6473" />
        </radialGradient>
      </defs>
      <circle cx="32" cy="32" r="30" fill="url(#mark-glaze)" />
      <path d="M18 40 L26 30 L23 20 M38 18 L43 28 L39 38 L47 45 M27 48 L34 41" stroke="#F3F1EA" strokeOpacity=".75" strokeWidth="1.4" fill="none" strokeLinecap="round" />
      <circle cx="32" cy="32" r="7" fill="#0A2E36" />
      <path d="M32 25.5 V29" stroke="#D5F1F5" strokeWidth="1.6" strokeLinecap="round" />
    </svg>
  );
}
