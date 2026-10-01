import { useCallback } from 'react';
import { useTranslation } from 'react-i18next';
import type { Kind } from './index';

export interface PageText {
  title: string;
  summary: string;
}

/**
 * 说明 / 算法页的标题与摘要（当前语言）：docs 命名空间 pages.<kind>.<file>。
 * 键由导航表拼出来，类型系统查不到具体是哪一条，这里统一转一次；两种语言都齐全由 scripts/check-i18n.mjs 核对。
 */
export function usePageText(): (kind: Kind, file: string) => PageText {
  const { t } = useTranslation('docs');
  return useCallback(
    (kind: Kind, file: string) => ({
      title: t(`pages.${kind}.${file}.title` as 'pages.docs.quickstart.title'),
      summary: t(`pages.${kind}.${file}.summary` as 'pages.docs.quickstart.summary')
    }),
    [t]
  );
}
