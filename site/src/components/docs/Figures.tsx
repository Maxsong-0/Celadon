import type { JSX } from 'react';

/*
  算法页与说明页里的示意图（`![说明](figure:key)`）。纯 SVG，颜色用站点调色板：
  青 = 信号 / 手动，紫 = GLAZE 自动，橙 = 需要注意，灰 = 结构。
*/

const mono = { fontFamily: 'var(--font-mono)', fontSize: 11, fill: '#637780' } as const;
const label = { fontFamily: 'var(--font-sans)', fontSize: 12, fill: '#0D1B21' } as const;
const small = { fontFamily: 'var(--font-sans)', fontSize: 10.5, fill: '#8A9CA3' } as const;

function Box({ x, y, w, h, title, sub, tone = 'c' }: { x: number; y: number; w: number; h: number; title: string; sub?: string; tone?: 'c' | 'v' | 'g' }) {
  const fill = tone === 'c' ? '#E6F5F7' : tone === 'v' ? '#EEEAFF' : '#F3F1EA';
  const stroke = tone === 'c' ? '#67CAD7' : tone === 'v' ? '#B9ACFF' : '#D9E2E4';
  const text = tone === 'v' ? '#4B37C9' : '#0D1B21';
  return (
    <g>
      <rect x={x} y={y} width={w} height={h} rx="10" fill={fill} stroke={stroke} />
      <text x={x + w / 2} y={y + (sub ? h / 2 - 3 : h / 2 + 4)} textAnchor="middle" style={{ ...label, fill: text, fontWeight: 600 }}>
        {title}
      </text>
      {sub ? (
        <text x={x + w / 2} y={y + h / 2 + 13} textAnchor="middle" style={small}>
          {sub}
        </text>
      ) : null}
    </g>
  );
}

function Arrow({ x1, y1, x2, y2, dashed = false, color = '#4B5E66' }: { x1: number; y1: number; x2: number; y2: number; dashed?: boolean; color?: string }) {
  return <line x1={x1} y1={y1} x2={x2} y2={y2} stroke={color} strokeWidth="1.3" strokeDasharray={dashed ? '4 4' : undefined} markerEnd="url(#fig-arrow)" />;
}

function Defs() {
  return (
    <defs>
      <marker id="fig-arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse">
        <path d="M0 0 L10 5 L0 10 z" fill="#4B5E66" />
      </marker>
    </defs>
  );
}

export const figures: Record<string, () => JSX.Element> = {
  chain: () => (
    <svg viewBox="0 0 760 300" className="w-full h-auto" role="img" aria-label="信号链示意图">
      <Defs />
      <Box x={20} y={120} w={80} h={56} title="输入" tone="g" />
      <Box x={150} y={120} w={110} h={56} title="BALANCE" sub="27 段 · 0 样本" />
      <Box x={290} y={120} w={110} h={56} title="CLAY" sub="修坯 · D + G" />
      <Box x={430} y={120} w={110} h={56} title="COLOR" sub="染色 · 64 样本" />
      <Box x={570} y={120} w={110} h={56} title="LIMIT" sub="限幅 · 60 + LA + 62" />
      <Box x={700} y={120} w={50} h={56} title="出" tone="g" />
      <Arrow x1={100} y1={148} x2={148} y2={148} />
      <Arrow x1={260} y1={148} x2={288} y2={148} />
      <Arrow x1={400} y1={148} x2={428} y2={148} />
      <Arrow x1={540} y1={148} x2={568} y2={148} />
      <Arrow x1={680} y1={148} x2={698} y2={148} />
      {/* 表 */}
      <Box x={60} y={230} w={110} h={44} title="IN 表" sub="真峰值 · LUFS 子块" tone="g" />
      <Box x={590} y={230} w={140} h={44} title="OUT 表" sub="真峰值 · LUFS · GR" tone="g" />
      <Arrow x1={110} y1={176} x2={112} y2={228} dashed />
      <Arrow x1={690} y1={176} x2={660} y2={228} dashed />
      {/* GLAZE */}
      <Box x={250} y={20} w={260} h={54} title="GLAZE 编排器" sub="画像 → 方案 → 校准 → 保护" tone="v" />
      <Arrow x1={120} y1={120} x2={250} y2={62} dashed color="#6A4DFF" />
      <text x="150" y="86" style={{ ...small, fill: '#6A4DFF' }}>入口特征</text>
      <Arrow x1={380} y1={74} x2={205} y2={118} dashed color="#6A4DFF" />
      <Arrow x1={400} y1={74} x2={345} y2={118} dashed color="#6A4DFF" />
      <Arrow x1={420} y1={74} x2={485} y2={118} dashed color="#6A4DFF" />
      <Arrow x1={440} y1={74} x2={625} y2={118} dashed color="#6A4DFF" />
      <text x="530" y="60" style={{ ...small, fill: '#6A4DFF' }}>实际参数 = clamp(手动 + 自动增量)</text>
      <Arrow x1={600} y1={176} x2={520} y2={78} dashed color="#8A9CA3" />
      <text x="548" y="105" style={small}>模块统计回传</text>
      <text x="20" y="292" style={mono}>顺序固定：先修频谱，再加密度与谐波，最后管峰值。每个模块的旁通都是延迟补偿式，切换不错位、不咔哒。</text>
    </svg>
  ),

  limiter: () => {
    const n = 90;
    const pts: [number, number][] = [];
    for (let i = 0; i <= n; i++) {
      const t = i / n;
      const beat = Math.exp(-(((t * 3) % 1) * 5));
      pts.push([40 + t * 680, 0.35 + 0.65 * beat * Math.abs(Math.sin(t * 40))]);
    }
    const ceil = 0.72;
    const gain = pts.map(([, a]) => Math.min(1, ceil / Math.max(a, 1e-3)));
    // 前瞻滑动最小 + 移动平均：粗略示意
    const win = 6;
    const rmin = gain.map((_, i) => Math.min(...gain.slice(i, i + win)));
    const env = rmin.map((_, i) => rmin.slice(Math.max(0, i - win), i + 1).reduce((a, b) => a + b, 0) / Math.min(i + 1, win));
    const y0 = 40;
    const hh = 110;
    return (
      <svg viewBox="0 0 760 320" className="w-full h-auto" role="img" aria-label="限幅器示意图">
        <Defs />
        <text x="40" y="26" style={mono}>|x| 与 8× 插值真峰值 peak[n]</text>
        <line x1="40" y1={y0 + hh * (1 - ceil)} x2="720" y2={y0 + hh * (1 - ceil)} stroke="#E8590C" strokeDasharray="5 4" />
        <text x="724" y={y0 + hh * (1 - ceil) + 4} style={{ ...mono, fill: '#C2410C' }}>Ceiling</text>
        <path d={pts.map(([x, a], i) => `${i ? 'L' : 'M'}${x} ${y0 + hh * (1 - a)}`).join(' ')} fill="none" stroke="#A3E0E8" strokeWidth="1.5" />
        <path d={pts.map(([x, a], i) => `${i ? 'L' : 'M'}${x} ${y0 + hh * (1 - Math.min(a, a * env[i]))}`).join(' ')} fill="none" stroke="#127F90" strokeWidth="1.6" />
        <text x="40" y="196" style={mono}>需求增益 r[n] = min(1, Ceiling / peak[n]) → 前瞻滑动最小 rMin → 双包络 min(快, 慢) → 两级移动平均（三角攻击沿）</text>
        <line x1="40" y1="290" x2="720" y2="290" stroke="#DCE6E9" />
        <line x1="40" y1="220" x2="720" y2="220" stroke="#DCE6E9" strokeDasharray="2 4" />
        <text x="724" y="224" style={mono}>1.0</text>
        <text x="724" y="294" style={mono}>0.5</text>
        <path d={gain.map((g, i) => `${i ? 'L' : 'M'}${pts[i][0]} ${290 - (g - 0.5) * 140}`).join(' ')} fill="none" stroke="#DCE6E9" strokeWidth="1.2" />
        <path d={rmin.map((g, i) => `${i ? 'L' : 'M'}${pts[i][0]} ${290 - (g - 0.5) * 140}`).join(' ')} fill="none" stroke="#B9ACFF" strokeWidth="1.2" />
        <path d={env.map((g, i) => `${i ? 'L' : 'M'}${pts[i][0]} ${290 - (g - 0.5) * 140}`).join(' ')} fill="none" stroke="#0A4A57" strokeWidth="1.8" />
        <text x="40" y="312" style={small}>浅灰 = r[n]（瞬时）· 淡紫 = 前瞻滑动最小 · 深青 = 最终施加的增益（在 1× 域乘到延迟后的音频上；不限幅时精确为 1.0）</text>
      </svg>
    );
  },

  truepeak: () => {
    const samples = [0.2, 0.55, 0.92, 0.9, 0.4, -0.3, -0.8, -0.6, -0.1, 0.3];
    const x = (i: number) => 60 + i * 70;
    const y = (v: number) => 130 - v * 90;
    // 用简单 sinc 插值画一条连续曲线
    const sinc = (t: number) => (t === 0 ? 1 : Math.sin(Math.PI * t) / (Math.PI * t));
    const pts: string[] = [];
    for (let k = 0; k <= 630; k += 2) {
      const tt = k / 70;
      let v = 0;
      for (let i = 0; i < samples.length; i++) v += samples[i] * sinc(tt - i);
      pts.push(`${60 + k},${y(v)}`);
    }
    return (
      <svg viewBox="0 0 760 260" className="w-full h-auto" role="img" aria-label="真峰值示意图">
        <line x1="40" y1="130" x2="720" y2="130" stroke="#DCE6E9" />
        <line x1="40" y1={y(0.92)} x2="720" y2={y(0.92)} stroke="#8A9CA3" strokeDasharray="3 4" />
        <text x="724" y={y(0.92) + 4} style={mono}>采样点峰值</text>
        <line x1="40" y1={y(1.0)} x2="720" y2={y(1.0)} stroke="#E8590C" strokeDasharray="5 4" />
        <text x="724" y={y(1.0) + 4} style={{ ...mono, fill: '#C2410C' }}>真峰值</text>
        <polyline points={pts.join(' ')} fill="none" stroke="#1799AB" strokeWidth="1.6" />
        {samples.map((v, i) => (
          <g key={i}>
            <line x1={x(i)} y1="130" x2={x(i)} y2={y(v)} stroke="#0A4A57" strokeWidth="1.2" />
            <circle cx={x(i)} cy={y(v)} r="3.5" fill="#0A4A57" />
          </g>
        ))}
        <text x="40" y="236" style={small}>采样点之间的真实波形会超过任何一个采样值。Celadon 用 8 相 × 64 抽头多相插值看见它（延迟 32 样本），限幅器与表用同一个检测器：限的峰 = 量的峰。</text>
      </svg>
    );
  },

  balance: () => {
    const n = 27;
    const bw = 680 / n;
    const L = Array.from({ length: n }, (_, i) => 60 + 40 * Math.sin((i / 26) * Math.PI) + (i % 3 === 0 ? 14 : 0) - (i === 9 ? 22 : 0) - (i === 18 ? -18 : 0));
    const R = L.map((_, i) => {
      const k = [1, 2, 3, 4, 3, 2, 1];
      let s = 0;
      let w = 0;
      for (let j = -3; j <= 3; j++) {
        const idx = Math.min(n - 1, Math.max(0, i + j));
        s += L[idx] * k[j + 3];
        w += k[j + 3];
      }
      return s / w;
    });
    const y = (v: number) => 200 - v * 1.4;
    return (
      <svg viewBox="0 0 760 300" className="w-full h-auto" role="img" aria-label="Balance 分析示意图">
        {L.map((v, i) => {
          const d = v - R[i];
          const tone = d > 1 ? '#0A4A57' : d < -1 ? '#6A4DFF' : '#A3E0E8';
          return <rect key={i} x={40 + i * bw + 2} y={y(v)} width={bw - 4} height={200 - y(v) + 40} rx="2" fill={tone} opacity="0.85" />;
        })}
        <path d={R.map((v, i) => `${i ? 'L' : 'M'}${40 + i * bw + bw / 2} ${y(v)}`).join(' ')} fill="none" stroke="#0D1B21" strokeDasharray="4 5" strokeWidth="1.4" />
        <path d={R.map((v, i) => `${i ? 'L' : 'M'}${40 + i * bw + bw / 2} ${y(v + 1)}`).join(' ')} fill="none" stroke="#8A9CA3" strokeWidth="0.8" />
        <path d={R.map((v, i) => `${i ? 'L' : 'M'}${40 + i * bw + bw / 2} ${y(v - 1)}`).join(' ')} fill="none" stroke="#8A9CA3" strokeWidth="0.8" />
        <text x="40" y="26" style={mono}>Lfast[b]（快跟踪电平）与参考轮廓 R[b]（频率方向 [1,2,3,4,3,2,1]/16 抹平 + 时间慢平滑 + Brighten 倾斜）</text>
        <text x="40" y="272" style={small}>深青 = D &gt; +1 dB，Tame 压下 · 紫 = D &lt; −1 dB，Recover 抬起（自动份额还要有两侧能量支持的持续缺口） · 浅青 = 死区 ±1 dB 不动 · 细灰线 = 死区边界</text>
        <text x="40" y="292" style={small}>G[b] 每 hop（≈10 ms）最多变 1 dB；封顶 6 dB × Tame / Recover；静音门 −60 dBFS 以下的段归零。</text>
      </svg>
    );
  },

  glue: () => (
    <svg viewBox="0 0 760 300" className="w-full h-auto" role="img" aria-label="Glue 分频施压示意图">
      <Defs />
      <Box x={20} y={40} w={110} h={50} title="输入" tone="g" />
      <Box x={180} y={40} w={150} h={50} title="侧链 100 Hz 高通" sub="L/R 取大 · 2:1 · 6 dB 拐点" />
      <Box x={380} y={40} w={150} h={50} title="联动包络 g" sub="攻击 20 ms · 自动释放" />
      <Arrow x1={130} y1={65} x2={178} y2={65} />
      <Arrow x1={330} y1={65} x2={378} y2={65} />
      <Box x={180} y={150} w={110} h={50} title="低 ≤ 180 Hz" sub="攻击 36 ms" />
      <Box x={325} y={150} w={110} h={50} title="中" sub="跟联动攻击" />
      <Box x={470} y={150} w={110} h={50} title="高 ≥ 5 kHz" sub="攻击 8 ms" />
      <Arrow x1={455} y1={90} x2={235} y2={148} dashed />
      <Arrow x1={455} y1={90} x2={380} y2={148} dashed />
      <Arrow x1={455} y1={90} x2={525} y2={148} dashed />
      <text x="600" y="170" style={{ ...label, fontSize: 11 }}>低 / 高若自己更轻：</text>
      <text x="600" y="186" style={{ ...mono, fill: '#0E6473' }}>g_band ≤ g + 3.2 dB</text>
      <text x="600" y="202" style={{ ...mono, fill: '#0E6473' }}>g_band ≥ g − 0.6 dB</text>
      <Box x={280} y={240} w={200} h={46} title="求和 → 输出" sub="三路差为 0 时退回单增益乘法" tone="g" />
      <Arrow x1={235} y1={200} x2={330} y2={238} />
      <Arrow x1={380} y1={200} x2={380} y2={238} />
      <Arrow x1={525} y1={200} x2={430} y2={238} />
      <text x="20" y="130" style={small}>表与统计仍读联动包络 g：稳态方波的压缩量读数不变。检测器直接用侧链那一次 100 Hz 高通，不再滤第二遍。</text>
    </svg>
  ),

  heat: () => (
    <svg viewBox="0 0 760 260" className="w-full h-auto" role="img" aria-label="Heat 残差染色示意图">
      <Defs />
      <Box x={20} y={100} w={90} h={50} title="输入" tone="g" />
      <Box x={170} y={30} w={170} h={50} title="干路 · 64 样本延迟" sub="原声完整保留" tone="g" />
      <Box x={170} y={170} w={150} h={50} title="150 Hz 二阶高通" sub="低音不进非线性" />
      <Box x={360} y={170} w={150} h={50} title="M / S 电子管曲线" sub="8× 过采样 · S 更轻且奇对称" />
      <Box x={550} y={170} w={110} h={50} title="残差" sub="湿 − 干" />
      <Box x={600} y={30} w={130} h={50} title="求和 → Mix → Output" tone="g" />
      <Arrow x1={110} y1={120} x2={168} y2={60} />
      <Arrow x1={110} y1={130} x2={168} y2={192} />
      <Arrow x1={320} y1={195} x2={358} y2={195} />
      <Arrow x1={510} y1={195} x2={548} y2={195} />
      <Arrow x1={605} y1={170} x2={640} y2={82} />
      <Arrow x1={340} y1={55} x2={598} y2={55} />
      <text x="20" y="245" style={small}>只把过采样非线性产生的残差加回原声：Heat 0 是等延迟直通；两侧采用更轻的奇对称曲线，纯 Side 不会生出中央内容；新增高频谐波过量时按比例自动收敛（最低 0.35）。</text>
    </svg>
  ),

  clay: () => (
    <svg viewBox="0 0 760 300" className="w-full h-auto" role="img" aria-label="Clay 修坯示意图">
      <Defs />
      <Box x={20} y={120} w={90} h={50} title="L / R" tone="g" />
      <Box x={150} y={120} w={130} h={50} title="① 低切" sub="4 阶 Butterworth · 10 ～ 60 Hz" />
      <Box x={320} y={40} w={130} h={50} title="M = (L+R)/2" sub="不动" tone="g" />
      <Box x={320} y={200} w={130} h={50} title="S = (L−R)/2" tone="g" />
      <Box x={490} y={200} w={120} h={50} title="② 收单" sub="2 阶高通 · 线性相位" />
      <Box x={640} y={200} w={100} h={50} title="③ 加宽" sub="2 kHz 高架" />
      <Box x={640} y={40} w={100} h={50} title="L' / R'" tone="g" />
      <Arrow x1={110} y1={145} x2={148} y2={145} />
      <Arrow x1={280} y1={135} x2={318} y2={70} />
      <Arrow x1={280} y1={155} x2={318} y2={220} />
      <Arrow x1={450} y1={225} x2={488} y2={225} />
      <Arrow x1={610} y1={225} x2={638} y2={225} />
      <Arrow x1={450} y1={65} x2={638} y2={65} />
      <Arrow x1={690} y1={200} x2={690} y2={92} />
      <Box x={150} y={230} w={130} h={44} title="峰值守护" sub="前瞻 8 ms · 会抬峰值就让开" tone="v" />
      <Arrow x1={215} y1={230} x2={215} y2={172} dashed color="#6A4DFF" />
      <text x="20" y="292" style={small}>② ③ 只动 S：单声道回放（L + R = 2M）完全不变。三项实际值都为 0 时逐位直通。收单用线性相位，不转动低频相位；低切会转相位，所以守护在峰值处让一点。</text>
    </svg>
  ),

  glaze: () => (
    <svg viewBox="0 0 760 320" className="w-full h-auto" role="img" aria-label="GLAZE 状态机示意图">
      <Defs />
      <Box x={20} y={40} w={120} h={54} title="未学习" sub="播放后自动开始" tone="g" />
      <Box x={190} y={40} w={140} h={54} title="聆听中" sub="15 ～ 30 s · 攒证据" tone="v" />
      <Box x={380} y={40} w={140} h={54} title="校准中" sub="对齐输入输出响度差" tone="v" />
      <Box x={570} y={40} w={150} h={54} title="已锁定" sub="基准附近有界保护" />
      <Arrow x1={140} y1={67} x2={188} y2={67} />
      <Arrow x1={330} y1={67} x2={378} y2={67} />
      <Arrow x1={520} y1={67} x2={568} y2={67} />
      <Box x={570} y={150} w={150} h={54} title="受限" sub="约束持续超出 · 慢速退让" tone="g" />
      <Arrow x1={645} y1={94} x2={645} y2={148} dashed />
      <Arrow x1={620} y1={148} x2={620} y2={96} dashed />
      <text x="655" y="126" style={{ ...small, fill: '#C2410C' }}>g_保护 ∈ [−3, 0] dB</text>
      <Box x={190} y={150} w={330} h={54} title="重新聆听 → 候选版本" sub="旧方案照常生效；听完约 3 s 平滑换过去" tone="v" />
      <Arrow x1={570} y1={175} x2={522} y2={175} />
      <Arrow x1={355} y1={150} x2={355} y2={96} dashed color="#6A4DFF" />
      <Box x={20} y={240} w={700} h={54} title="随工程保存：画像 + 方案 + 校准 + 标定快照 + 执行快照" sub="重开不重学；从同一起点重渲染逐位可复现。校准参考条件（手动参数、采样率、高级设置）一变即失效。" tone="g" />
      <text x="20" y="130" style={small}>决策器是纯函数：画像 + 风格 + GLAZE + 手动值 + 标定快照 → 方案。GLAZE 0 时增量全为 0。</text>
    </svg>
  ),

  latency: () => (
    <svg viewBox="0 0 760 200" className="w-full h-auto" role="img" aria-label="延迟构成示意图">
      {[
        { x: 40, w: 0, t: 'BALANCE', v: '0' },
        { x: 40, w: 386, t: 'CLAY', v: '1102 = D 574 + G 528' },
        { x: 426, w: 22, t: 'COLOR', v: '64' },
        { x: 448, w: 76, t: 'LIMIT', v: '218 = 60 + 96 + 62' }
      ].map((s, i) => (
        <g key={i}>
          <rect x={s.x} y="70" width={Math.max(s.w, 2)} height="36" fill={['#DCE6E9', '#67CAD7', '#33B3C4', '#127F90'][i]} />
          <text x={s.x + Math.max(s.w, 2) / 2} y="62" textAnchor="middle" style={{ ...mono, fontSize: 10 }}>{s.t}</text>
          <text x={s.x + Math.max(s.w, 2) / 2} y="124" textAnchor="middle" style={{ ...small, fontSize: 9.5 }}>{s.v}</text>
        </g>
      ))}
      <text x="540" y="94" style={{ ...label, fontWeight: 600 }}>= 1384 样本 ≈ 28.8 ms</text>
      <text x="40" y="160" style={small}>48 kHz、Lookahead 2 ms。96 kHz 为 2584、192 kHz 为 4984 样本。Lookahead 停稳 0.3 s 后才换前瞻并重报延迟；宿主自动补偿。宿主旁通走同一条链，延迟不变。</text>
    </svg>
  )
};
