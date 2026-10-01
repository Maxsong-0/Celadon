// GSAP 只在这里注册一次；所有组件从这里拿 gsap / ScrollTrigger，不各自 registerPlugin。
import { gsap } from 'gsap';
import { ScrollTrigger } from 'gsap/ScrollTrigger';
import { SplitText } from 'gsap/SplitText';
import { useGSAP } from '@gsap/react';

gsap.registerPlugin(ScrollTrigger, SplitText, useGSAP);

// 站点统一的缓动：像釉在坯上流下来——起得快、收得慢。
gsap.defaults({ ease: 'power3.out', duration: 0.9 });

export { gsap, ScrollTrigger, SplitText, useGSAP };
