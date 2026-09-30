#!/usr/bin/env bash
# 首页「肆 · 试听」的音频：原曲 → celadon_render（107）→ 切同一段、等响、AAC、波形包络（107 的 venv）→ 拉回 public/audio/ 与 src/content/listen.json。
# 本机只做文件搬运与清单拼接，不编译、不渲染、不测试（全局纪律）。曲目必须是 Max 自己有版权的作品。
#
# 用法：site/scripts/make-listen.sh <歌曲目录>
#   目录第一层的每首歌（wav / flac / aif / aiff）旁边可放一个同名 .json 写元数据，例：
#     {"title": "歌名", "note": "流行 · 2026", "start": 62.5}      # start 可省，省了就用全曲最响的 20 秒
#   没有 .json 时标题用文件名、note 为空。曲目 id 按文件名排序依次为 t1、t2……
# 环境：LISTEN_GLAZE（默认 50）、LISTEN_SECONDS（默认 20）、CELADON_TEST_HOST（默认 maxsong@192.168.100.107）
# 107 上需要 ~/venvs/celadon-site（numpy、scipy、soundfile、pyloudnorm、imageio-ffmpeg；T-033 已装）。
set -euo pipefail
SITE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
REPO="$(cd "$SITE/.." && pwd -P)"
SONGS="$(cd "${1:?用法：site/scripts/make-listen.sh <歌曲目录>}" && pwd -P)"
GLAZE="${LISTEN_GLAZE:-50}"
SECONDS_LEN="${LISTEN_SECONDS:-20}"
HOST="${CELADON_TEST_HOST:-maxsong@192.168.100.107}"
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=15)
STAMP="$(date +%Y%m%d-%H%M%S)"
RENDER_OUT="$REPO/artifacts/site-ab/render-$STAMP"
REMOTE=".cache/celadon-site-listen/$STAMP"

shopt -s nullglob nocaseglob
songs=("$SONGS"/*.{wav,flac,aif,aiff})
shopt -u nocaseglob
[ "${#songs[@]}" -gt 0 ] || { echo "$SONGS 里没有 wav / flac / aif / aiff" >&2; exit 1; }
IFS=$'\n' songs=($(printf '%s\n' "${songs[@]}" | sort)); unset IFS

echo "▶ 1/3 在 107 上渲染（GLAZE $GLAZE · 修坯自动 · 风格自动）→ $RENDER_OUT"
CELADON_RENDER_OUT="$RENDER_OUT" "$REPO/scripts/render-linux.sh" "$SONGS" --glaze "$GLAZE" --clay auto --style auto --match-lufs -16

echo "▶ 2/3 切片段、等响、编码"
"${SSH[@]}" "$HOST" "mkdir -p '$REMOTE/songs' '$REMOTE/render' '$REMOTE/out'"
rsync -t -e "${SSH[*]}" "${songs[@]}" "$HOST:$REMOTE/songs/"
rsync -t -e "${SSH[*]}" "$RENDER_OUT"/*__g"$GLAZE"_clay-auto_auto.wav "$RENDER_OUT/metrics.csv" "$HOST:$REMOTE/render/"
rsync -t -e "${SSH[*]}" "$SITE/scripts/listen_prepare.py" "$HOST:$REMOTE/"

i=0
for song in "${songs[@]}"; do
  i=$((i + 1))
  base="$(basename "$song")"
  stem="${base%.*}"
  meta="${song%.*}.json"
  # 元数据与对齐增益（metrics.csv 的 align_gain_db）用本机 python3 标准库读——只是解析文本。
  # 输出一行、制表符分隔；空值写成 -（read 会把连续的制表符并成一个）。
  IFS=$'\t' read -r title note start gain rendered < <(python3 - "$meta" "$stem" "$RENDER_OUT/metrics.csv" "$GLAZE" <<'PY'
import csv, json, sys, os
meta_path, stem, metrics, glaze = sys.argv[1:]
meta = json.load(open(meta_path, encoding='utf-8')) if os.path.exists(meta_path) else {}
want = f'{stem}__g{glaze}_clay-auto_auto.wav'
row = next((r for r in csv.DictReader(open(metrics, encoding='utf-8')) if r.get('file') == want), None)
if row is None:
    sys.exit(f'metrics.csv 里没有 {want}')
q = lambda v: str(v).replace(chr(9), ' ') if v not in (None, '') else '-'
print(chr(9).join([q(meta.get('title', stem)), q(meta.get('note')), q(meta.get('start')), row['align_gain_db'], want]))
PY
)
  [ "$note" = "-" ] && note=""
  args=(--original "$REMOTE/songs/$base" --rendered "$REMOTE/render/$rendered" --align-gain-db "$gain"
        --id "t$i" --title "$title" --note "$note" --seconds "$SECONDS_LEN" --out "$REMOTE/out/t$i"
        --settings "GLAZE $GLAZE % · 风格自动 · 其余出厂默认")
  [ "$start" != "-" ] && args+=(--start "$start")
  echo "  · t$i $title"
  "${SSH[@]}" "$HOST" "cd ~ && ~/venvs/celadon-site/bin/python $REMOTE/listen_prepare.py $(printf '%q ' "${args[@]}")"
done

echo "▶ 3/3 拉回 public/audio/ 与 src/content/listen.json"
rm -rf "$SITE/public/audio"
mkdir -p "$SITE/public/audio"
rsync -rt -e "${SSH[*]}" --exclude 'entry.json' "$HOST:$REMOTE/out/" "$SITE/public/audio/"
entries="$(mktemp -d)"
rsync -rt -e "${SSH[*]}" --include '*/' --include 'entry.json' --exclude '*' "$HOST:$REMOTE/out/" "$entries/"
python3 - "$entries" "$SITE/src/content/listen.json" <<'PY'
import json, sys, pathlib
root, dest = pathlib.Path(sys.argv[1]), sys.argv[2]
items = [json.loads(p.read_text(encoding='utf-8')) for p in sorted(root.glob('t*/entry.json'), key=lambda p: int(p.parent.name[1:]))]
pathlib.Path(dest).write_text(json.dumps(items, ensure_ascii=False, indent=1) + '\n', encoding='utf-8')
print(f'{len(items)} 首写进 {dest}')
PY
rm -rf "$entries"
"${SSH[@]}" "$HOST" "rm -rf '$REMOTE'"
du -sh "$SITE/public/audio"
echo "✔ 完成。渲染成品留在 $RENDER_OUT（artifacts/ 不入库，可删）。"
