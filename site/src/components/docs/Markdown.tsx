import { type ReactNode, isValidElement, Children, memo } from 'react';
import ReactMarkdown, { defaultUrlTransform, type Components } from 'react-markdown';
import remarkGfm from 'remark-gfm';
import { Link } from 'react-router-dom';
import { slugify } from '@/content';
import { figures } from './Figures';

function textOf(node: ReactNode): string {
  if (typeof node === 'string' || typeof node === 'number') return String(node);
  if (Array.isArray(node)) return node.map(textOf).join('');
  if (isValidElement<{ children?: ReactNode }>(node)) return textOf(node.props.children);
  return '';
}

// 锚点 id 用 content/index.ts 的 slugify：搜索结果的深链、本页目录与这里是同一个函数，链接一定对得上。
const components: Components = {
  h2: ({ children }) => {
    const id = slugify(textOf(children));
    return (
      <h2 id={id} className="group scroll-mt-32">
        <a href={`#${id}`} className="no-underline text-inherit">
          {children}
          <span aria-hidden="true" className="ml-3 font-sans text-[0.7em] font-normal text-c300 opacity-0 transition-opacity duration-300 ease-glaze group-hover:opacity-100">
            #
          </span>
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
      // 示意图是说明页这一节的主角，用瓷板（设计规格-官网 第 3 节）
      return (
        <figure className="my-10">
          <div className="slab">
            <div className="slab-core p-4 sm:p-6 overflow-hidden">
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

const remarkPlugins = [remarkGfm];

// react-markdown 默认只放行 http / https / mailto 等协议，会把 `figure:key` 清成空串（示意图全变坏图）。
// 只额外放行站内示意图的 figure: 前缀，其余照默认规则过滤。
function urlTransform(url: string) {
  return /^figure:[a-z0-9-]+$/.test(url) ? url : defaultUrlTransform(url);
}

/** 说明页正文。只随 source 变化重排：点本页目录改了 #hash 时外层会重渲染，正文不必重新解析。 */
const Markdown = memo(function Markdown({ source }: { source: string }) {
  return (
    <div className="prose-celadon">
      <ReactMarkdown remarkPlugins={remarkPlugins} components={components} urlTransform={urlTransform}>
        {source}
      </ReactMarkdown>
    </div>
  );
});

export default Markdown;
