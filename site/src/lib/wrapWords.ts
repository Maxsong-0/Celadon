/**
 * SVG 文字折行的纯逻辑：不碰 DOM，量宽度的办法由调用方给。与 SvgText.tsx 分开，是为了能在 Node 里单独测
 * （site/scripts/check-svg-text.mjs，随 `pnpm run build` 跑）。
 *
 * 断点：空白处；中日韩文字不用空格分词，每个字之间也都可以断。标点不会落在行首（句读、右括号、引号粘在前一个字上），
 * 左括号与引号不会落在行尾（粘在后一个字上）。数字和后面的单位（25 Hz、3.8 dB、10 s）粘成一个词，不会在它们中间断开。
 * 文案里的 \n 当普通空白处理，不是手动换行。
 */

// 中日韩文字：CJK 部首与符号、假名、统一表意文字、谚文、兼容表意文字、全角形式。
const CJK = /[⺀-鿿가-힯豈-﫿＀-￯]/;
// 不能出现在行首：句读、右括号与引号、省略号、破折号、间隔号。
const NO_LINE_START = /[，。、；：！？）］｝〉》」』】〕…—·]/;
// 不能出现在行尾：左括号与引号。
const NO_LINE_END = /[（［｛〈《「『【〔]/;
const WHITESPACE = /[ \t\r\n]/;

/** space：这个词与前一个词之间原来隔着空白（拼回一行时补一个空格；中日韩文字之间没有）。 */
export type Token = { text: string; space: boolean };

export function tokenize(text: string): Token[] {
  const glued = text.replace(/(\d)[ \t]+(?=[A-Za-z%°µ])/g, '$1 ');
  const tokens: Token[] = [];
  let word = ''; // 正在攒的西文词
  let gap = false; // 下一个词与前一个词之间隔着空白

  const flush = () => {
    if (!word) return;
    const last = tokens[tokens.length - 1];
    if (last && !gap && NO_LINE_END.test(last.text.slice(-1))) last.text += word;
    else tokens.push({ text: word, space: gap });
    word = '';
    gap = false;
  };

  for (const ch of glued) {
    if (WHITESPACE.test(ch)) {
      flush();
      gap = tokens.length > 0;
      continue;
    }
    if (CJK.test(ch)) {
      flush();
      const last = tokens[tokens.length - 1];
      if (last && !gap && (NO_LINE_START.test(ch) || NO_LINE_END.test(last.text.slice(-1)))) last.text += ch;
      else {
        tokens.push({ text: ch, space: gap });
        gap = false;
      }
      continue;
    }
    // 紧跟在中日韩文字后面的破折号、省略号、间隔号：不能另起一行
    const last = tokens[tokens.length - 1];
    if (!word && last && !gap && NO_LINE_START.test(ch) && (CJK.test(last.text.slice(-1)) || NO_LINE_START.test(last.text.slice(-1)))) {
      last.text += ch;
      continue;
    }
    word += ch;
  }
  flush();
  return tokens;
}

/**
 * 贪心折行：一行装不下就换行；单个词比 maxWidth 还宽时独占一行（不拆词）。
 * 在空格处断开的行，行尾带着那个空格（复制、页内查找时行与行不会连成一个词；SVG 里它在两个 tspan 之间，不显示）；
 * 中日韩文字之间断开的行没有。把各行直接拼起来，等于把原文里的连续空白合成一个空格之后的文字。
 */
export function wrapWords(text: string, maxWidth: number, widthOf: (line: string) => number): string[] {
  const lines: string[] = [];
  let line = '';
  for (const { text: word, space } of tokenize(text)) {
    const candidate = line ? `${line}${space ? ' ' : ''}${word}` : word;
    if (line && widthOf(candidate) > maxWidth) {
      lines.push(space ? `${line} ` : line);
      line = word;
    } else {
      line = candidate;
    }
  }
  lines.push(line);
  return lines;
}
