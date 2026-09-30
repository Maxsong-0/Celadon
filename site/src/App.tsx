import { lazy, Suspense, useEffect } from 'react';
import { Navigate, Route, Routes, useLocation } from 'react-router-dom';
import Nav from '@/components/celadon/Nav';
import Footer from '@/components/celadon/Footer';
import Noise from '@/components/react-bits/Noise';
import ClickSpark from '@/components/react-bits/ClickSpark';
import Home from '@/pages/Home';
import { prefersReducedMotion } from '@/lib/utils';

const DocsLayout = lazy(() => import('@/pages/DocsLayout'));

function ScrollToTop() {
  const { pathname, hash } = useLocation();
  useEffect(() => {
    if (!hash) window.scrollTo({ top: 0, left: 0, behavior: 'instant' as ScrollBehavior });
  }, [pathname, hash]);
  return null;
}

export default function App() {
  const reduced = prefersReducedMotion();
  return (
    <ClickSpark sparkColor="#1799AB" sparkSize={9} sparkRadius={18} sparkCount={8} duration={420}>
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
        <Route path="*" element={<NotFound />} />
      </Routes>
      <Footer />
      {reduced ? null : (
        <div className="noise-fixed" aria-hidden="true">
          <Noise patternRefreshInterval={4} patternAlpha={10} />
        </div>
      )}
    </ClickSpark>
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
        <p className="mt-2 text-ink-3 text-sm">回到<a href="/" className="text-c700 underline decoration-c300 ml-1">首页</a>，或看看<a href="/docs" className="text-c700 underline decoration-c300 ml-1">使用说明</a>。</p>
      </div>
    </main>
  );
}
