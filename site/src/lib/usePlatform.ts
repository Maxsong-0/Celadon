import { useEffect, useState } from 'react';
import { classify, gpuFromWebGL, type Detection, type Signals } from './platform';

/*
  读浏览器给的信号、交给 classify（lib/platform.ts）。整个页面生命周期只认一次：
  同步的那部分（UA、平台、触点）首帧就有；要异步问的（UA-CH 的 CPU 架构与系统版本）和要建 WebGL 上下文的（显卡）
  只在可能改变结论时做一次，结果缓存在模块里，导航、首页、下载页共用。
*/

interface UADataLike {
  platform?: string;
  mobile?: boolean;
  getHighEntropyValues?: (hints: string[]) => Promise<{ architecture?: string; platformVersion?: string }>;
}

function uaData(): UADataLike | undefined {
  return (navigator as unknown as { userAgentData?: UADataLike }).userAgentData;
}

function syncSignals(): Signals {
  const data = uaData();
  return {
    ua: navigator.userAgent,
    platform: data?.platform || navigator.platform || '',
    maxTouchPoints: navigator.maxTouchPoints || 0,
    mobile: data?.mobile
  };
}

let first: Detection | null = null;
let refined: Promise<Detection> | null = null;

function detectSync(): Detection {
  if (typeof navigator === 'undefined') return { kind: 'unknown' };
  return (first ??= classify(syncSignals()));
}

function detectRefined(): Promise<Detection> {
  return (refined ??= refine());
}

async function refine(): Promise<Detection> {
  const base = detectSync();
  // 只有 Mac（芯片、系统版本）和 Windows（是不是 ARM）还可能被异步信号改判；其余首帧的结论就是最终的。
  const isMac = base.kind === 'mac' || (base.kind === 'match' && base.id !== 'win-x64');
  const isWindows = base.kind === 'match' && base.id === 'win-x64';
  if (!isMac && !isWindows) return base;

  const signals = syncSignals();
  const data = uaData();
  if (data?.getHighEntropyValues) {
    try {
      const v = await data.getHighEntropyValues(['architecture', 'platformVersion']);
      signals.architecture = v.architecture || undefined;
      signals.platformVersion = v.platformVersion || undefined;
    } catch {
      // 拿不到就按首帧信号
    }
  }
  if (isMac && signals.architecture === undefined) signals.gpu = gpuFromWebGL(...webglProbe());
  return classify(signals);
}

/** 建一个低功耗的 WebGL 上下文读显卡名与 ASTC 支持，读完立刻释放（双显卡的 Intel MacBook 不会因此切到独显）。 */
function webglProbe(): [string, boolean] {
  try {
    const canvas = document.createElement('canvas');
    const gl = canvas.getContext('webgl', { powerPreference: 'low-power' });
    if (!gl) return ['', false];
    // Firefox 的 RENDERER 就是去掉细节的显卡名；Chromium / Safari 这里是「WebKit WebGL」，再看调试扩展
    // （Firefox 读调试扩展会在控制台报弃用警告，所以只在 RENDERER 不像显卡名时才读）。
    let renderer = String(gl.getParameter(gl.RENDERER) ?? '');
    if (!/Apple|Intel|AMD|Radeon|NVIDIA|GeForce/i.test(renderer)) {
      const info = gl.getExtension('WEBGL_debug_renderer_info');
      if (info) renderer = String(gl.getParameter(info.UNMASKED_RENDERER_WEBGL) ?? '');
    }
    const astc = gl.getExtension('WEBGL_compressed_texture_astc') !== null;
    gl.getExtension('WEBGL_lose_context')?.loseContext();
    return [renderer, astc];
  } catch {
    return ['', false];
  }
}

/** 这台设备的识别结果：首帧给同步结论，异步信号回来后（至多一次）更新。 */
export function usePlatform(): Detection {
  const [detection, setDetection] = useState(detectSync);

  useEffect(() => {
    let alive = true;
    detectRefined().then(d => {
      if (alive) setDetection(prev => (sameDetection(prev, d) ? prev : d));
    });
    return () => {
      alive = false;
    };
  }, []);

  return detection;
}

function sameDetection(a: Detection, b: Detection): boolean {
  if (a.kind !== b.kind) return false;
  if (a.kind === 'match' && b.kind === 'match') return a.id === b.id;
  if (a.kind === 'unsupported' && b.kind === 'unsupported') return a.reason === b.reason;
  return true;
}
