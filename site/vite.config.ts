import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import tailwindcss from '@tailwindcss/vite';

// 静态站：产物 dist/ 可放任何静态托管（Cloudflare Pages / GitHub Pages / 自建 nginx）。
// 部署到子路径时用环境变量 SITE_BASE（如 /celadon/）。
export default defineConfig({
  base: process.env.SITE_BASE ?? '/',
  plugins: [react(), tailwindcss()],
  resolve: {
    alias: { '@': new URL('./src', import.meta.url).pathname }
  },
  build: {
    target: 'es2022',
    sourcemap: false,
    rollupOptions: {
      output: {
        // 把大依赖拆开：GSAP、WebGL（ogl）、motion、Markdown 各自一包，首屏只拉用到的（Vite 8 / rolldown 要函数形式）。
        manualChunks(id: string) {
          if (!id.includes('node_modules')) return undefined;
          if (/node_modules\/(gsap|@gsap)\//.test(id)) return 'gsap';
          if (/node_modules\/ogl\//.test(id)) return 'ogl';
          if (/node_modules\/(motion|motion-dom|motion-utils|framer-motion)\//.test(id)) return 'motion';
          if (/node_modules\/(react-markdown|remark-|mdast-|micromark|unified|unist-|hast-|vfile|property-information|comma-separated-tokens|space-separated-tokens|devlop|estree-|html-url-attributes|bail|trough|is-plain-obj|decode-named-character-reference|character-entities|ccount|markdown-table|longest-streak|zwitch|trim-lines|escape-string-regexp|style-to-js|style-to-object|inline-style-parser)/.test(id)) return 'markdown';
          return undefined;
        }
      }
    }
  }
});
