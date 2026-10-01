/**
 * 语言与路径（纯函数，不依赖 React / i18next）：英文在 /，中文在 /zh/（ADR-024）。
 * 页面组件只写不带语言的路径（/docs/install），由 LocaleLink / useLocalePath 按当前语言补上 /zh。
 */
export type Locale = 'en' | 'zh';

export const NAMESPACES = ['common', 'home', 'download', 'changelog', 'docs', 'figures'] as const;
export type Namespace = (typeof NAMESPACES)[number];

/** <html lang> 与 hreflang 用的语言标签 */
export const HTML_LANG: Record<Locale, string> = { en: 'en', zh: 'zh-CN' };

// /zh 之后只能是路径结束、下一段、锚点或查询串：/zhuyin 这种不是中文前缀。
const ZH_PREFIX = /^\/zh(?=[/?#]|$)/;

export function localeFromPath(pathname: string): Locale {
  return ZH_PREFIX.test(pathname) ? 'zh' : 'en';
}

/** 去掉语言前缀：/zh/docs → /docs，/zh → / */
export function stripLocale(pathname: string): string {
  const rest = pathname.replace(ZH_PREFIX, '');
  return rest === '' || rest.startsWith('?') || rest.startsWith('#') ? `/${rest}` : rest;
}

/** 给站内路径补语言前缀；站外地址、相对地址、已带前缀的原样返回。 */
export function localizePath(to: string, locale: Locale): string {
  if (locale === 'en' || !to.startsWith('/') || to.startsWith('//') || ZH_PREFIX.test(to)) return to;
  if (to === '/') return '/zh';
  if (to.startsWith('/#') || to.startsWith('/?')) return `/zh${to.slice(1)}`;
  return `/zh${to}`;
}

/** 首屏要用的界面短句命名空间（与 common 一起在初始化时并行拉，少一轮等待）。 */
export function pageNamespace(pathname: string): Namespace {
  const p = stripLocale(pathname);
  if (p === '/') return 'home';
  if (p.startsWith('/download')) return 'download';
  if (p.startsWith('/changelog')) return 'changelog';
  if (p.startsWith('/docs') || p.startsWith('/algorithms')) return 'docs';
  return 'common';
}

const LANG_KEY = 'celadon.lang';
const HINT_KEY = 'celadon.langHint';

/** 用户自己选过的语言（导航里的开关）；没选过或存储不可用时为 null。 */
export function savedLocale(): Locale | null {
  try {
    const v = localStorage.getItem(LANG_KEY);
    return v === 'en' || v === 'zh' ? v : null;
  } catch {
    return null;
  }
}

export function saveLocale(locale: Locale): void {
  try {
    localStorage.setItem(LANG_KEY, locale);
  } catch {
    // 隐私模式等：不记也照常切换
  }
}

export function hintDismissed(): boolean {
  try {
    return localStorage.getItem(HINT_KEY) === 'off';
  } catch {
    return false;
  }
}

export function dismissHint(): void {
  try {
    localStorage.setItem(HINT_KEY, 'off');
  } catch {
    // 同上
  }
}
