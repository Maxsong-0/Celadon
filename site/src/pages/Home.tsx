import { useEffect } from 'react';
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

/** 首页：首屏 → 跑马灯 → 壹 工序 → 贰 施釉 → 叁 真界面 → 肆 试听 → 伍 判断 → 陆 公开算法 → 柒 数字 → 捌 获取（设计规格-官网 第 4 节）。 */
export default function Home() {
  const { hash } = useLocation();

  useEffect(() => {
    // 从别的页面跳回来带 #process / #listen / #get：等布局稳定后滚过去，再让 ScrollTrigger 重新量一遍位置。
    if (!hash) return;
    const id = hash.slice(1);
    const t = window.setTimeout(() => {
      document.getElementById(id)?.scrollIntoView({ behavior: 'smooth', block: 'start' });
      ScrollTrigger.refresh();
    }, 80);
    return () => window.clearTimeout(t);
  }, [hash]);

  useEffect(() => {
    // 字体到位后触发器位置会变，刷新一次。
    document.fonts?.ready.then(() => ScrollTrigger.refresh());
  }, []);

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
