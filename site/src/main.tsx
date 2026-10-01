import { StrictMode, Suspense } from 'react';
import { createRoot } from 'react-dom/client';
import { BrowserRouter } from 'react-router-dom';
import App from './App';
import './i18n';
import { localeFromPath } from './i18n/paths';
import { loadCjkFonts } from './lib/fonts';
// 自托管字体：Fraunces 全轴（opsz / SOFT / WONK）、JetBrains Mono、英文正文的 Inter 在首屏样式表里；
// 思源宋体、思源黑体的声明约 200 KB，只在中文页由 lib/fonts 异步插入，不挡首次渲染（ADR-024）。
import '@fontsource-variable/fraunces/full.css';
import '@fontsource-variable/fraunces/full-italic.css';
import '@fontsource-variable/jetbrains-mono';
import '@fontsource-variable/inter';
import './index.css';

if (localeFromPath(location.pathname) === 'zh') void loadCjkFonts();

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <BrowserRouter basename={import.meta.env.BASE_URL.replace(/\/$/, '')}>
      {/* 进站第一屏要等 common 与本页的界面短句（i18n/index.ts 初始化时已并行在拉），通常是一个很小的请求 */}
      <Suspense fallback={null}>
        <App />
      </Suspense>
    </BrowserRouter>
  </StrictMode>
);
