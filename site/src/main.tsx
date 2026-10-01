import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import { BrowserRouter } from 'react-router-dom';
import App from './App';
// 自托管字体：Fraunces 全轴（opsz / SOFT / WONK）、JetBrains Mono 在首屏样式表里；
// 思源宋体、思源黑体的声明约 200 KB，由 lib/fonts 异步插入，不挡首次渲染。
import '@fontsource-variable/fraunces/full.css';
import '@fontsource-variable/fraunces/full-italic.css';
import '@fontsource-variable/jetbrains-mono';
import './lib/fonts';
import './index.css';

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <BrowserRouter basename={import.meta.env.BASE_URL.replace(/\/$/, '')}>
      <App />
    </BrowserRouter>
  </StrictMode>
);
