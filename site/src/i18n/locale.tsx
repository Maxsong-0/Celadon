import { useCallback, useEffect, useLayoutEffect, type ComponentProps } from 'react';
import { Link, useLocation, useNavigate } from 'react-router-dom';
import i18n from './index';
import { HTML_LANG, localeFromPath, localizePath, saveLocale, savedLocale, stripLocale, type Locale } from './paths';
import { loadCjkFonts } from '@/lib/fonts';

/** 当前语言（由网址决定）。 */
export function useLocale(): Locale {
  return localeFromPath(useLocation().pathname);
}

/** 把不带语言的站内路径换成当前语言的：useLocalePath()('/docs') → /zh/docs（中文时）。 */
export function useLocalePath(): (to: string) => string {
  const locale = useLocale();
  return useCallback((to: string) => localizePath(to, locale), [locale]);
}

type LocaleLinkProps = Omit<ComponentProps<typeof Link>, 'to'> & { to: string };

/** 站内链接一律用它：路径写不带语言的（/docs/install），按当前语言补 /zh。 */
export function LocaleLink({ to, ...rest }: LocaleLinkProps) {
  const localize = useLocalePath();
  return <Link to={localize(to)} {...rest} />;
}

/**
 * 路由与 i18next、<html lang>、中文字体保持一致；挂在 App 最外层。
 * 存过「中文」的人从英文地址进来（比如别人发的链接），首次挂载时换到对应的中文页；没存过的不跳（ADR-024）。
 */
export function LocaleSync() {
  const { pathname, search, hash } = useLocation();
  const locale = localeFromPath(pathname);
  const navigate = useNavigate();

  useEffect(() => {
    if (locale === 'en' && savedLocale() === 'zh') navigate(localizePath(pathname, 'zh') + search + hash, { replace: true });
    // 只在进站时判断一次（依赖留空是有意的）：之后用户在站内点了英文链接就听用户的
  }, []);

  // 在绘制前切语言，换路由的那一帧不先闪一下旧语言（新语言的文案没到时由外层 Suspense 接住）
  useLayoutEffect(() => {
    document.documentElement.lang = HTML_LANG[locale];
    if (locale === 'zh') void loadCjkFonts();
    if (i18n.resolvedLanguage !== locale) void i18n.changeLanguage(locale);
  }, [locale]);

  return null;
}

/** 导航里的语言开关：记住选择，先把另一种语言的文案拉好再换到同一页（去掉锚点与查询串，两种语言的标题锚点不同）。 */
export function useSwitchLocale(): () => void {
  const { pathname } = useLocation();
  const navigate = useNavigate();
  const locale = localeFromPath(pathname);
  return useCallback(() => {
    const target: Locale = locale === 'zh' ? 'en' : 'zh';
    saveLocale(target);
    if (target === 'zh') void loadCjkFonts();
    void i18n.changeLanguage(target).then(
      () => navigate(localizePath(stripLocale(pathname), target)),
      () => navigate(localizePath(stripLocale(pathname), target))
    );
  }, [locale, pathname, navigate]);
}
