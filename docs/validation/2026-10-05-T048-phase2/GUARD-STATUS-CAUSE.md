# 状态行守门原因

第 4 条「受限 · 原因 · 已退 x dB」里的原因和退让是一对，来自同一扇门。

`publishStatus` 只看 `engaged == true` 的守门（正在退，或退到底仍超）。其中退让更负的胜出，这一扇的 `cause` 写成 `guardCause`，它的 `reliefDb` 写成 `protectionDb`。退让相同则留下先看到的，顺序是预算、保对比、PSR。退让为 0 时原因是 `none`。一扇都没在起作用时，`guardEngaged` 为 false，退让为 0，原因是 `none`。

正在回升的门 `engaged` 为 false。它即使退得更深，也不拿来当这一行的原因，数字也不用它的。

限幅器上实际减去的仍是三扇门里最深的那一份（`applyLimitExtras`），不因这一行改。
