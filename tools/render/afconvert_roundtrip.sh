#!/usr/bin/env bash
# T-096c2：Apple AAC 参考列的 Mac 端半边。只做编码再解码，不量测、不改输入、不装 Python。
# 对输入目录里每个 wav：用系统 afconvert 编成 AAC（默认 256 kbps CBR），再解回 32-bit float wav，
# 同名写进输出目录。量测在 DGX 上做：codec_tp_check.py master-check --apple-aac-dir <输出目录拷过去后的路径>。
#
# 用法：
#   tools/render/afconvert_roundtrip.sh [--dry-run] [--force] [--bitrate-bps N] [--strategy N] [--quality N] <输入目录> <输出目录>
#
#   --dry-run       只打印将要做什么和每条命令，不建目录、不编码、不写文件（Linux 上也能预演）
#   --force         输出目录里已有同名文件时覆盖（默认跳过并提示）
#   --bitrate-bps   AAC 码率，单位 bit/s（默认 256000）
#   --strategy      afconvert 的码率分配：0 = CBR（默认，对应 ffmpeg 的 -b:a 256k）、1 = ABR、2 = 受限 VBR、3 = VBR
#   --quality       afconvert 的编码质量 0～127（默认 127）
#
# 只读 <输入目录> 第一层的 *.wav（不分大小写，跳过 . 开头的）。返回 0 = 全部成功或跳过；1 = 有文件失败；2 = 用法 / 环境错误。
# 兼容 macOS 自带的 bash 3.2。
set -euo pipefail

BITRATE_BPS=256000
STRATEGY=0
QUALITY=127
DRY_RUN=0
FORCE=0
TMP_DIR=""

usage() {
  sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
}

die_usage() {
  echo "错误：$1" >&2
  echo "（--help 看用法）" >&2
  exit 2
}

cleanup() {
  if [[ -n "$TMP_DIR" && -d "$TMP_DIR" ]]; then
    rm -rf "$TMP_DIR"
  fi
}
trap cleanup EXIT

positional=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --dry-run) DRY_RUN=1; shift ;;
    --force) FORCE=1; shift ;;
    --bitrate-bps)
      [[ $# -ge 2 ]] || die_usage "--bitrate-bps 需要一个数值"
      BITRATE_BPS="$2"; shift 2 ;;
    --strategy)
      [[ $# -ge 2 ]] || die_usage "--strategy 需要一个数值"
      STRATEGY="$2"; shift 2 ;;
    --quality)
      [[ $# -ge 2 ]] || die_usage "--quality 需要一个数值"
      QUALITY="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    --) shift; while [[ $# -gt 0 ]]; do positional+=("$1"); shift; done ;;
    -*) die_usage "未知选项：$1" ;;
    *) positional+=("$1"); shift ;;
  esac
done

if [[ ${#positional[@]} -ne 2 ]]; then
  die_usage "需要两个参数：<输入目录> <输出目录>"
fi
IN_DIR="${positional[0]}"
OUT_DIR="${positional[1]}"

[[ "$BITRATE_BPS" =~ ^[0-9]+$ ]] || die_usage "--bitrate-bps 必须是正整数：$BITRATE_BPS"
[[ "$STRATEGY" =~ ^[0-3]$ ]] || die_usage "--strategy 必须是 0 ～ 3：$STRATEGY"
[[ "$QUALITY" =~ ^[0-9]+$ && "$QUALITY" -le 127 ]] || die_usage "--quality 必须是 0 ～ 127：$QUALITY"
[[ -d "$IN_DIR" ]] || die_usage "输入目录不存在：$IN_DIR"

if [[ -d "$OUT_DIR" && "$(cd "$IN_DIR" && pwd -P)" == "$(cd "$OUT_DIR" && pwd -P)" ]]; then
  die_usage "输出目录不能和输入目录是同一个目录（会覆盖原始成品）"
fi

if [[ $DRY_RUN -eq 0 ]]; then
  command -v afconvert >/dev/null 2>&1 || die_usage "找不到 afconvert（它只在 macOS 上有；Linux 上只能用 --dry-run 预演）"
fi

shopt -s nullglob nocaseglob
files=("$IN_DIR"/*.wav)
shopt -u nullglob nocaseglob
if [[ ${#files[@]} -eq 0 ]]; then
  die_usage "输入目录里没有 wav：$IN_DIR"
fi

echo "输入 ${#files[@]} 个 wav（$IN_DIR）-> AAC $((BITRATE_BPS / 1000)) kbps（strategy $STRATEGY，quality $QUALITY）-> 32-bit float wav（$OUT_DIR）"
for src in "${files[@]}"; do
  echo "  $(basename "$src")"
done

if [[ $DRY_RUN -eq 1 ]]; then
  echo "[dry-run] 不建目录、不编码、不写文件；下面是每个文件将执行的命令"
  TMP_SHOW="<tmp>"
else
  mkdir -p "$OUT_DIR"
  TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/afconvert_roundtrip.XXXXXX")"
  TMP_SHOW="$TMP_DIR"
fi

ok=0
skipped=0
failed=0
for src in "${files[@]}"; do
  name="$(basename "$src")"
  stem="${name%.*}"
  dest="$OUT_DIR/$name"
  coded="$TMP_SHOW/$stem.m4a"
  encode_cmd=(afconvert -f m4af -d aac -b "$BITRATE_BPS" -s "$STRATEGY" -q "$QUALITY" "$src" "$coded")
  decode_cmd=(afconvert -f WAVE -d LEF32 "$coded" "$dest")

  if [[ $DRY_RUN -eq 1 ]]; then
    printf '[dry-run] %q ' "${encode_cmd[@]}"; echo
    printf '[dry-run] %q ' "${decode_cmd[@]}"; echo
    continue
  fi
  if [[ -e "$dest" && $FORCE -eq 0 ]]; then
    echo "跳过（输出里已有同名文件，加 --force 覆盖）：$dest"
    skipped=$((skipped + 1))
    continue
  fi
  if "${encode_cmd[@]}" && "${decode_cmd[@]}" && [[ -s "$dest" ]]; then
    echo "OK    $name -> $dest"
    ok=$((ok + 1))
  else
    echo "失败  $name（afconvert 返回错误，或输出为空）" >&2
    rm -f "$dest"
    failed=$((failed + 1))
  fi
  rm -f "$coded"
done

if [[ $DRY_RUN -eq 1 ]]; then
  exit 0
fi
echo "完成：成功 $ok，跳过 $skipped，失败 $failed。把 $OUT_DIR 拷到 DGX，再用 codec_tp_check.py master-check --apple-aac-dir 量。"
if [[ $failed -gt 0 ]]; then
  exit 1
fi
