// 站点与插件共用的事实，只在这里定义一次。
// 版本号的唯一真源是仓库根的 Celadon.jucer（ADR-010）；构建时注入，站内不许再手写版本号。
export const PLUGIN_VERSION: string = __CELADON_VERSION__;
