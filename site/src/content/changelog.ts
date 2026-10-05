/**
 * 更新日志：/changelog 页的唯一数据源，新的在上。
 *
 * 版本号就是插件顶栏显示的四段号（Celadon.jucer）；号码不单调，顺序按发布时间排。
 * 每版 3～5 条使用者听得见、用得着的变化：只写效果，不写内部实现名与提交号，不提任何别家插件。
 * 素材在档案 05-版本/ 的各版本档；出新版时在最上面加一条，version 与 PLUGIN_VERSION 相同的那条会标「当前版本」。
 * 文字一律中英两份（ADR-024，英文用词照 src/i18n/glossary.md）；类别的显示名在 i18n 的 changelog 命名空间 kinds.*。
 */
import type { Locale } from '@/i18n/paths';

export type ChangeKind = 'new' | 'sound' | 'ui' | 'compat' | 'perf' | 'fix';

/** 一段对外文字的两种语言 */
export type Text = Record<Locale, string>;

export interface ChangeItem {
  kind: ChangeKind;
  text: Text;
}

export interface ReleaseNote {
  /** 页内锚点，/changelog#<id> 可直接定位 */
  id: string;
  /** 四段版本号；合并的早期条目为 null，显示 label */
  version: string | null;
  label?: Text;
  /** YYYY-MM-DD；合并条目给 range（按语言写区间） */
  date: string;
  range?: Text;
  /** 版本号下面的一句小字（如「内部开发版」） */
  note?: Text;
  title: Text;
  items: ChangeItem[];
}

export const CHANGELOG: ReleaseNote[] = [
  {
    id: 'v0.1.8.8',
    version: '0.1.8.8',
    date: '2026-10-05',
    title: { zh: '曲风与交付，修坯更谨慎', en: 'Genre and Delivery, and a more careful CLAY' },
    items: [
      {
        kind: 'new',
        text: {
          zh: 'GLAZE 左边新增「曲风」：选 Deep / Tech / Melodic（House 的三个子类）时，按子类定音色，低切到 25 Hz、不自动加宽，GLAZE 按小节聆听、听到主段落（drop）才锁定；选「通用」沿用原来的方式。',
          en: 'A new Genre choice sits to the left of GLAZE. Pick Deep, Tech or Melodic (the three House sub-genres) and the tone is set per sub-genre, the low cut sits at 25 Hz, widening is never automatic, and GLAZE learns bar by bar and locks only after it has heard the main section (the drop). General works as before.'
        }
      },
      {
        kind: 'new',
        text: {
          zh: 'GLAZE 右边新增「交付」：「标准」沿用原来的响度；「流媒体」约 −10 LUFS、真峰值不高于 −2 dBTP；「俱乐部」约 −7 LUFS，给 DJ 和下载店。同一首歌每档各导出一次，就得到各档的母带。',
          en: 'A new Delivery choice sits to the right of GLAZE. Standard works as before; Streaming aims near −10 LUFS with true peak at or below −2 dBTP; Club aims near −7 LUFS for DJs and download stores. Export once per setting to get a master for each.'
        }
      },
      {
        kind: 'sound',
        text: {
          zh: 'CLAY 的自动低切更谨慎：聆听时会连「低音被抵消变薄」一起试，会让低音变薄或抬高整首峰值的歌，低切自动减量甚至不用；播放中出现同样的问题，也会逐档退让。',
          en: "CLAY's automatic Low cut is more careful: it now also checks whether the cut would thin your bass. Where it would thin the low end or push the whole song's peak up, the cut is reduced or skipped, and it backs off step by step if that shows up during playback."
        }
      },
      {
        kind: 'ui',
        text: {
          zh: 'CLAY 的灯更可信：低切或收单被峰值守护明显收回时，灯和数值变琥珀，悬停气泡写实际还剩约几成；低频只剩上方分音漏进来的段落，不再误亮反相提醒。',
          en: 'The CLAY lamps say more: when the peak guard clearly pulls back Low cut or Mono, the lamp and its value turn amber and the tooltip says roughly how much is left. Sections whose low end is only upper partials leaking down no longer light a false out-of-phase warning.'
        }
      },
      {
        kind: 'perf',
        text: {
          zh: '频谱仪更省 CPU：PEAK 关着时不再每帧重画；界面缩放不是整数倍时，也只重画变了的部分。',
          en: 'The spectrum analyser uses less CPU: with PEAK off it no longer redraws every frame, and at non-integer UI scales it redraws only what changed.'
        }
      }
    ]
  },
  {
    id: 'v0.1.8.7',
    version: '0.1.8.7',
    date: '2026-10-01',
    title: { zh: '频谱仪更顺、更跟手', en: 'A smoother, more responsive spectrum analyser' },
    items: [
      {
        kind: 'ui',
        text: {
          zh: '频谱跟着屏幕刷新走：60 Hz 屏每秒 60 帧、120 Hz 屏 120 帧（原来固定 30 帧），曲线动起来顺得多。',
          en: "The spectrum now follows your screen's refresh rate: 60 frames a second on a 60 Hz display, 120 on a 120 Hz one (previously a fixed 30), so the curve moves far more smoothly."
        }
      },
      {
        kind: 'ui',
        text: {
          zh: '更跟手：起音几乎一帧到位，回落更利落；宿主缓冲设得大时，曲线也不再「先掉一下再跳回来」。',
          en: 'More responsive: attacks land in about one frame and the fall-off is crisper; with a large host buffer, the curve no longer dips and then jumps back.'
        }
      },
      {
        kind: 'ui',
        text: {
          zh: '低频曲线变圆滑：夹在两个频点之间的峰画成圆包，顶的高度更接近真实电平；高频的读数口径与之前相同。',
          en: 'A smoother low-frequency curve: a peak that falls between two frequency bins is drawn as a rounded hump whose top sits closer to the true level; high-frequency readings work the same as before.'
        }
      },
      {
        kind: 'ui',
        text: {
          zh: '输出填充按响度上色：越响的地方颜色越亮，一眼看出哪一段最满。',
          en: "The output fill is coloured by loudness: the louder a region, the brighter it gets, so you can see at a glance where it's fullest."
        }
      }
    ]
  },
  {
    id: 'v0.1.8.6',
    version: '0.1.8.6',
    date: '2026-10-01',
    title: { zh: '更准、更稳的一轮打磨', en: 'A round of polish: more accurate, more stable' },
    items: [
      {
        kind: 'sound',
        text: {
          zh: '推响不刺只压真正刺耳的 2 ～ 8 kHz：音量突然变大、鼓点密集的段落不再被当成刺耳压下去。COLOR 的 Glue 中频也压得更贴近设定，不再多压一截。',
          en: "“Loud, not harsh” now only tames genuinely harsh 2–8 kHz content: sudden jumps in level and busy drum passages are no longer mistaken for harshness and pulled down. COLOR's Glue mid band also tracks the setting more closely instead of compressing extra."
        }
      },
      {
        kind: 'sound',
        text: {
          zh: 'CLAY 的加宽设多少就是多少：以前峰值守护会把大部分加宽收回去。旧工程里开着加宽的，打开后声场会比以前宽。',
          en: "CLAY's Width now delivers what you set: previously the peak guard took most of the widening back. Old projects with Width on will sound wider than before."
        }
      },
      {
        kind: 'sound',
        text: {
          zh: 'GLAZE 只在 Heat 真能削掉峰值时才加它，新聆听的歌饱和更克制、更干净。已经聆听过的旧工程保持原来的决策，点「重新聆听」后才用新规则。',
          en: 'GLAZE only adds Heat when it can genuinely shave peaks, so newly learned songs get more restrained, cleaner saturation. Projects that were already learned keep their original decisions until you click Relearn.'
        }
      },
      {
        kind: 'fix',
        text: {
          zh: '临时关掉 GLAZE、旁通 LIMIT 或换风格再切回后会重新校准，不再停在没校准的状态；换风格后保护上限跟着换；静音、停播、宿主旁通时保护不会误动作。归一化预听在退出或「重新计量」时音量不再跳一下，听原声时 OUT 的整体响度读数也不再被拉低。',
          en: 'After briefly turning GLAZE off, bypassing LIMIT or switching styles and back, it recalibrates instead of staying uncalibrated; the protection limits follow a style change; and protection no longer acts during silence, stopped playback or host bypass. Normalisation preview no longer jumps in level when you stop it or press Reset meters, and listening to the original no longer drags down the OUT integrated loudness reading.'
        }
      },
      {
        kind: 'ui',
        text: {
          zh: '主旋钮的瓷面更清晰；粒子带与频谱仪只重画变了的部分，拖动 GLAZE、在频谱上悬停时更省资源；176.4 / 192 kHz 下频谱仪反应更快。',
          en: "The main knob's porcelain looks sharper; the particle band and spectrum analyser redraw only what changed, so dragging GLAZE or hovering over the spectrum takes fewer resources; the analyser responds faster at 176.4 / 192 kHz."
        }
      }
    ]
  },
  {
    id: 'v0.1.8.5',
    version: '0.1.8.5',
    date: '2026-09-29',
    title: { zh: '高级母带控制，三个平台都有安装包', en: 'Advanced mastering controls, and installers for all three platforms' },
    items: [
      {
        kind: 'new',
        text: {
          zh: '「高级」面板的三个页签能用了：推响不刺（压住 2 ～ 8 kHz 的刺耳，鼓点照样留住）、目标频谱（温暖 / 均衡 / 明亮，或从侧链学一首参考曲的频谱形状，不学响度）、发行安全（归一化预听、编码余量、TPDF 抖动）。',
          en: "All three tabs of the Advanced panel now work: “Loud, not harsh” (tames harshness at 2–8 kHz while keeping the drums), Target spectrum (Warm / Neutral / Bright, or learn a reference track's spectral shape from the sidechain — the shape, not the loudness) and Release safety (normalisation preview, codec margin, TPDF dither)."
        }
      },
      {
        kind: 'compat',
        text: {
          zh: 'Windows 10 / 11（64 位）与 Intel 芯片的 Mac 有了正式安装包；Mac 按芯片分两个包。Intel 上 BALANCE 与 COLOR 用上更快的指令，更省 CPU，声音不变。',
          en: 'Proper installers for Windows 10 / 11 (64-bit) and Intel Macs; the Mac version comes as two installers split by chip. On Intel, BALANCE and COLOR use faster instructions — less CPU, identical sound.'
        }
      },
      {
        kind: 'fix',
        text: {
          zh: '重开工程后 GLAZE 的校准不会再丢，打开就接着用，不用重新校准。',
          en: 'GLAZE no longer loses its calibration when you reopen a project — it picks up where it left off, no recalibration needed.'
        }
      },
      {
        kind: 'ui',
        text: {
          zh: '四张卡片按真实处理顺序排成 BALANCE → CLAY → COLOR → LIMIT；字、旋钮、按钮都加大；快捷键提示按系统显示（Mac ⌘ / Option，Windows Ctrl / Alt）。',
          en: 'The four cards now follow the real processing order, BALANCE → CLAY → COLOR → LIMIT; text, knobs and buttons are larger; shortcut hints match your system (⌘ / Option on Mac, Ctrl / Alt on Windows).'
        }
      },
      {
        kind: 'compat',
        text: {
          zh: '旧工程打开后，新加的推响不刺、目标频谱、编码余量、抖动一律关着，声音与从前一样；新建的实例才默认「自动」。',
          en: 'When you open an old project, the new “Loud, not harsh”, Target spectrum, codec margin and dither are all off, so it sounds exactly as before; only new instances default to Auto.'
        }
      }
    ]
  },
  {
    id: 'v0.1.8.0',
    version: '0.1.8.0',
    date: '2026-09-28',
    title: { zh: '新界面：浅色青瓷', en: 'New interface: light celadon' },
    items: [
      {
        kind: 'ui',
        text: {
          zh: '界面整个重做：浅色单色青、瓷器质感的 GLAZE 主旋钮、实时频谱仪，四道工序各一张卡片；窗口可在 70 % ～ 200 % 之间缩放。',
          en: 'A complete interface redesign: a light, single-hue celadon palette, a porcelain GLAZE knob, a live spectrum analyser and one card per stage; the window scales from 70% to 200%.'
        }
      },
      {
        kind: 'new',
        text: {
          zh: '中文 / English 一键切换，每个控件的悬停说明也跟着换。',
          en: "Switch between Chinese and English in one click — every control's hover help switches too."
        }
      },
      {
        kind: 'new',
        text: {
          zh: 'A / B 听原声：与宿主旁通走同一条干路，延迟对齐、20 ms 交叉淡化；打开等响时两边只衰减、不放大。',
          en: 'A / B against the original: it uses the same dry path as host bypass, latency-aligned with a 20 ms crossfade; with Match on, either side is only ever attenuated, never boosted.'
        }
      },
      {
        kind: 'new',
        text: {
          zh: 'LIMIT 卡片上的耳机「试听差值」：只听被削掉的那部分，鼓点有没有变扁、低音有没有发闷，一听就知道。',
          en: "Listen to the difference — the headphones on the LIMIT card: hear only what's being removed, and you'll know straight away whether the drums went flat or the bass got muddy."
        }
      }
    ]
  },
  {
    id: 'v0.1.7.2',
    version: '0.1.7.2',
    date: '2026-09-28',
    title: { zh: '三道工序的听感打磨', en: 'Sound polish across three stages' },
    items: [
      {
        kind: 'sound',
        text: {
          zh: 'BALANCE：回补与抑制同时打开时，压下一个过于突出的频段，会顺带把上方被它遮住、仍有内容的邻段稍稍提起来；净增益拉回 0 附近，谱本来就平的歌声音不变。',
          en: 'BALANCE: with Recover and Tame both on, pulling down an overly prominent band now also lifts the neighbour above it that it was masking, as long as that band still has content; net gain is brought back near 0, so songs with an already flat spectrum sound the same.'
        }
      },
      {
        kind: 'sound',
        text: {
          zh: 'LIMIT：攻击沿更圆滑；鼓点这类短于约 70 ms 的过载过去后，增益回得更快，持续的过载照样压稳。只要限幅器在工作，声音就会有细微变化。',
          en: 'LIMIT: a smoother attack; after short overloads under about 70 ms, such as drum hits, gain recovers faster, while sustained overloads are held as firmly as before. Whenever the limiter is working, the sound changes subtly.'
        }
      },
      {
        kind: 'sound',
        text: {
          zh: 'COLOR 的粘合分低、中、高三段施压：人声一响，低音不再跟着一起被压下去。',
          en: "COLOR's Glue now presses low, mid and high separately: when the vocal comes in, the bass no longer gets pushed down with it."
        }
      },
      {
        kind: 'perf',
        text: {
          zh: 'CLAY 的峰值守护重写：修坯这一级更省 CPU（x86 上快 1.3 ～ 2 倍），输出逐位不变。',
          en: "CLAY's peak guard was rewritten: the stage uses less CPU (1.3–2× faster on x86) with bit-identical output."
        }
      }
    ]
  },
  {
    id: 'v0.1.7.1',
    version: '0.1.7.1',
    date: '2026-09-28',
    title: { zh: '修坯更自然，校准不再原地打转', en: 'A more natural CLAY, and calibration that no longer spins in place' },
    items: [
      {
        kind: 'sound',
        text: {
          zh: 'CLAY 不再自动把低频收成单声道——试听下来不自然；低频反相时只亮灯提示，需要时仍可手动收单。',
          en: "CLAY no longer folds the lows to mono automatically — it didn't sound natural in listening tests; low-frequency phase problems now only light up a warning, and you can still apply Mono by hand when needed."
        }
      },
      {
        kind: 'fix',
        text: {
          zh: '响的歌不会再整首停在「校准中」：GLAZE 碰到保护层之后接着用已经做好的校准，不再从零重来。',
          en: 'Loud songs no longer sit in Calibrating for the whole track: after GLAZE hits its protection layer, it keeps the calibration it already has instead of starting from scratch.'
        }
      },
      {
        kind: 'perf',
        text: {
          zh: '拖旋钮或方案切换时，修坯换分频点不再有瞬间的 CPU 尖峰（192 kHz、32 样本缓冲下，那一块从多约 18 µs 降到 0.4 µs）。',
          en: "Changing CLAY's crossover while you drag a knob or the plan switches no longer causes a momentary CPU spike (at 192 kHz with a 32-sample buffer, that block's extra cost dropped from about 18 µs to 0.4 µs)."
        }
      }
    ]
  },
  {
    id: 'v0.1.7.0',
    version: '0.1.7.0',
    date: '2026-09-27',
    title: { zh: '第四道工序：修坯 CLAY', en: 'The fourth stage: CLAY' },
    items: [
      {
        kind: 'new',
        text: {
          zh: 'CLAY 接在 BALANCE 之后：低切只清 16 Hz 以下的次声；收单在单声道回放会丢低音时收拢低频；加宽只给偏窄的歌、只加高频，并盯住相关度防发虚。全自动，随 GLAZE 走。',
          en: 'CLAY sits after BALANCE: Low cut only clears infrasonics below 16 Hz; Mono folds the lows when mono playback would lose bass; Width only widens narrow songs, only in the highs, and watches correlation so nothing turns hollow. Fully automatic, following GLAZE.'
        }
      },
      {
        kind: 'sound',
        text: {
          zh: '先试后用：聆听时先把修坯在这首歌上试一遍，会抬高峰值的项自动减量或跳过，悬停说明里写着原因。',
          en: 'Try before applying: while listening, CLAY is trial-run on the song first; anything that would raise the peaks is reduced or skipped automatically, with the reason shown in the hover help.'
        }
      },
      {
        kind: 'sound',
        text: {
          zh: '峰值守护：修坯从不让峰值高过原信号；整首都要出手时，自动修坯逐档减量。',
          en: 'Peak guard: CLAY never lets the peaks exceed the original signal; if it would have to step in throughout the song, automatic CLAY backs off step by step.'
        }
      },
      {
        kind: 'compat',
        text: {
          zh: '插件延迟变长，48 kHz、预读 2 ms 时约 28.8 ms，宿主会自动补偿。打开 0.1.6.2 的工程时修坯先不动，点「重新聆听」后启用。',
          en: 'Plugin latency increased to about 28.8 ms at 48 kHz with 2 ms lookahead; hosts compensate automatically. In projects from 0.1.6.2, CLAY stays inactive until you click Relearn.'
        }
      },
      {
        kind: 'fix',
        text: {
          zh: 'GLAZE 的保护层不会再卡在约 −0.9 dB 收不回来。',
          en: "GLAZE's protection layer no longer gets stuck at about −0.9 dB."
        }
      }
    ]
  },
  {
    id: 'v0.1.6.2',
    version: '0.1.6.2',
    date: '2026-09-26',
    title: { zh: '一个旋钮的自动母带', en: 'One-knob automatic mastering' },
    items: [
      {
        kind: 'new',
        text: {
          zh: 'GLAZE 默认 35 %：新实例插上就开始工作；旧工程里没存 GLAZE 的，仍从 0 开始。',
          en: "GLAZE defaults to 35%: new instances start working as soon as they're inserted; old projects with no saved GLAZE value still start at 0."
        }
      },
      {
        kind: 'sound',
        text: {
          zh: '热度（Heat）换了做法：原声完整保留，只把染色多出来的部分加回去。高频不再发刺，低音也不再搅浑中高频——1 kHz 满档的失真从 39.8 % 降到 2.7 %。',
          en: 'Heat works differently: the original is kept intact and only the extra colouring is added back. The highs no longer get spiky and the bass no longer muddies the mids and highs — distortion at 1 kHz on full Heat fell from 39.8% to 2.7%.'
        }
      },
      {
        kind: 'sound',
        text: {
          zh: '自动处理跟着段落走：主歌、副歌的密度和亮度不同，音调、抑制、热度、粘合的自动份额随之慢慢调整，不碰你手动拧的部分。',
          en: "Automatic processing follows the song's sections: verses and choruses differ in density and brightness, so the automatic share of Tone, Tame, Heat and Glue slowly adjusts with them — without touching what you've set by hand."
        }
      },
      {
        kind: 'sound',
        text: {
          zh: '等响改为测整条链的实际响度差，只衰减、不放大；抑制上限从 6 dB 提到 8 dB，提亮单独打开也生效。',
          en: "Match now measures the real loudness difference across the whole chain and only attenuates, never boosts; Tame's maximum cut rose from 6 dB to 8 dB, and Brighten works on its own too."
        }
      },
      {
        kind: 'fix',
        text: {
          zh: 'LIMIT 在宽带素材上偶尔比上限高出最多 0.3 dB：限幅时多留 0.35 dB 余量，现在最坏也低于上限。',
          en: 'LIMIT could occasionally overshoot the ceiling by up to 0.3 dB on broadband material; it now keeps an extra 0.35 dB of headroom, so even the worst case stays below the ceiling.'
        }
      }
    ]
  },
  {
    id: 'earlier',
    version: null,
    label: { zh: '更早', en: 'Earlier' },
    date: '2026-09-24',
    range: { zh: '2026-09-24 ～ 26', en: 'Sep 24–26, 2026' },
    note: { zh: '内部开发版', en: 'Internal builds' },
    title: { zh: '从一个限幅器开始', en: 'It started with a limiter' },
    items: [
      {
        kind: 'new',
        text: {
          zh: 'LIMIT 真峰值限幅：前瞻、8 倍插值检测，不超上限、不咔哒；带响度、真峰值、限幅衰减三块表。',
          en: 'LIMIT true-peak limiting: lookahead with 8× interpolated detection, never above the ceiling and click-free, with loudness, true-peak and gain-reduction meters.'
        }
      },
      {
        kind: 'new',
        text: {
          zh: '第一版界面与 COLOR（音调、粘合、热度、干湿）；每个控件都有三行悬停说明：作用、效果、建议。',
          en: "The first interface and COLOR (Tone, Glue, Heat, Mix); every control has three lines of hover help: what it does, what you'll hear, and where to start."
        }
      },
      {
        kind: 'new',
        text: {
          zh: 'BALANCE：27 段动态频谱平衡，把突出的压下去、被遮住的提起来，零延迟。',
          en: "BALANCE: 27-band dynamic spectral balance that pulls down what sticks out and lifts what's masked, with zero latency."
        }
      },
      {
        kind: 'new',
        text: {
          zh: 'GLAZE 从「几个旋钮一起拧大」改成「先听这首歌再决定」：聆听 15 ～ 30 秒，按响度、动态、频谱、起音定方案，四种风格可选；FL Studio 的 Pattern 循环里也能正常校准。',
          en: "GLAZE went from turning several knobs up together to listening to the song before deciding: it listens for 15–30 seconds and plans by loudness, dynamics, spectrum and attacks, with four styles to choose from; it also calibrates correctly inside FL Studio's Pattern loop."
        }
      },
      {
        kind: 'perf',
        text: {
          zh: '性能全面优化：48 kHz 下单核占用从约 8 % 降到 1 % 左右。',
          en: 'Across-the-board performance work: single-core load at 48 kHz fell from about 8% to around 1%.'
        }
      }
    ]
  }
];
