import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import { BrowserRouter } from 'react-router-dom';
import App from './App';
// 自托管字体：Fraunces 全轴（opsz / SOFT / WONK）、思源宋体、思源黑体、JetBrains Mono
import '@fontsource-variable/fraunces/full.css';
import '@fontsource-variable/fraunces/full-italic.css';
import '@fontsource-variable/noto-serif-sc';
import '@fontsource-variable/noto-sans-sc';
import '@fontsource-variable/jetbrains-mono';
import './index.css';

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <BrowserRouter basename={import.meta.env.BASE_URL.replace(/\/$/, '')}>
      <App />
    </BrowserRouter>
  </StrictMode>
);
