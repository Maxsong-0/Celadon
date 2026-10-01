import type { Locale } from '@/i18n/paths';

/** YYYY-MM-DD → 中文「2026 年 10 月 1 日」（站内排版习惯：数字两侧留空）/ 英文「Oct 1, 2026」。按 UTC 解析，不受时区影响。 */
export function formatDate(iso: string, locale: Locale): string {
  const [y, m, d] = iso.split('-').map(Number);
  if (locale === 'zh') return `${y} 年 ${m} 月 ${d} 日`;
  return new Intl.DateTimeFormat('en-US', { year: 'numeric', month: 'short', day: 'numeric', timeZone: 'UTC' }).format(Date.UTC(y, m - 1, d));
}

/** 数字的本地写法（千分位）：中文与英文都用逗号分组。 */
export function formatNumber(n: number, locale: Locale): string {
  return n.toLocaleString(locale === 'zh' ? 'zh-CN' : 'en-US');
}
