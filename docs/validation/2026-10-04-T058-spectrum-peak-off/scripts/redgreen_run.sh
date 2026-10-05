#!/usr/bin/env bash
# T-058 红绿验证第 3 轮：16 个变体二进制各跑一次 --only check（在全机锁里一次跑完）。用法：redgreen_run.sh <redgreen_build.sh 的输出目录>
set -u
OUT=$1
mkdir -p "$OUT/check"
for bin in "$OUT"/bin/*; do
  name=$(basename "$bin")
  home="$OUT/home-$name"; mkdir -p "$home/.config"
  HOME="$home" XDG_CONFIG_HOME="$home/.config" xvfb-run -a -s "-screen 0 1280x1024x24" "$bin" --only check > "$OUT/check/$name.txt" 2>&1
  echo "$name 退出码 $?  PASS=$(grep -c '^PASS' "$OUT/check/$name.txt") FAIL=$(grep -c '^FAIL' "$OUT/check/$name.txt")"
done
echo "全部完成 $(date +%T)"
