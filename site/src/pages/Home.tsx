import { lazy, Suspense, useEffect } from 'react';
import { useLocation } from 'react-router-dom';
import Hero from '@/components/celadon/Hero';
import ProcessSection from '@/components/celadon/ProcessSection';
import GlazeSection from '@/components/celadon/GlazeSection';
import Showcase from '@/components/celadon/Showcase';
import Listen from '@/components/celadon/Listen';
import Bento from '@/components/celadon/Bento';
import AlgoTeaser from '@/components/celadon/AlgoTeaser';
import Platforms from '@/components/celadon/Platforms';
import { ScrollTrigger } from '@/lib/gsap';

// 首屏之下、较重的两段懒加载，别挡首屏。
const Marquee = lazy(() => import('@/components/celadon/Marquee'));
const Stats = lazy(() => import('@/components/celadon/Stats'));

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
      <Suspense fallback={<div className="h-[220px] sm:h-[260px]" aria-hidden="true" />}>
        <Marquee />
      </Suspense>
      <ProcessSection />
      <GlazeSection />
      <Showcase />
      <Listen />
      <Bento />
      <AlgoTeaser />
      <Suspense fallback={<div className="h-[420px]" aria-hidden="true" />}>
        <Stats />
      </Suspense>
      <Platforms />
    </main>
  );
}
