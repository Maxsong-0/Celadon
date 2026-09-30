import { cn } from '@/lib/utils';

/** 品牌标记：一枚豆青小瓷钮——开片、边缘积釉、中心浅凹与刻线，指针是边缘的短槽（与首屏旋钮同一语言，不用深色圆帽）。 */
export default function Mark({ className }: { className?: string }) {
  return (
    <svg viewBox="0 0 64 64" className={cn('block', className)} aria-hidden="true">
      <defs>
        <radialGradient id="mark-glaze" cx="36%" cy="30%" r="80%">
          <stop offset="0" stopColor="#F1F5F0" />
          <stop offset="0.55" stopColor="#B4CBC0" />
          <stop offset="1" stopColor="#5C8579" />
        </radialGradient>
      </defs>
      <circle cx="32" cy="32" r="30" fill="url(#mark-glaze)" />
      <path d="M13 38 L22 30 L20 19 M40 14 L44 26 L39 37 L49 44 M24 50 L31 42" stroke="#2F564C" strokeOpacity=".45" strokeWidth="1.1" fill="none" strokeLinecap="round" />
      <circle cx="32" cy="32" r="8.5" fill="#E4ECE6" fillOpacity=".75" stroke="#3D6258" strokeOpacity=".4" strokeWidth=".9" />
      <path d="M32 5.5 V12" stroke="#2F564C" strokeWidth="2.4" strokeLinecap="round" />
      <circle cx="32" cy="32" r="29.2" fill="none" stroke="#3D6258" strokeOpacity=".35" strokeWidth="1.2" />
    </svg>
  );
}
