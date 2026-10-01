import { lazy, Suspense, useEffect } from 'react';
import { Navigate, Route, Routes, useLocation } from 'react-router-dom';
import { Trans, useTranslation } from 'react-i18next';
import Nav from '@/components/celadon/Nav';
import Footer from '@/components/celadon/Footer';
import LangHint from '@/components/celadon/LangHint';
import Home from '@/pages/Home';
import { LocaleLink, LocaleSync, useLocale, useLocalePath } from '@/i18n/locale';

const DocsLayout = lazy(() => import('@/pages/DocsLayout'));
const Download = lazy(() => import('@/pages/Download'));
const Changelog = lazy(() => import('@/pages/Changelog'));

function ScrollToTop() {
  const { pathname, hash } = useLocation();
  useEffect(() => {
    if (!hash) window.scrollTo({ top: 0, left: 0, behavior: 'instant' as ScrollBehavior });
  }, [pathname, hash]);
  return null;
}

/**
 * 两种语言共用同一组页面（ADR-024）：英文挂在根下，中文挂在 /zh 下；页面组件不感知前缀，只读当前语言。
 * 路径都是相对的（docs/* 等），由外层的 / 或 /zh 决定落在哪种语言。
 */
function pageRoutes() {
  return (
    <>
      <Route index element={<Home />} />
      <Route
        path="docs/*"
        element={
          <Suspense fallback={<PageFallback />}>
            <DocsLayout kind="docs" />
          </Suspense>
        }
      />
      <Route path="algorithms/references" element={<LocalizedRedirect to="/algorithms" />} />
      <Route
        path="algorithms/*"
        element={
          <Suspense fallback={<PageFallback />}>
            <DocsLayout kind="algorithms" />
          </Suspense>
        }
      />
      <Route
        path="download"
        element={
          <Suspense fallback={<PageFallback />}>
            <Download />
          </Suspense>
        }
      />
      <Route
        path="changelog"
        element={
          <Suspense fallback={<PageFallback />}>
            <Changelog />
          </Suspense>
        }
      />
      <Route path="*" element={<NotFound />} />
    </>
  );
}

// 纸纹在 index.css 的 body::before（静态）；不再有全站点击火花与每帧重绘的噪点（设计规格-官网 第 1 节第 6 条）。
export default function App() {
  return (
    <>
      <LocaleSync />
      <ScrollToTop />
      <Nav />
      <LangHint />
      {/* 某页的界面短句第一次用到时要拉一下（按语言懒加载），这时只让页面区域显示占位，导航与页脚不动 */}
      <Suspense fallback={<PageFallback />}>
        <LocaleRoutes />
      </Suspense>
      <Footer />
    </>
  );
}

/**
 * 同一页的两种语言按两棵树渲染（key = 语言）：切语言时整页重挂。
 * 否则 / 与 /zh 下同一位置的页面组件会被复用，只在挂载时按语言定下的东西停在旧语言——
 * 首屏标题按字 / 按词拆分、真界面截图的默认语言、钉住滚动按旧文字量出的位置。站内普通跳转不受影响（同语言 key 不变）。
 */
function LocaleRoutes() {
  const locale = useLocale();
  return (
    <Routes key={locale}>
      <Route path="/zh">{pageRoutes()}</Route>
      <Route path="/">{pageRoutes()}</Route>
    </Routes>
  );
}

function LocalizedRedirect({ to }: { to: string }) {
  const localize = useLocalePath();
  return <Navigate to={localize(to)} replace />;
}

function PageFallback() {
  const { t } = useTranslation();
  return (
    <div className="min-h-[60dvh] grid place-items-center pt-32">
      <div className="flex items-center gap-3 text-[13px] text-ink-4">
        <span className="size-2 rounded-full bg-c400 animate-pulse" />
        {t('pageLoading')}
      </div>
    </div>
  );
}

function NotFound() {
  const { t } = useTranslation();
  const link = 'text-c700 underline decoration-c300 mx-1';
  return (
    <main className="min-h-[70dvh] grid place-items-center pt-32 px-6 text-center">
      <div>
        <div className="display text-[96px] leading-none text-c300">404</div>
        <p className="mt-4 font-serif text-2xl text-ink">{t('notFound.title')}</p>
        <p className="mt-2 text-ink-3 text-sm">
          <Trans i18nKey="notFound.body" components={{ home: <LocaleLink to="/" className={link} />, docs: <LocaleLink to="/docs" className={link} /> }} />
        </p>
      </div>
    </main>
  );
}
