/*
  来自 React Bits（https://reactbits.dev · https://github.com/DavidHDev/react-bits），作者 David Haz。
  许可：MIT + Commons Clause License Condition v1.0——可作为网站的一部分使用与修改，不得单独转卖或再分发组件本身。
  本文件按 Celadon 站点需要做了少量改动（见文件内「Celadon 改动」注释）。
*/
'use client';

import React, { useRef, useEffect, useState } from 'react';
// Celadon 改动：gsap 与插件统一从 @/lib/gsap 取（那里只注册一次）。
import { gsap, ScrollTrigger, SplitText as GSAPSplitText, useGSAP } from '@/lib/gsap';

export interface SplitTextProps {
  text: string;
  className?: string;
  delay?: number;
  duration?: number;
  ease?: string | ((t: number) => number);
  splitType?: 'chars' | 'words' | 'lines' | 'words, chars';
  from?: gsap.TweenVars;
  to?: gsap.TweenVars;
  threshold?: number;
  rootMargin?: string;
  tag?: 'h1' | 'h2' | 'h3' | 'h4' | 'h5' | 'h6' | 'p' | 'span';
  textAlign?: React.CSSProperties['textAlign'];
  onLetterAnimationComplete?: () => void;
  /** Celadon 改动：false = 不给每个字开 3D 合成层（父元素用 background-clip: text 渐变字时，Chrome 会把渐变画错） */
  force3D?: boolean;
}

const SplitText: React.FC<SplitTextProps> = ({
  text,
  className = '',
  delay = 50,
  duration = 1.25,
  ease = 'power3.out',
  splitType = 'chars',
  from = { opacity: 0, y: 40 },
  to = { opacity: 1, y: 0 },
  threshold = 0.1,
  rootMargin = '-100px',
  tag = 'p',
  textAlign = 'center',
  onLetterAnimationComplete,
  force3D = true
}) => {
  const ref = useRef<HTMLParagraphElement>(null);
  const animationCompletedRef = useRef(false);
  const onCompleteRef = useRef(onLetterAnimationComplete);
  const [fontsLoaded, setFontsLoaded] = useState<boolean>(false);

  // Keep callback ref updated
  useEffect(() => {
    onCompleteRef.current = onLetterAnimationComplete;
  }, [onLetterAnimationComplete]);

  // Celadon 改动：只等这段文字用到的字体子集，不等整页 document.fonts.ready
  // （思源宋体按 unicode-range 拆成上百个子集，整页等齐要好几秒，标题会一直藏着）。最多等 0.6 s。
  useEffect(() => {
    const el = ref.current;
    if (!el) return;
    let done = false;
    const finish = () => {
      if (!done) {
        done = true;
        setFontsLoaded(true);
      }
    };
    const timer = window.setTimeout(finish, 600);
    try {
      const cs = window.getComputedStyle(el);
      document.fonts.load(`${cs.fontWeight} ${cs.fontSize} ${cs.fontFamily}`, text).then(finish, finish);
    } catch {
      finish();
    }
    return () => window.clearTimeout(timer);
  }, [text]);

  useGSAP(
    () => {
      if (!ref.current || !text || !fontsLoaded) return;
      // Prevent re-animation if already completed
      if (animationCompletedRef.current) return;
      const el = ref.current as HTMLElement & {
        _rbsplitInstance?: GSAPSplitText;
      };

      if (el._rbsplitInstance) {
        try {
          el._rbsplitInstance.revert();
        } catch {
          /* ignore */
        }
        el._rbsplitInstance = undefined;
      }

      const startPct = (1 - threshold) * 100;
      const marginMatch = /^(-?\d+(?:\.\d+)?)(px|em|rem|%)?$/.exec(rootMargin);
      const marginValue = marginMatch ? parseFloat(marginMatch[1]) : 0;
      const marginUnit = marginMatch ? marginMatch[2] || 'px' : 'px';
      const sign =
        marginValue === 0
          ? ''
          : marginValue < 0
            ? `-=${Math.abs(marginValue)}${marginUnit}`
            : `+=${marginValue}${marginUnit}`;
      const start = `top ${startPct}%${sign}`;
      let targets: Element[] = [];
      const assignTargets = (self: GSAPSplitText) => {
        if (splitType.includes('chars') && (self as GSAPSplitText).chars?.length)
          targets = (self as GSAPSplitText).chars;
        if (!targets.length && splitType.includes('words') && self.words.length) targets = self.words;
        if (!targets.length && splitType.includes('lines') && self.lines.length) targets = self.lines;
        if (!targets.length) targets = self.chars || self.words || self.lines;
      };
      const splitInstance = new GSAPSplitText(el, {
        type: splitType,
        smartWrap: true,
        autoSplit: splitType === 'lines',
        linesClass: 'split-line',
        wordsClass: 'split-word',
        charsClass: 'split-char',
        reduceWhiteSpace: false,
        onSplit: (self: GSAPSplitText) => {
          assignTargets(self);
          return gsap.fromTo(
            targets,
            { ...from },
            {
              ...to,
              duration,
              ease,
              stagger: delay / 1000,
              scrollTrigger: {
                trigger: el,
                start,
                once: true,
                fastScrollEnd: true,
                anticipatePin: 0.4
              },
              onComplete: () => {
                animationCompletedRef.current = true;
                onCompleteRef.current?.();
                // Celadon 改动：动画结束后清掉 transform / will-change，让渐变字与文字选择恢复正常。
                gsap.set(targets, { clearProps: 'transform,willChange' });
              },
              willChange: force3D ? 'transform, opacity' : 'opacity',
              force3D
            }
          );
        }
      });
      el._rbsplitInstance = splitInstance;
      return () => {
        ScrollTrigger.getAll().forEach(st => {
          if (st.trigger === el) st.kill();
        });
        try {
          splitInstance.revert();
        } catch {
          /* ignore */
        }
        el._rbsplitInstance = undefined;
      };
    },
    {
      dependencies: [
        text,
        delay,
        duration,
        ease,
        splitType,
        JSON.stringify(from),
        JSON.stringify(to),
        threshold,
        rootMargin,
        fontsLoaded,
        force3D
      ],
      scope: ref
    }
  );

  const renderTag = () => {
    const style: React.CSSProperties = {
      textAlign,
      wordWrap: 'break-word',
      willChange: force3D ? 'transform, opacity' : undefined
    };
    const classes = `split-parent overflow-hidden inline-block whitespace-normal ${className}`;
    const Tag = tag || 'p';

    return (
      <Tag ref={ref} style={style} className={classes}>
        {text}
      </Tag>
    );
  };

  return renderTag();
};

export default SplitText;
