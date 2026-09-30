export interface PageMeta {
  /** 路由末段；'' = 该分区首页 */
  slug: string;
  title: string;
  summary: string;
  /** Markdown 文件名（不含扩展名） */
  file: string;
}
export interface NavGroup {
  group: string;
  pages: PageMeta[];
}

export const docsNav: NavGroup[] = [
  {
    group: '开始',
    pages: [
      { slug: '', title: '快速上手', summary: '插到总轨、播放、拧 GLAZE——三步。', file: 'quickstart' },
      { slug: 'install', title: '安装与兼容', summary: 'macOS 通用二进制、Windows x64、宿主、延迟。', file: 'install' },
      { slug: 'interface', title: '界面总览', summary: '顶栏、表、频谱仪、GLAZE、四张卡片各是什么。', file: 'interface' }
    ]
  },
  {
    group: '处理',
    pages: [
      { slug: 'glaze', title: 'GLAZE 与风格', summary: '大旋钮的含义、四种风格、聆听与重新聆听。', file: 'glaze' },
      { slug: 'balance', title: 'BALANCE 卡片', summary: '回补、抑制、提亮、速度与 27 段修正曲线。', file: 'balance' },
      { slug: 'clay', title: 'CLAY 卡片', summary: '低切、收单、加宽各自的自动 / 关 / 手动。', file: 'clay' },
      { slug: 'color', title: 'COLOR 卡片', summary: '音调、粘合、热度、干湿、输出。', file: 'color' },
      { slug: 'limit', title: 'LIMIT 卡片', summary: '增益、上限、削波、释放、预读与试听差值。', file: 'limit' },
      { slug: 'advanced', title: '高级面板', summary: '推响不刺、目标频谱与参考学习、发行安全。', file: 'advanced' }
    ]
  },
  {
    group: '判断',
    pages: [
      { slug: 'listening', title: '监听方式', summary: 'A / B、等响、试听差值、归一化预听。', file: 'listening' },
      { slug: 'meters', title: '表与频谱仪', summary: '每个读数是什么、该看多少。', file: 'meters' },
      { slug: 'controls', title: '手感与键位', summary: '拖动、精调、双击、直接输入、缩放、语言。', file: 'controls' },
      { slug: 'faq', title: '常见问题', summary: '声音没变、校准丢了、宿主没看到新版本……', file: 'faq' }
    ]
  }
];

export const algoNav: NavGroup[] = [
  {
    group: '总览',
    pages: [
      { slug: '', title: '信号链与设计原则', summary: '固定顺序、延迟补偿旁通、零差、逐位契约、真峰值口径。', file: 'overview' },
      { slug: 'references', title: '参考与边界', summary: '三款参考插件的公开能力，与刻意没做的。', file: 'references' }
    ]
  },
  {
    group: '四道工序',
    pages: [
      { slug: 'balance', title: 'BALANCE · 动态频谱平衡', summary: '27 段、参考轮廓、死区、自动许可、上行掩蔽转移。', file: 'balance' },
      { slug: 'clay', title: 'CLAY · 修坯', summary: 'M/S 三级、先试后用、峰值守护。', file: 'clay' },
      { slug: 'color', title: 'COLOR · 染色', summary: 'Tone、分频 Glue、保留低频主体的 Heat。', file: 'color' },
      { slug: 'limit', title: 'LIMIT · 真峰值限幅', summary: '8× 检测 1× 施加、双包络、三角攻击沿、周期释放。', file: 'limit' }
    ]
  },
  {
    group: '编排与验证',
    pages: [
      { slug: 'glaze', title: 'GLAZE · 决策与校准', summary: '画像 → 方案 → 校准 → 保护；存档与重开。', file: 'glaze' },
      { slug: 'verification', title: '性能与验证', summary: '预算口径、实测数字、测试与逐位契约的编译前提。', file: 'verification' }
    ]
  }
];

export function flatten(nav: NavGroup[]): PageMeta[] {
  return nav.flatMap(g => g.pages);
}

// Markdown 正文按需加载（每页一个小包）。
const docsFiles = import.meta.glob('./docs/*.md', { query: '?raw', import: 'default' }) as Record<string, () => Promise<string>>;
const algoFiles = import.meta.glob('./algorithms/*.md', { query: '?raw', import: 'default' }) as Record<string, () => Promise<string>>;

export function loadMarkdown(kind: 'docs' | 'algorithms', file: string): Promise<string> {
  const table = kind === 'docs' ? docsFiles : algoFiles;
  const loader = table[`./${kind}/${file}.md`];
  if (!loader) return Promise.reject(new Error(`没有这一页：${kind}/${file}`));
  return loader();
}
