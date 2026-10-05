#!/usr/bin/env python3
"""按发布目录（artifacts/release-<版本>-<日期>/）重写 site/src/content/release.ts 里的 RELEASE 块。

只读发布目录，只写 release.ts；先核对再写：
  1. SHA256SUMS 恰好列三个安装包（Mac Apple Silicon / Mac Intel / Windows），文件名里的版本号与目录名一致；
  2. 每个文件真实存在、真实 SHA-256 与 SHA256SUMS 那行一致；
  3. 字节数取文件真实大小。
用法：patch_release_ts.py <发布目录> [release.ts 路径] [--dry-run]
"""
import difflib
import hashlib
import os
import re
import sys

args = [a for a in sys.argv[1:] if not a.startswith("--")]
dry = "--dry-run" in sys.argv
if not args:
    sys.exit(__doc__)
rel_dir = os.path.abspath(args[0])
ts_path = os.path.abspath(args[1]) if len(args) > 1 else os.path.join(os.getcwd(), "site/src/content/release.ts")

m = re.fullmatch(r"release-(\d+\.\d+\.\d+\.\d+)-(\d{4}-\d{2}-\d{2})", os.path.basename(rel_dir))
if not m:
    sys.exit(f"发布目录名要形如 release-<四段版本>-<YYYY-MM-DD>：{rel_dir}")
version, date = m.groups()

WANT = {  # id -> 文件名（与 release.ts 里的 id 一致）
    "mac-arm64": f"Celadon-{version}-macOS-AppleSilicon.pkg",
    "mac-x64": f"Celadon-{version}-macOS-Intel.pkg",
    "win-x64": f"Celadon-{version}-Windows-x64-Setup.exe",
}

sums = {}
for line in open(os.path.join(rel_dir, "SHA256SUMS"), encoding="utf-8"):
    line = line.rstrip("\n")
    if not line:
        continue
    mm = re.fullmatch(r"([0-9a-f]{64})  (\S+)", line)
    if not mm:
        sys.exit(f"SHA256SUMS 里有一行格式不对：{line!r}")
    sums[mm.group(2)] = mm.group(1)
if set(sums) != set(WANT.values()):
    sys.exit(f"SHA256SUMS 要恰好列这三个文件：\n  期望 {sorted(WANT.values())}\n  实际 {sorted(sums)}")

pk = []
for pid, name in WANT.items():
    path = os.path.join(rel_dir, name)
    if not os.path.isfile(path):
        sys.exit(f"发布目录里没有 {name}")
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    if h.hexdigest() != sums[name]:
        sys.exit(f"{name} 的真实 SHA-256 与 SHA256SUMS 不一致（{h.hexdigest()[:12]}… vs {sums[name][:12]}…）")
    pk.append((pid, name, os.path.getsize(path), sums[name]))

block = [f"export const RELEASE: Release = {{", f"  version: '{version}',", f"  date: '{date}',", "  packages: ["]
for i, (pid, name, size, sha) in enumerate(pk):
    block += ["    {", f"      id: '{pid}',", f"      file: '{name}',", f"      bytes: {size},", f"      sha256: '{sha}'",
              "    }" + ("," if i < len(pk) - 1 else "")]
block += ["  ]", "};"]
new_block = "\n".join(block)

src = open(ts_path, encoding="utf-8").read()
pat = re.compile(r"export const RELEASE: Release = \{.*?\n\};", re.S)
if len(pat.findall(src)) != 1:
    sys.exit("release.ts 里要恰好有一个 `export const RELEASE: Release = {…};` 块")
new_src = pat.sub(lambda _m: new_block, src)

diff = "".join(difflib.unified_diff(src.splitlines(True), new_src.splitlines(True), "release.ts（改前）", "release.ts（改后）", n=1))
print(diff or "（release.ts 已经是这些值，没有改动）")
if dry:
    print("[dry-run] 没写文件")
else:
    open(ts_path, "w", encoding="utf-8").write(new_src)
    print(f"已写 {ts_path}")
