# React Bits（免费版）组件拷贝

来源：<https://reactbits.dev>（GitHub `DavidHDev/react-bits`，`src/ts-tailwind/**`），2026-09-29 拷入。
许可：**MIT + Commons Clause License Condition v1.0**——允许作为网站 / 产品的一部分使用、修改、分发；**不允许**把组件本身单独出售、再授权或再分发（含打包或移植版）。每个文件头都保留了版权与许可说明。

| 文件 | 用在哪 | Celadon 改动 |
|---|---|---|
| `SplitText.tsx` | 首屏标题逐字浮现（GSAP SplitText + ScrollTrigger） | 从 `@/lib/gsap` 取已注册的 gsap，不再自己 registerPlugin |
| `CountUp.tsx` | 数字段（motion 弹簧计数） | 无 |
| `AnimatedContent.tsx` | 进入视口时的整块浮起 | 从 `@/lib/gsap` 取 gsap |
| `Magnet.tsx` | 按钮磁吸 | 无 |
| `ClickSpark.tsx` | 点击迸溅（釉色火花） | 画布固定在视口、监听 window 点击（原版按父元素大小，包住整页会太大） |
| `SpotlightCard.tsx` | 卡片跟随光斑 | 默认类改为青瓷浅色 |
| `Noise.tsx` | 全站宣纸颗粒（fixed 叠加层） | 无 |
| `Topography.tsx` | 首屏釉面等高线（OGL WebGL） | 无 |
| `GlareHover.tsx` | 平台卡片的釉光扫过 | 无 |
| `ScrollVelocity.tsx` | 模块名跑马灯（随滚动速度加速） | 去掉写死的字号字重，排版交给 className |

没有 Pro 许可（环境里没有 `REACTBITS_LICENSE_KEY`），不用 Pro 的 blocks。
