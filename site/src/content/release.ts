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
  version: '0.1.8.11',
  date: '2026-11-14',
  packages: [
    {
      id: 'mac-arm64',
      file: 'Celadon-0.1.8.11-macOS-AppleSilicon.pkg',
      bytes: 7192112,
      sha256: 'afb0a96aed2c902128e8bd8dec196c5133f0149fd7f51bb26cf55bfa4b4baa91'
    },
    {
      id: 'mac-x64',
      file: 'Celadon-0.1.8.11-macOS-Intel.pkg',
      bytes: 8079944,
      sha256: '6aba68a9f0ff1ee3bfa316ffcb51424e0ad62b905fcfcf3ba63ec9f3bbd7a5f6'
    },
    {
      id: 'win-x64',
      file: 'Celadon-0.1.8.11-Windows-x64-Setup.exe',
      bytes: 7555247,
      sha256: '4df84f97767167ba8190c89a4b0d94b5f8c410097d8339ea60b74551ff5dbbb3'
    }
  ]
};

/** 三个包的校验值合在一份里，与安装包放在一起（Mac：shasum -a 256 -c SHA256SUMS）。 */
export const CHECKSUMS_FILE = 'SHA256SUMS';

/** 下载地址：走静态服务的 /downloads/，不走前端路由；部署在子路径时跟着 BASE_URL。 */
export function downloadHref(file: string): string {
  return `${import.meta.env.BASE_URL}downloads/${encodeURIComponent(file)}`;
}
