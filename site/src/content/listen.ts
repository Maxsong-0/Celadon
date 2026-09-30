// 首页「肆 · 试听」的曲目清单。音频与波形在 public/audio/<id>/ 下，由主进程用 celadon_render 在 107 上渲染：
//   original.m4a = 原曲同一段（只乘增益对齐到 matchLufs）；celadon.m4a = 过整条链（GLAZE / 风格见 settings）后同样对齐；
//   两段逐样本对齐、等长，切换时位置不变。peaks = 两版各 400 点的峰值包络（0 ～ 1）。
// 曲目必须是 Max 自己有版权的作品（T-033，2026-09-30 定）。清单为空时整节不显示。

export interface ListenVersion {
  /** 相对 public/ 的路径 */
  src: string;
  /** 对齐前的整合响度（LUFS-I）：关掉「等响」时用它还原真实响度差 */
  lufs: number;
  /** 对齐前的真峰值（dBTP） */
  truePeak: number;
  peaks: number[];
}

export interface ListenTrack {
  id: string;
  title: string;
  note: string;
  /** 片段在原曲里的起点（秒），只用于说明 */
  from: number;
  seconds: number;
  settings: string;
  original: ListenVersion;
  celadon: ListenVersion;
}

/** 两版在网页里都对齐到这个响度播放（等响开时）。 */
export const MATCH_LUFS = -14;

export const tracks: ListenTrack[] = [];
