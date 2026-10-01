/**
 * 认这台设备该用哪个安装包、或者根本用不了（下载页、导航与首页的「一键直下」共用）。
 * 本文件只有纯函数：classify 只看传进来的信号，不碰 navigator / DOM，好在服务器上用 node 跑对照（读信号与缓存在 usePlatform.ts）。
 *
 * 能确定的才一键直下：Windows 64 位；Mac 的芯片要么浏览器直接报了 CPU 架构（Chromium 的 UA-CH），要么从显卡推得出来。
 * 推不出来的 Mac 交给下载页让人自己选——Safari 的 UA 永远写 Intel，猜错了 .pkg 会直接说「不能装在这台电脑上」。
 */
import type { PackageId } from '@/content/release';

export type UnsupportedReason = 'phone' | 'tablet' | 'linux' | 'chromeos' | 'old-windows' | 'windows-32' | 'old-macos';

export type Detection =
  /** 确定：可以一键直下 */
  | { kind: 'match'; id: PackageId }
  /** 认出是 Mac，看不出芯片 */
  | { kind: 'mac' }
  /** ARM 版 Windows：x64 安装包要靠系统转译，只有 x64 版宿主加载得了 */
  | { kind: 'win-arm' }
  | { kind: 'unsupported'; reason: UnsupportedReason }
  /** 桌面设备但认不出系统：不猜 */
  | { kind: 'unknown' };

/** 显卡推断：Apple 芯片的 GPU，或 Intel Mac 才有的 Intel / AMD / NVIDIA 显卡。 */
export type GpuHint = 'apple' | 'intel';

export interface Signals {
  ua: string;
  /** navigator.userAgentData.platform，没有就用 navigator.platform */
  platform: string;
  maxTouchPoints: number;
  /** navigator.userAgentData.mobile */
  mobile?: boolean;
  /** UA-CH 高熵值 architecture：'arm' / 'x86' */
  architecture?: string;
  /** UA-CH 高熵值 platformVersion：macOS 的真实版本（UA 里冻结成 10_15_7） */
  platformVersion?: string;
  gpu?: GpuHint;
}

const WINDOWS_NT = /Windows NT (\d+)\.(\d+)/;
// 64 位 Windows 上的浏览器 UA 里总有其中一个（Chromium 精简后的 UA 一律写 Win64; x64），32 位系统上一个都没有。
const WINDOWS_64 = /Win64|WOW64|x64|ARM64|aarch64/i;
const WINDOWS_ARM = /ARM64|aarch64/i;
// UA 冻结值是 10_15_7；比 10.15 还小的写法只会来自真正的旧系统（Safari 13 及更早）。
const MAC_OS_X = /Mac OS X 10[._](\d+)/;
const MIN_MACOS_MAJOR = 12;

export function classify(s: Signals): Detection {
  const { ua, platform } = s;

  // 手机 / 平板先认。UA 里明写的（iPhone、Android）排在推断之前：Android 平板的 Chrome 不带 Mobile；
  // iPad 的桌面版 Safari 自称 Mac，只能靠「Mac + 多点触控」推断（模拟器里平台字段可能还是宿主的 MacIntel，不能让它盖过明写的 Android）。
  if (/iPhone|iPod/.test(ua)) return { kind: 'unsupported', reason: 'phone' };
  if (/Android/i.test(ua)) return { kind: 'unsupported', reason: s.mobile || /Mobile/.test(ua) ? 'phone' : 'tablet' };
  if (/iPad/.test(ua) || ((/^Mac/i.test(platform) || /Macintosh/.test(ua)) && s.maxTouchPoints > 1)) {
    return { kind: 'unsupported', reason: 'tablet' };
  }
  if (s.mobile) return { kind: 'unsupported', reason: 'phone' };
  if (/CrOS/.test(ua) || /Chrome OS/i.test(platform)) return { kind: 'unsupported', reason: 'chromeos' };

  const nt = WINDOWS_NT.exec(ua);
  if (nt || /^Win/i.test(platform)) {
    if (nt && Number(nt[1]) < 10) return { kind: 'unsupported', reason: 'old-windows' };
    if (s.architecture === 'arm' || WINDOWS_ARM.test(ua)) return { kind: 'win-arm' };
    if (nt && !WINDOWS_64.test(ua)) return { kind: 'unsupported', reason: 'windows-32' };
    return { kind: 'match', id: 'win-x64' };
  }

  if (/^Mac/i.test(platform) || /Macintosh/.test(ua)) {
    const major = macMajor(s);
    if (major !== null && major < MIN_MACOS_MAJOR) return { kind: 'unsupported', reason: 'old-macos' };
    if (s.architecture === 'arm' || (s.architecture === undefined && s.gpu === 'apple')) return { kind: 'match', id: 'mac-arm64' };
    if (s.architecture === 'x86' || (s.architecture === undefined && s.gpu === 'intel')) return { kind: 'match', id: 'mac-x64' };
    return { kind: 'mac' };
  }

  if (/Linux|X11/i.test(platform) || /Linux|X11/.test(ua)) return { kind: 'unsupported', reason: 'linux' };
  return { kind: 'unknown' };
}

/** macOS 主版本：UA-CH 的真实版本优先；UA 里只有比冻结值 10.15 还旧的写法可信。读不到返回 null（不拦）。 */
function macMajor(s: Signals): number | null {
  if (s.platformVersion) {
    const major = Number(s.platformVersion.split('.')[0]);
    if (Number.isFinite(major) && major > 0) return major;
  }
  const m = MAC_OS_X.exec(s.ua);
  if (m && Number(m[1]) < 15) return 10;
  return null;
}

/** 从 WebGL 的显卡名与压缩纹理支持推 Mac 的芯片（Safari / Firefox 不报 CPU 架构）。推不出返回 undefined。 */
export function gpuFromWebGL(renderer: string, hasAstc: boolean): GpuHint | undefined {
  // Firefox 直接给（去掉细节的）显卡名：「Apple M1」或「Intel(R) …」「AMD Radeon …」；Intel Mac 才有独立显卡。
  if (/Apple M\d/i.test(renderer)) return 'apple';
  if (/Intel|AMD|Radeon|NVIDIA|GeForce/i.test(renderer)) return 'intel';
  // Safari 只报「Apple GPU」，分不出来；但 ASTC 压缩纹理只有 Apple 芯片的 GPU 支持，Intel Mac 的显卡在 Metal 上没有。
  if (hasAstc) return 'apple';
  return undefined;
}

/** 卡片 / 按钮上的平台名。 */
export const PACKAGE_LABEL: Record<PackageId, string> = {
  'mac-arm64': 'Mac 版 · Apple 芯片',
  'mac-x64': 'Mac 版 · Intel',
  'win-x64': 'Windows 版'
};

/** 不支持时的一句话原因（下载页顶部的提示）。 */
export const UNSUPPORTED_TEXT: Record<UnsupportedReason, string> = {
  phone: '这台是手机。Celadon 是电脑上的音频插件，要装在 Mac 或 Windows 电脑上。',
  tablet: '这台是平板。Celadon 是电脑上的音频插件，要装在 Mac 或 Windows 电脑上。',
  linux: '这台是 Linux。Celadon 现在只有 Mac 和 Windows 版。',
  chromeos: '这台是 ChromeOS。Celadon 现在只有 Mac 和 Windows 版。',
  'old-windows': '这台是 Windows 7 / 8。Celadon 要 Windows 10 或 11（64 位）。',
  'windows-32': '这台是 32 位 Windows。Celadon 只有 64 位版。',
  'old-macos': '这台 Mac 的系统低于 macOS 12。Celadon 要 macOS 12（Monterey）及以上。'
};
