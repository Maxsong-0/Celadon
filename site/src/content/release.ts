/**
 * 当前发布的安装包：/download 页的唯一数据源。
 *
 * 安装包不入 git，在主仓库 artifacts/release-<版本>-<日期>/；site/deploy/deploy.sh 把它们连同 SHA256SUMS 同步到服务器的
 * ~/celadon-site/downloads/，静态服务在 /downloads/<文件名> 提供。出新版时照发布目录改这里：
 *   文件名与 SHA-256 抄 SHA256SUMS，字节数用 `stat -f%z <文件>`（Mac）/ `stat -c%s <文件>`（Linux）。
 * version 与 PLUGIN_VERSION（Celadon.jucer）不一致时，下载页只提示「安装包还没更新」，不给旧文件；
 * deploy.sh 也会核对构建产物里的文件名与校验值和发布目录一致，对不上就不部署。
 */
export type PackageId = 'mac-arm64' | 'mac-x64' | 'win-x64';

export interface ReleasePackage {
  id: PackageId;
  /** 发布目录里的文件名，也是 /downloads/ 下的文件名 */
  file: string;
  /** 字节数（显示时按 1 MB = 10^6 字节换算，与 macOS 访达一致） */
  bytes: number;
  /** 小写十六进制，64 位 */
  sha256: string;
}

export interface Release {
  version: string;
  /** 出包日期 YYYY-MM-DD */
  date: string;
  packages: readonly ReleasePackage[];
}

export const RELEASE: Release = {
  version: '0.1.8.7',
  date: '2026-10-01',
  packages: [
    {
      id: 'mac-arm64',
      file: 'Celadon-0.1.8.7-macOS-AppleSilicon.pkg',
      bytes: 7189798,
      sha256: '85601027349c157bb0bec1f38657beef491d49ce49fa87a06a9d5a79df45ac39'
    },
    {
      id: 'mac-x64',
      file: 'Celadon-0.1.8.7-macOS-Intel.pkg',
      bytes: 8039668,
      sha256: 'a1ef417e60502b50ea2532cb70505e8911db464f311e326be9ef6edfbe0f1b75'
    },
    {
      id: 'win-x64',
      file: 'Celadon-0.1.8.7-Windows-x64-Setup.exe',
      bytes: 7464000,
      sha256: '37dce2e7fe6794f60727c79fb33735b4c3c052ce13543e52a23ec4736160bbf7'
    }
  ]
};

/** 三个包的校验值合在一份里，与安装包放在一起（Mac：shasum -a 256 -c SHA256SUMS）。 */
export const CHECKSUMS_FILE = 'SHA256SUMS';

/** 下载地址：走静态服务的 /downloads/，不走前端路由；部署在子路径时跟着 BASE_URL。 */
export function downloadHref(file: string): string {
  return `${import.meta.env.BASE_URL}downloads/${encodeURIComponent(file)}`;
}
