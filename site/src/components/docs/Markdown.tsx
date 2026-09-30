import { type ReactNode, isValidElement, Children } from 'react';
import ReactMarkdown, { defaultUrlTransform, type Components } from 'react-markdown';
import remarkGfm from 'remark-gfm';
import { Link } from 'react-router-dom';
import { figures } from './Figures';

function textOf(node: ReactNode): string {
  if (typeof node === 'string' || typeof node === 'number') return String(node);
  if (Array.isArray(node)) return node.map(textOf).join('');
  if (isValidElement<{ children?: ReactNode }>(node)) return textOf(node.props.children);
  return '';
}

export function slugify(text: string) {
  return text
    .trim()
    .toLowerCase()
    .replace(/[`*_~]/g, '')
    .replace(/[^\p{L}\p{N}]+/gu, '-')
    .replace(/^-+|-+$/g, '');
}

const components: Components = {
  h2: ({ children }) => {
    const id = slugify(textOf(children));
    return (
      <h2 id={id} className="group scroll-mt-32">
        <a href={`#${id}`} className="no-underline text-inherit">
          {children}
        </a>
      </h2>
    );
  },
  h3: ({ children }) => {
    const id = slugify(textOf(children));
    return (
      <h3 id={id} className="scroll-mt-32">
        {children}
      </h3>
    );
  },
  a: ({ href = '', children }) => {
    if (href.startsWith('/')) {
      return <Link to={href}>{children}</Link>;
    }
    const external = /^https?:/.test(href);
    return (
      <a href={href} target={external ? '_blank' : undefined} rel={external ? 'noreferrer' : undefined}>
        {children}
      </a>
    );
  },
  img: ({ src = '', alt = '' }) => {
    if (typeof src === 'string' && src.startsWith('figure:')) {
      const key = src.slice('figure:'.length);
      const Fig = figures[key];
      if (!Fig) return <p className="warn">缺少示意图：{key}</p>;
      return (
        <figure className="my-10">
          <div className="bezel">
            <div className="bezel-core p-4 sm:p-6 overflow-hidden">
              <Fig />
            </div>
          </div>
          {alt ? <figcaption className="mt-3 text-center text-[13px] text-ink-4">{alt}</figcaption> : null}
        </figure>
      );
    }
    return <img src={src} alt={alt} loading="lazy" />;
  },
  blockquote: ({ children }) => {
    const text = textOf(children).trim();
    const warn = text.startsWith('⚠') || text.startsWith('注意');
    return <blockquote className={warn ? 'warn not-italic border-0 pl-5' : 'note not-italic border-0 pl-5'}>{children}</blockquote>;
  },
  table: ({ children }) => (
    <div className="my-6 -mx-1 overflow-x-auto">
      <table>{children}</table>
    </div>
  ),
  pre: ({ children }) => {
    // 只有一个 code 子节点时直接用 pre（保持样式）
    const only = Children.toArray(children)[0];
    return <pre>{only}</pre>;
  }
};

// react-markdown 默认只放行 http / https / mailto 等协议，会把 `figure:key` 清成空串（示意图全变坏图）。
// 只额外放行站内示意图的 figure: 前缀，其余照默认规则过滤。
function urlTransform(url: string) {
  return /^figure:[a-z0-9-]+$/.test(url) ? url : defaultUrlTransform(url);
}

export default function Markdown({ source }: { source: string }) {
  return (
    <div className="prose-celadon">
      <ReactMarkdown remarkPlugins={[remarkGfm]} components={components} urlTransform={urlTransform}>
        {source}
      </ReactMarkdown>
    </div>
  );
}
