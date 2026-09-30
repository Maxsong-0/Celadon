import { useCallback, useEffect, useId, useMemo, useRef, useState, type KeyboardEvent } from 'react';
import { Em, LinkArrow, SectionHead } from './Eyebrow';
import { useReveal } from '@/lib/reveal';
import { cn } from '@/lib/utils';
import { MATCH_LUFS, tracks, type ListenTrack, type ListenVersion } from '@/content/listen';

type Which = 'original' | 'celadon';
const BASE = import.meta.env.BASE_URL;
const FADE = 0.025; // 切换 A / B 的交叉淡化（秒）：够短听不出空档，够长没有咔哒

/** 肆 · 试听：同一段音乐的原声与 Celadon 两版，默认等响；清单为空时整节不显示。 */
export default function Listen() {
  const root = useRef<HTMLElement>(null);
  useReveal(root);
  if (!tracks.length) return null;
  return (
    <section id="listen" ref={root} className="section relative">
      <div className="mx-auto max-w-[1280px] px-5 sm:px-8">
        <SectionHead
          chapter={{ n: '肆', label: '试听', en: 'Listen' }}
          title={
            <>
              同一首歌，<Em>等响</Em>对比。
            </>
          }
          lead={`原声与 Celadon 两版逐样本对齐，默认都对齐到 ${MATCH_LUFS} LUFS 播放：只比声音，不比音量。切换时位置不变；关掉「等响」听真实的响度差。建议戴耳机。`}
        />
        <div data-reveal className="mt-16">
          <Player />
        </div>
        <p data-reveal className="mt-6 text-[13px] text-ink-4">
          曲目均为 Max 的原创作品，仅供试听。页面里是 AAC 压缩后的片段；插件本身的处理不经过任何压缩。
        </p>
      </div>
    </section>
  );
}

interface Engine {
  ctx: AudioContext;
  gains: Record<Which, GainNode>;
  sources: AudioBufferSourceNode[];
  startedAt: number;
  offset: number;
}

/** 关等响时：较响的一版放在对齐电平，较轻的一版按真实差距再压低——只衰减，绝不把谁推过 0 dBFS。 */
function levelOf(track: ListenTrack, which: Which, matched: boolean) {
  if (matched) return 1;
  const louder = Math.max(track.original.lufs, track.celadon.lufs);
  return Math.pow(10, (track[which].lufs - louder) / 20);
}

function fmt(t: number) {
  const s = Math.max(0, Math.floor(t));
  return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;
}

function Player() {
  const [index, setIndex] = useState(0);
  const [which, setWhich] = useState<Which>('celadon');
  const [matched, setMatched] = useState(true);
  const [playing, setPlaying] = useState(false);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [position, setPosition] = useState(0);
  const engine = useRef<Engine | null>(null);
  const buffers = useRef(new Map<string, Record<Which, AudioBuffer>>());
  const raf = useRef(0);
  const track = tracks[index];
  const duration = track.seconds;
  const labelId = useId();

  // 读最新状态给音频回调用（回调里不订阅渲染）
  const live = useRef({ which, matched, track });
  live.current = { which, matched, track };

  const stopSources = useCallback(() => {
    const e = engine.current;
    if (!e) return;
    for (const s of e.sources) {
      s.onended = null;
      try {
        s.stop();
      } catch {
        /* 已经停了 */
      }
      s.disconnect();
    }
    e.sources = [];
    cancelAnimationFrame(raf.current);
  }, []);

  const load = useCallback(async (t: ListenTrack, ctx: AudioContext) => {
    const cached = buffers.current.get(t.id);
    if (cached) return cached;
    const get = async (v: ListenVersion) => {
      const res = await fetch(`${BASE}${v.src}`);
      if (!res.ok) throw new Error(`${res.status}`);
      return ctx.decodeAudioData(await res.arrayBuffer());
    };
    const [original, celadon] = await Promise.all([get(t.original), get(t.celadon)]);
    const pair = { original, celadon };
    buffers.current.set(t.id, pair);
    return pair;
  }, []);

  const start = useCallback(
    async (from: number) => {
      setError(null);
      const Ctx = window.AudioContext || (window as unknown as { webkitAudioContext: typeof AudioContext }).webkitAudioContext;
      if (!engine.current) {
        const ctx = new Ctx();
        const gains = { original: ctx.createGain(), celadon: ctx.createGain() };
        gains.original.connect(ctx.destination);
        gains.celadon.connect(ctx.destination);
        engine.current = { ctx, gains, sources: [], startedAt: 0, offset: 0 };
      }
      const e = engine.current;
      if (e.ctx.state === 'suspended') await e.ctx.resume();
      setLoading(true);
      let pair: Record<Which, AudioBuffer>;
      try {
        pair = await load(live.current.track, e.ctx);
      } catch {
        setLoading(false);
        setError('音频没取到，稍后再试。');
        return;
      }
      setLoading(false);
      stopSources();
      const { which: w, matched: m, track: t } = live.current;
      const when = e.ctx.currentTime + 0.03;
      (['original', 'celadon'] as const).forEach(v => {
        e.gains[v].gain.cancelScheduledValues(0);
        e.gains[v].gain.setValueAtTime(v === w ? levelOf(t, v, m) : 0, e.ctx.currentTime);
        const s = e.ctx.createBufferSource();
        s.buffer = pair[v];
        s.connect(e.gains[v]);
        s.start(when, Math.min(from, pair[v].duration - 0.05));
        e.sources.push(s);
      });
      e.sources[0].onended = () => {
        // 自然播完：回到开头、停下
        if (engine.current?.sources.length) {
          stopSources();
          setPlaying(false);
          setPosition(0);
        }
      };
      e.startedAt = when;
      e.offset = from;
      setPlaying(true);
      const tick = () => {
        const cur = engine.current;
        if (!cur) return;
        setPosition(Math.min(duration, cur.offset + Math.max(0, cur.ctx.currentTime - cur.startedAt)));
        raf.current = requestAnimationFrame(tick);
      };
      raf.current = requestAnimationFrame(tick);
    },
    [duration, load, stopSources]
  );

  const pause = useCallback(() => {
    const e = engine.current;
    if (e) setPosition(Math.min(duration, e.offset + Math.max(0, e.ctx.currentTime - e.startedAt)));
    stopSources();
    setPlaying(false);
  }, [duration, stopSources]);

  // A / B 或等响变化：只改增益（交叉淡化），不重启音源
  useEffect(() => {
    const e = engine.current;
    if (!e || !playing) return;
    const now = e.ctx.currentTime;
    (['original', 'celadon'] as const).forEach(v => {
      const g = e.gains[v].gain;
      g.cancelScheduledValues(now);
      g.setValueAtTime(g.value, now);
      g.linearRampToValueAtTime(v === which ? levelOf(track, v, matched) : 0, now + FADE);
    });
  }, [which, matched, playing, track]);

  // 换曲：停下、回到开头
  useEffect(() => {
    stopSources();
    setPlaying(false);
    setPosition(0);
  }, [index, stopSources]);

  useEffect(
    () => () => {
      stopSources();
      engine.current?.ctx.close();
      engine.current = null;
    },
    [stopSources]
  );

  const seek = (t: number) => {
    const to = Math.max(0, Math.min(duration - 0.05, t));
    setPosition(to);
    if (playing) void start(to);
  };

  const onKey = (e: KeyboardEvent<HTMLDivElement>) => {
    if (e.target instanceof HTMLInputElement) return;
    if (e.key === 'b' || e.key === 'B') {
      setWhich(w => (w === 'celadon' ? 'original' : 'celadon'));
      e.preventDefault();
    } else if (e.key === ' ' && !(e.target instanceof HTMLButtonElement)) {
      if (playing) pause();
      else void start(position);
      e.preventDefault();
    }
  };

  const version = track[which];
  const diff = track.celadon.lufs - track.original.lufs;

  return (
    <div className="slab" onKeyDown={onKey}>
      <div className="slab-core p-5 sm:p-8">
        {tracks.length > 1 ? (
          <div role="tablist" aria-label="曲目" className="mb-6 flex flex-wrap gap-x-8 gap-y-2 border-b border-line">
            {tracks.map((t, i) => (
              <button
                key={t.id}
                role="tab"
                type="button"
                aria-selected={i === index}
                onClick={() => setIndex(i)}
                className={cn('-mb-px border-b-2 pb-3 text-left transition-colors duration-300', i === index ? 'border-c700 text-ink' : 'border-transparent text-ink-3 hover:text-ink')}
              >
                <span className="font-serif text-[17px] font-semibold">{t.title}</span>
                <span className="ml-2 text-[12px] text-ink-4">{t.note}</span>
              </button>
            ))}
          </div>
        ) : (
          <div className="mb-6 flex items-baseline gap-3">
            <span className="font-serif text-[18px] font-semibold text-ink">{track.title}</span>
            <span className="text-[12px] text-ink-4">{track.note}</span>
          </div>
        )}

        <Waveform peaks={version.peaks} progress={position / duration} onSeek={f => seek(f * duration)} labelledBy={labelId} />

        <div className="mt-6 flex flex-wrap items-center gap-x-6 gap-y-4">
          <button
            type="button"
            onClick={() => (playing ? pause() : void start(position))}
            aria-label={playing ? '暂停' : '播放'}
            className="grid size-14 place-items-center rounded-full bg-ink text-paper transition-transform duration-500 ease-glaze hover:scale-105 active:scale-95"
          >
            {loading ? (
              <span className="size-4 rounded-full border-2 border-paper/30 border-t-paper motion-safe:animate-spin" />
            ) : playing ? (
              <svg viewBox="0 0 20 20" className="size-5" fill="currentColor" aria-hidden="true">
                <rect x="5" y="4" width="3.5" height="12" rx="1" />
                <rect x="11.5" y="4" width="3.5" height="12" rx="1" />
              </svg>
            ) : (
              <svg viewBox="0 0 20 20" className="size-5 translate-x-px" fill="currentColor" aria-hidden="true">
                <path d="M6 4.2v11.6a.8.8 0 0 0 1.2.7l9.2-5.8a.8.8 0 0 0 0-1.4L7.2 3.5A.8.8 0 0 0 6 4.2Z" />
              </svg>
            )}
          </button>
          <span id={labelId} className="font-mono text-[13px] text-ink-3 tabular-nums">
            {fmt(position)} / {fmt(duration)}
          </span>

          <div role="group" aria-label="版本" className="inline-flex rounded-full bg-ink/5 p-1 ring-1 ring-ink/8">
            {(
              [
                ['original', 'B 原声'],
                ['celadon', 'A Celadon']
              ] as const
            ).map(([v, label]) => (
              <button
                key={v}
                type="button"
                aria-pressed={which === v}
                onClick={() => setWhich(v)}
                className={cn('rounded-full px-4 py-1.5 text-[13px] transition-colors duration-300', which === v ? 'bg-panel text-ink shadow-sm' : 'text-ink-3 hover:text-ink')}
              >
                {label}
              </button>
            ))}
          </div>

          <label className="inline-flex cursor-pointer items-center gap-2.5 text-[13px] text-ink-2">
            <input type="checkbox" checked={matched} onChange={e => setMatched(e.target.checked)} className="peer sr-only" />
            <span className="relative h-5 w-9 rounded-full bg-ink/15 transition-colors duration-300 peer-checked:bg-c700 peer-focus-visible:ring-2 peer-focus-visible:ring-c500 peer-focus-visible:ring-offset-2 after:absolute after:top-0.5 after:left-0.5 after:size-4 after:rounded-full after:bg-paper after:transition-transform after:duration-300 peer-checked:after:translate-x-4" aria-hidden="true" />
            等响
          </label>

          <dl className="ml-auto flex gap-6 text-[12px] text-ink-4">
            <div>
              <dt>响度</dt>
              <dd className="mt-0.5 font-mono text-[13px] text-ink-2 tabular-nums">{version.lufs.toFixed(1)} LUFS</dd>
            </div>
            <div>
              <dt>真峰值</dt>
              <dd className="mt-0.5 font-mono text-[13px] text-ink-2 tabular-nums">{version.truePeak.toFixed(1)} dBTP</dd>
            </div>
            <div>
              <dt>比原声</dt>
              <dd className="mt-0.5 font-mono text-[13px] text-ink-2 tabular-nums">{diff >= 0 ? '+' : '−'}{Math.abs(diff).toFixed(1)} dB</dd>
            </div>
          </dl>
        </div>

        <p className="mt-5 text-[12.5px] text-ink-4">
          {track.settings} · 片段 {fmt(track.from)} 起 {Math.round(track.seconds)} 秒 · 焦点在播放器里时按 <kbd className="font-mono">B</kbd> 切换、空格播放 / 暂停
        </p>
        {error ? <p className="mt-3 text-[13px] text-warn">{error}</p> : null}
      </div>
      <div className="px-6 pb-2 pt-4 sm:px-8">
        <LinkArrow to="/docs/listening">插件里怎么做等响 A / B</LinkArrow>
      </div>
    </div>
  );
}

/** 峰值包络：上下对称的细竖线；已播部分深、未播部分浅。只有两条 path，播放时只改裁切宽度。 */
function Waveform({ peaks, progress, onSeek, labelledBy }: { peaks: number[]; progress: number; onSeek: (f: number) => void; labelledBy: string }) {
  const clipId = useId().replace(/:/g, '');
  const W = 1000;
  const H = 120;
  const step = W / peaks.length;
  // 播放时每帧都会重渲染（进度在变），包络路径只在换曲 / 换版本时重算
  const d = useMemo(
    () => peaks.map((p, i) => `M${(i * step + step / 2).toFixed(1)} ${(H / 2 - Math.max(0.6, p * (H / 2 - 4))).toFixed(1)}V${(H / 2 + Math.max(0.6, p * (H / 2 - 4))).toFixed(1)}`).join(''),
    [peaks, step]
  );
  const pct = Math.max(0, Math.min(1, progress));
  return (
    <div
      role="slider"
      tabIndex={0}
      aria-labelledby={labelledBy}
      aria-valuemin={0}
      aria-valuemax={100}
      aria-valuenow={Math.round(pct * 100)}
      aria-valuetext={`${Math.round(pct * 100)} %`}
      className="relative cursor-pointer rounded-xl focus-visible:outline-2 focus-visible:outline-c500"
      onClick={e => {
        const r = e.currentTarget.getBoundingClientRect();
        onSeek((e.clientX - r.left) / r.width);
      }}
      onKeyDown={e => {
        if (e.key === 'ArrowRight') onSeek(pct + 0.05);
        else if (e.key === 'ArrowLeft') onSeek(pct - 0.05);
        else return;
        e.preventDefault();
        e.stopPropagation();
      }}
    >
      <svg viewBox={`0 0 ${W} ${H}`} preserveAspectRatio="none" className="block h-24 w-full sm:h-28" aria-hidden="true">
        <defs>
          <clipPath id={clipId}>
            <rect x="0" y="0" width={W * pct} height={H} />
          </clipPath>
        </defs>
        <path d={d} stroke="#C7D3D6" strokeWidth={step * 0.55} strokeLinecap="round" vectorEffect="non-scaling-stroke" />
        <path d={d} stroke="#127F90" strokeWidth={step * 0.55} strokeLinecap="round" clipPath={`url(#${clipId})`} vectorEffect="non-scaling-stroke" />
        <line x1={W * pct} x2={W * pct} y1="0" y2={H} stroke="#0D1B21" strokeOpacity="0.5" strokeWidth="1" vectorEffect="non-scaling-stroke" />
      </svg>
    </div>
  );
}
