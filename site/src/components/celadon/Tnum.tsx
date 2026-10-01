/**
 * 等宽数字：Fraunces（Google Fonts 版）只带 liga / kern，没有 tnum，tabular-nums 对它不起作用——
 * 会变的读数（目标响度、数字节的计数）一变，整串连同后面的单位就左右晃。
 * 这里每个字符各包一格，数字格同宽（index.css 的 .tnum-cell），其余字符照常排。
 * 数字节计数时直接改这些格里的文字（见 Stats），所以每个字符都是一个元素。SVG 里的读数见 GlazeKnob。
 */
export default function Tnum({ text }: { text: string }) {
  return (
    <>
      {[...text].map((ch, i) => (
        <span key={i} className={/\d/.test(ch) ? 'tnum-cell' : undefined}>
          {ch}
        </span>
      ))}
    </>
  );
}
