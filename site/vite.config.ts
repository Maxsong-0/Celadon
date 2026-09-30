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
    rolldownOptions: {
      output: {
        // 把大依赖拆开，首屏只拉用到的（Rolldown 的 codeSplitting；manualChunks 在 Vite 8 已弃用）。
        // 每组默认连同依赖一起收进本包，所以 React 必须单独成组、优先级最高：否则 react 会被 @gsap/react 带进 gsap 包，
        // react/jsx-runtime 会被 react-markdown 带进 markdown 包——入口一用 JSX 就得先下整个 Markdown 解析器（改前首页因此多拉 46 KB gzip）。
        codeSplitting: {
          groups: [
            // 框架：几乎不变，单独一包缓存最久。
            { name: 'react', test: /node_modules[\\/](react|react-dom|scheduler|react-router|react-router-dom)[\\/]/, priority: 30 },
            // 动画引擎（全站唯一）：GSAP 核心、ScrollTrigger、SplitText、useGSAP。
            { name: 'gsap', test: /node_modules[\\/](gsap|@gsap[\\/]react)[\\/]/, priority: 20 },
            // WebGL：只有首屏等高线（懒加载）用。
            { name: 'ogl', test: /node_modules[\\/]ogl[\\/]/, priority: 20 },
            // Markdown：unified 一族随这两个入口的依赖一并收进来，只有说明页与算法页用。
            { name: 'markdown', test: /node_modules[\\/](react-markdown|remark-gfm)[\\/]/, priority: 10 }
          ]
        }
      }
    }
  }
});
