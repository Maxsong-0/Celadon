import { existsSync, readFileSync } from 'node:fs';
import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import tailwindcss from '@tailwindcss/vite';

// 静态站：产物 dist/ 可放任何静态托管（Cloudflare Pages / GitHub Pages / 自建 nginx）。

// 插件版本号的唯一真源是仓库根的 Celadon.jucer（ADR-010）。build-linux.sh 只把 site/ 同步到服务器，
// 所以它在本机读好 jucer、经 CELADON_VERSION 传过去；在仓库里直接构建时读 ../Celadon.jucer。两处都没有就构建失败，不用默认值。
function pluginVersion(): string {
  const fromEnv = process.env.CELADON_VERSION?.trim();
  if (fromEnv) return fromEnv;
  const jucer = new URL('../Celadon.jucer', import.meta.url);
  if (existsSync(jucer)) {
    const m = readFileSync(jucer, 'utf8').match(/<JUCERPROJECT[^>]*\sversion="([0-9.]+)"/);
    if (m) return m[1];
  }
  throw new Error('读不到插件版本号：设环境变量 CELADON_VERSION，或在仓库里构建（读 ../Celadon.jucer）');
}

export default defineConfig({
  // 部署到子路径时用环境变量 SITE_BASE（如 /celadon/）。
  base: process.env.SITE_BASE ?? '/',
  define: { __CELADON_VERSION__: JSON.stringify(pluginVersion()) },
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
