// 以英文 JSON 为界面短句的类型：t('…') 的 key 写错、命名空间不对都过不了 tsc；中文 JSON 的 key 集合由 scripts/check-i18n.mjs 核对。
import 'i18next';
import type common from './locales/en/common.json';
import type home from './locales/en/home.json';
import type download from './locales/en/download.json';
import type changelog from './locales/en/changelog.json';
import type docs from './locales/en/docs.json';
import type figures from './locales/en/figures.json';

declare module 'i18next' {
  interface CustomTypeOptions {
    defaultNS: 'common';
    resources: {
      common: typeof common;
      home: typeof home;
      download: typeof download;
      changelog: typeof changelog;
      docs: typeof docs;
      figures: typeof figures;
    };
  }
}
