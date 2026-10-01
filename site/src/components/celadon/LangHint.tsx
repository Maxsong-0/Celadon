import { useEffect, useState } from 'react';
import { useLocale, useSwitchLocale } from '@/i18n/locale';
import { dismissHint, hintDismissed, savedLocale } from '@/i18n/paths';

/**
 * 「有中文版」提示（ADR-024）：英文页、浏览器首选语言是中文、用户没自己选过语言、也没关过提示时，导航下方一条小胶囊。
 * 不自动跳转。文字固定用中文（给看中文的人看的），不进 i18n 资源。关掉或切换后都不再出现。
 */
export default function LangHint() {
  const locale = useLocale();
  const switchLocale = useSwitchLocale();
  const [show, setShow] = useState(false);

  useEffect(() => {
    if (locale !== 'en' || savedLocale() !== null || hintDismissed()) return;
    const first = navigator.languages?.[0] ?? navigator.language ?? '';
    if (/^zh\b/i.test(first)) setShow(true);
  }, [locale]);

  if (!show || locale !== 'en') return null;

  return (
    <div lang="zh-CN" className="pointer-events-none fixed inset-x-0 top-[72px] z-20 flex justify-center px-4 sm:top-[80px]">
      <div
        role="note"
        className="pointer-events-auto flex items-center gap-3 rounded-full bg-paper/85 py-1.5 pl-4 pr-1.5 text-[13px] text-ink-2 ring-1 ring-ink/8 shadow-[0_10px_40px_-18px_rgba(13,40,48,0.35)] backdrop-blur-xl"
      >
        <span>本站有中文版</span>
        <button
          type="button"
          onClick={() => {
            setShow(false);
            switchLocale();
          }}
          className="rounded-full bg-c700 px-3 py-1 text-paper transition-colors duration-300 hover:bg-c800"
        >
          切换到中文
        </button>
        <button
          type="button"
          aria-label="关闭提示"
          onClick={() => {
            dismissHint();
            setShow(false);
          }}
          className="grid size-7 place-items-center rounded-full text-ink-3 transition-colors duration-300 hover:bg-ink/5 hover:text-ink"
        >
          <svg viewBox="0 0 16 16" className="size-3" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" aria-hidden="true">
            <path d="m4 4 8 8M12 4l-8 8" />
          </svg>
        </button>
      </div>
    </div>
  );
}
