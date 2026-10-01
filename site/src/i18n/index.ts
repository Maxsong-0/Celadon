import i18n from 'i18next';
import { initReactI18next } from 'react-i18next';
import resourcesToBackend from 'i18next-resources-to-backend';
import { localeFromPath, pageNamespaces, type Locale } from './paths';

/*
  界面短句：src/i18n/locales/<语言>/<命名空间>.json，按语言懒加载（英文首屏不下载中文文案）。
  语言只由网址决定（/zh/… 中文，其余英文；ADR-024），不用浏览器语言检测：初始化时按当前路径定，之后由 LocaleSync 跟路由切换。
  初始化同时并行拉 common 与这一页的命名空间；其余命名空间在组件第一次用到时拉（useTranslation 挂起，外层 Suspense 接住）。
*/
const initial: Locale = typeof location === 'undefined' ? 'en' : localeFromPath(location.pathname);
const firstPage = typeof location === 'undefined' ? [] : pageNamespaces(location.pathname);

void i18n
  .use(resourcesToBackend((lng: string, ns: string) => import(`./locales/${lng}/${ns}.json`)))
  .use(initReactI18next)
  .init({
    lng: initial,
    fallbackLng: 'en',
    supportedLngs: ['en', 'zh'],
    load: 'currentOnly',
    ns: ['common', ...firstPage],
    defaultNS: 'common',
    interpolation: { escapeValue: false }, // React 自己转义
    react: { useSuspense: true }
  });

export default i18n;
