# React Bits（免费版）组件拷贝

来源：<https://reactbits.dev>（GitHub `DavidHDev/react-bits`，`src/ts-tailwind/**`），2026-09-29 拷入。
许可：**MIT + Commons Clause License Condition v1.0**——允许作为网站 / 产品的一部分使用、修改、分发；**不允许**把组件本身单独出售、再授权或再分发（含打包或移植版）。每个文件头都保留了版权与许可说明。

| 文件 | 用在哪 | Celadon 改动 |
|---|---|---|
| `SplitText.tsx` | 首屏标题逐字浮现（GSAP SplitText + ScrollTrigger） | 从 `@/lib/gsap` 取已注册的 gsap；加 `force3D` 属性（渐变字要关 3D 合成层）；渐变字逐字各裁一段同一渐变（Chrome 不把逐字图层算进父元素的 background-clip: text）；动画结束清 transform；只等本段文字的字体子集而不是整页 fonts.ready |
| `Magnet.tsx` | 按钮磁吸 | 无 |
| `Topography.tsx` | 首屏釉面等高线（OGL WebGL） | 无 |

没有 Pro 许可（环境里没有 `REACTBITS_LICENSE_KEY`），不用 Pro 的 blocks。

## 已删除（2026-09-30，T-033）

全站动画只用 GSAP 一套引擎（设计规格-官网 第 1 节第 6 条），下面四个不再用，已从仓库删掉，站点也不再依赖 `motion`（framer-motion）：

| 文件 | 原来用在哪 | 现在 |
|---|---|---|
| `ScrollVelocity.tsx` | 模块名跑马灯（motion） | 站内自写的 GSAP 实现：`components/celadon/Marquee.tsx`，不含 React Bits 代码 |
| `CountUp.tsx` | 数字段的计数（motion 弹簧） | 站内自写的 GSAP 实现：`components/celadon/Stats.tsx`，不含 React Bits 代码 |
| `ClickSpark.tsx` | 全站点击迸溅 | 不做（规范：不做点击火花） |
| `Noise.tsx` | 全站宣纸颗粒（每帧重绘的 canvas） | 改为 `index.css` 里 `body::before` 的静态 SVG 噪点 |
