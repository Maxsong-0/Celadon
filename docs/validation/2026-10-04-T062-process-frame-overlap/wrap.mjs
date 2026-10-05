export function wrapWords(text, maxWidth, widthOf) {
  const lines = [];
  let line = '';
  const words = text.replace(/(\d)[ \t]+(?=[A-Za-z%°µ])/g, '$1 ').split(/[ \t\r\n]+/).filter(Boolean);
  for (const word of words) {
    const candidate = line ? `${line} ${word}` : word;
    if (line && widthOf(candidate) > maxWidth) {
      lines.push(line);
      line = word;
    } else {
      line = candidate;
    }
  }
  lines.push(line);
  return lines;
}

const w = s => s.length * 6;   // 假的量宽度：每字 6 单位（NBSP 也算 1 个字）
const show = (t, max) => console.log(JSON.stringify(wrapWords(t, max, w).map(l => l.replace(/\u00a0/g, '␣'))), `<= ${max}`);
show('Low cut 25 Hz', 62);                 // 期望 ["Low cut","25␣Hz"]
show('Low cut 25 Hz', 80);                 // 放得下：一行
show('低切 25 Hz', 62);                     // 中文：一行
show('Light = rectangular window · dark = two-stage average (smooth corners)', 248 / 5.4 * 6);
show('Sidechain 10 / 10 s · reference ready', 100);   // 10 s 粘住
show('Supercalifragilisticexpialidocious', 20);       // 单个词超宽：独占一行
show('', 50);                                          // 空串
show('   ', 50);
