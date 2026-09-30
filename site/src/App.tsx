import { lazy, Suspense, useEffect } from 'react';
import { Navigate, Route, Routes, useLocation } from 'react-router-dom';
import Nav from '@/components/celadon/Nav';
import Footer from '@/components/celadon/Footer';
import Home from '@/pages/Home';

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

// 纸纹在 index.css 的 body::before（静态）；不再有全站点击火花与每帧重绘的噪点（设计规格-官网 第 1 节第 6 条）。
export default function App() {
  return (
    <>
      <ScrollToTop />
      <Nav />
      <Routes>
        <Route path="/" element={<Home />} />
        <Route
          path="/docs/*"
          element={
            <Suspense fallback={<PageFallback />}>
              <DocsLayout kind="docs" />
            </Suspense>
          }
        />
        <Route path="/algorithms/references" element={<Navigate to="/algorithms" replace />} />
        <Route
          path="/algorithms/*"
          element={
            <Suspense fallback={<PageFallback />}>
              <DocsLayout kind="algorithms" />
            </Suspense>
          }
        />
        <Route
          path="/download"
          element={
            <Suspense fallback={<PageFallback />}>
              <Download />
            </Suspense>
          }
        />
        <Route
          path="/changelog"
          element={
            <Suspense fallback={<PageFallback />}>
              <Changelog />
            </Suspense>
          }
        />
        <Route path="*" element={<NotFound />} />
      </Routes>
      <Footer />
    </>
  );
}

function PageFallback() {
  return (
    <div className="min-h-[60dvh] grid place-items-center pt-32">
      <div className="flex items-center gap-3 text-[13px] text-ink-4">
        <span className="size-2 rounded-full bg-c400 animate-pulse" />
        正在开窑……
      </div>
    </div>
  );
}

function NotFound() {
  return (
    <main className="min-h-[70dvh] grid place-items-center pt-32 px-6 text-center">
      <div>
        <div className="display text-[96px] leading-none text-c300">404</div>
        <p className="mt-4 font-serif text-2xl text-ink">这一页没烧出来。</p>
        <p className="mt-2 text-ink-3 text-sm">
          回到<a href="/" className="text-c700 underline decoration-c300 ml-1">首页</a>，或看看<a href="/docs" className="text-c700 underline decoration-c300 ml-1">使用说明</a>。
        </p>
      </div>
    </main>
  );
}
