#!/usr/bin/env bash
# 同一个任务里把两棵树各出两套图，再逐像素比（T-123）：隔着任务比文件哈希不可靠（电平表的下落按墙钟算）。
# 在 testbox 打包目录里跑（与 run.sh 同级）；目录里要有 Celadon/（现在的树）与 Celadon-m0/（基线的树，git archive 8090ad3）。
#   base 对 base2、now 对 now2：各自两遍之间哪些图本来就不稳定；base 对 now：改动让哪些图变了。
set -uo pipefail
cd "$(dirname "$0")"
out="$PWD/ab"
rm -rf "$out"; mkdir -p "$out"
printf '%s\n' "--out $out/base --deterministic" "--out $out/base2 --deterministic" > ab-base.list
printf '%s\n' "--out $out/now --deterministic" "--out $out/now2 --deterministic" \
              "--compare $out/base $out/base2" "--compare $out/now $out/now2" "--compare $out/base $out/now" > ab-now.list
CELADON_SRC=Celadon-m0 BUILD_ROOT="${BASE_BUILD_ROOT:-/cache/celadon-t123-m0}" ./run.sh ui ab-base.list | grep -v '^saved \|^::' | tail -8
BUILD_ROOT="${NOW_BUILD_ROOT:-/cache/celadon-t123}" ./run.sh ui ab-now.list | grep -E '^compare .*(differ|missing)|^==== compare|^调用|编译警告|warning:|error'
echo "::ab-done::"
