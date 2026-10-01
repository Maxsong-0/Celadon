import { useEffect, useRef } from 'react';
import { useLocation } from 'react-router-dom';
import Hero from '@/components/celadon/Hero';
// 跑马灯与数字节去掉 motion 后只剩 1 ～ 2 KB（gzip），依赖都在首屏包里：静态引入，省两次请求，也不再有占位高度与实际高度不符导致的跳动。
import Marquee from '@/components/celadon/Marquee';
import ProcessSection from '@/components/celadon/ProcessSection';
import GlazeSection from '@/components/celadon/GlazeSection';
import Showcase from '@/components/celadon/Showcase';
import Listen from '@/components/celadon/Listen';
import Bento from '@/components/celadon/Bento';
import AlgoTeaser from '@/components/celadon/AlgoTeaser';
import Platforms from '@/components/celadon/Platforms';
import Stats from '@/components/celadon/Stats';
import { ScrollTrigger } from '@/lib/gsap';
import { onFontsLoaded } from '@/lib/fonts';
import { scrollToAnchor } from '@/lib/anchor';

/** 首页：首屏 → 跑马灯 → 壹 工序 → 贰 施釉 → 叁 真界面 → 肆 试听 → 伍 判断 → 陆 公开算法 → 柒 数字 → 捌 获取（设计规格-官网 第 4 节）。 */
export default function Home() {
  const { hash, key } = useLocation();

  // 带 #process / #listen / #get 进来（导航「工艺」、别的页面跳回来）：滚到那一节。原来先滚再 ScrollTrigger.refresh()，
  // 刷新把滚动位置写回 0，平滑滚动当场被打断、哪儿也去不了；改由 lib/anchor 先量再滚、之后每次重新测量后再对准。
  // 进首页时直接到位，已在首页时再点导航才平滑滚过去；依赖带上 key：已在 #process 时再点一次「工艺」也会滚回去。
  const landed = useRef(false);
  useEffect(() => {
    const stop = scrollToAnchor(hash, landed.current);
    landed.current = true;
    return stop;
  }, [hash, key]);

  // 字体换上后触发器位置会变，重新量（思源的声明是异步插入的，见 lib/fonts）。
  useEffect(() => onFontsLoaded(() => ScrollTrigger.refresh()), []);

  return (
    <main>
      <Hero />
      <Marquee />
      <ProcessSection />
      <GlazeSection />
      <Showcase />
      <Listen />
      <Bento />
      <AlgoTeaser />
      <Stats />
      <Platforms />
    </main>
  );
}
