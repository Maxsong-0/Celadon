#!/usr/bin/env bash
# T-096a 质量基线：22 首门槛曲 + 45 首 Jamendo House 各渲一遍（带 --trace），再用 quality_metrics.py 出 quality-22.csv / quality-45.csv。
#
# 用法：run_quality_baseline.sh [--dry-run] <celadon_render 路径> <输出根> <语料根>
#   在 DGX 上经 spark-test.sh 的 run 子命令跑（拿全机锁；Mac 上不跑）。--dry-run 只打印要跑的命令，不建目录、不建软链、不渲染。
#   语料：22 首 = <语料根>/celadon-t048-22songs/wav（平铺），45 首 = <语料根>/celadon-t048-cal/<deep|tech|melodic>/<曲>/<曲>.wav；
#   可用环境变量 SONGS22_DIR / CAL45_DIR 另指。Python 用 PY（默认 ~/.cache/celadon-spark/venv/bin/python），并行数 JOBS（默认 8）。
#
# 网格（T-096 计划 Q 取值）：
#   22 首：--glaze 0,35,70 --style auto --clay auto --trace；另跑一遍 GLAZE 35 的流媒体 / 俱乐部（--delivery streaming,club）。
#   45 首：按子类 --genre deep|tech|melodic，--delivery standard,streaming,club，--glaze 35,100，--style auto --clay auto --trace，
#          另加 --bpm <tempo-manifest 的速度> --segments-csv（逐小节统计：落差走段落口径、drop 目标与主段核对才算得出）。
#   重放（clay-replay-off）照默认保留：门槛脚本要它，quality_metrics.py 自己跳过。
# 输出根下：22/、45/（每首一个输入目录（软链）与输出目录、日志），45/sources/（两层目录平铺成软链，给 quality_metrics.py 找源文件），
#   quality-22.csv、quality-45.csv、run.tsv（提交号、celadon_render 的 sha256、参数、日期）。音频留在 DGX，只有 CSV 与 run.tsv 进仓库。
# 输出根必须不存在或为空（脚本不删任何东西）。退出码：0 成功；1 有渲染或指标失败；2 用法错。
set -euo pipefail

usage() {
  echo '用法：run_quality_baseline.sh [--dry-run] <celadon_render 路径> <输出根> <语料根>' >&2
  exit 2
}

DRY_RUN=0
if [[ ${1:-} == --dry-run ]]; then
  DRY_RUN=1
  shift
fi
(( $# == 3 )) || usage
RENDER=$1
OUT=$2
CORPUS=$3

if (( BASH_VERSINFO[0] < 4 || (BASH_VERSINFO[0] == 4 && BASH_VERSINFO[1] < 3) )); then
  echo 'run_quality_baseline：要 bash ≥ 4.3（用 wait -n 控并发）' >&2
  exit 2
fi

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$SCRIPT_DIR/../.." && pwd)
CAL_DOCS=$REPO/docs/validation/2026-10-01-T038-calibration   # 45 首的速度清单与逐小节标签（仓库里那份）
TEMPO=$CAL_DOCS/tempo-manifest.tsv
LABELS_AUTO=$CAL_DOCS/labels-auto.tsv
LABELS_OVERRIDE=$CAL_DOCS/labels-override.tsv
SONGS22_DIR=${SONGS22_DIR:-$CORPUS/celadon-t048-22songs/wav}
CAL45_DIR=${CAL45_DIR:-$CORPUS/celadon-t048-cal}
PY=${PY:-$HOME/.cache/celadon-spark/venv/bin/python}
JOBS=${JOBS:-8}

# 网格参数集中在这里（run.tsv 照抄）
ARGS_22_GLAZE=(--glaze 0,35,70 --style auto --clay auto --trace)
ARGS_22_DELIVERY=(--glaze 35 --style auto --clay auto --delivery streaming,club --trace)
ARGS_45=(--delivery standard,streaming,club --glaze 35,100 --style auto --clay auto --segments-csv --trace)   # 每首另加 --genre、--bpm

die() {
  echo "run_quality_baseline：$1" >&2
  exit "${2:-1}"
}

[[ -x $RENDER ]] || die "celadon_render 不存在或不可执行：$RENDER" 2
[[ -d $SONGS22_DIR ]] || die "22 首目录不存在：$SONGS22_DIR" 2
[[ -d $CAL45_DIR ]] || die "45 首目录不存在：$CAL45_DIR" 2
[[ -f $TEMPO && -f $LABELS_AUTO && -f $LABELS_OVERRIDE ]] || die "仓库里缺 T-038 的速度清单或标签：$CAL_DOCS" 2
[[ $JOBS =~ ^[1-9][0-9]*$ ]] || die "JOBS 要是正整数：$JOBS" 2
if (( ! DRY_RUN )); then
  [[ -x $PY ]] || die "Python 不存在：$PY" 2
  if [[ -e $OUT ]] && [[ -n $(ls -A "$OUT") ]]; then
    die "输出根已存在且非空：$OUT（换一个目录；脚本不删东西）" 2
  fi
fi

# ---- 歌单（只读语料目录）----
shopt -s nullglob nocaseglob
SONGS22=()
for file in "$SONGS22_DIR"/*.wav "$SONGS22_DIR"/*.aif "$SONGS22_DIR"/*.aiff "$SONGS22_DIR"/*.flac; do
  SONGS22+=("$file")
done
shopt -u nullglob nocaseglob
(( ${#SONGS22[@]} > 0 )) || die "22 首目录里没有 wav / aiff / flac：$SONGS22_DIR"

TRACKS45=()   # 每项「子类<TAB>曲名<TAB>速度」
while IFS=$'\t' read -r class track bpm _rest; do
  [[ -f $CAL45_DIR/$class/$track/$track.wav ]] || die "45 首缺文件：$CAL45_DIR/$class/$track/$track.wav"
  [[ $bpm =~ ^[0-9]+(\.[0-9]+)?$ ]] || die "速度清单里 $track 的速度读不成数：$bpm"
  TRACKS45+=("$class"$'\t'"$track"$'\t'"$bpm")
done < <(tail -n +2 "$TEMPO")
(( ${#TRACKS45[@]} > 0 )) || die "速度清单是空的：$TEMPO"

# ---- 执行层：干跑只打印；真跑时每个作业在后台跑，写日志与退出码文件 ----
run_or_print() {
  if (( DRY_RUN )); then
    printf '%q ' "$@"
    printf '\n'
  else
    "$@"
  fi
}

render_job() {   # <日志> <退出码文件> <命令…>：命令失败不让整个脚本退出，退出码记进文件，最后统一数
  local log=$1 status=$2 code=0
  shift 2
  nice -n 5 "$@" >"$log" 2>&1 || code=$?
  printf '%s\n' "$code" >"$status"
}

launch() {   # <日志> <退出码文件> <命令…>
  if (( DRY_RUN )); then
    printf '%q ' "${@:3}"
    printf '\n'
    return
  fi
  while (( $(jobs -rp | wc -l) >= JOBS )); do
    wait -n || true
  done
  render_job "$@" &
}

link_input() {   # <源文件> <输入目录>：每首一个只含这一首的输入目录（celadon_render 读 --in 目录第一层的全部音频）
  run_or_print mkdir -p "$2"
  run_or_print ln -s "$1" "$2/$(basename "$1")"
}

# ---- run.tsv（开头先写，跑完补结束行）----
COMMIT=${CELADON_COMMIT:-$(git -C "$REPO" rev-parse HEAD 2>/dev/null || echo unknown)}
if (( ! DRY_RUN )); then
  mkdir -p "$OUT"
  {
    printf 'key\tvalue\n'
    printf 'commit\t%s\n' "$COMMIT"
    printf 'celadon_render\t%s\n' "$RENDER"
    printf 'celadon_render_sha256\t%s\n' "$(sha256sum "$RENDER" | cut -d' ' -f1)"
    printf 'songs22_dir\t%s\n' "$SONGS22_DIR"
    printf 'songs22_count\t%s\n' "${#SONGS22[@]}"
    printf 'cal45_dir\t%s\n' "$CAL45_DIR"
    printf 'cal45_count\t%s\n' "${#TRACKS45[@]}"
    printf 'args_22_glaze\t%s\n' "${ARGS_22_GLAZE[*]}"
    printf 'args_22_delivery\t%s\n' "${ARGS_22_DELIVERY[*]}"
    printf 'args_45\t%s --genre <子类> --bpm <tempo-manifest>\n' "${ARGS_45[*]}"
    printf 'labels\t%s ; %s\n' "${LABELS_AUTO#"$REPO"/}" "${LABELS_OVERRIDE#"$REPO"/}"
    printf 'python\t%s\n' "$PY"
    printf 'jobs\t%s\n' "$JOBS"
    printf 'started_utc\t%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  } >"$OUT/run.tsv"
fi

# ---- 22 首 ----
for file in "${SONGS22[@]}"; do
  name=$(basename "$file")
  song=${name%.*}
  in_dir=$OUT/22/in/$song
  run_dir=$OUT/22/runs/$song
  link_input "$file" "$in_dir"
  run_or_print mkdir -p "$run_dir/glaze" "$run_dir/delivery" "$OUT/22/logs"
  launch "$OUT/22/logs/$song.glaze.log" "$OUT/22/logs/$song.glaze.exit" \
    "$RENDER" --in "$in_dir" --out "$run_dir/glaze" "${ARGS_22_GLAZE[@]}"
  launch "$OUT/22/logs/$song.delivery.log" "$OUT/22/logs/$song.delivery.exit" \
    "$RENDER" --in "$in_dir" --out "$run_dir/delivery" "${ARGS_22_DELIVERY[@]}"
done

# ---- 45 首 ----
run_or_print mkdir -p "$OUT/45/sources" "$OUT/45/logs"
for entry in "${TRACKS45[@]}"; do
  IFS=$'\t' read -r class track bpm <<<"$entry"
  source_file=$CAL45_DIR/$class/$track/$track.wav
  in_dir=$OUT/45/in/$track
  link_input "$source_file" "$in_dir"
  run_or_print ln -s "$source_file" "$OUT/45/sources/$track.wav"
  run_or_print mkdir -p "$OUT/45/runs/$track"
  launch "$OUT/45/logs/$track.log" "$OUT/45/logs/$track.exit" \
    "$RENDER" --in "$in_dir" --out "$OUT/45/runs/$track" --genre "$class" --bpm "$bpm" "${ARGS_45[@]}"
done

if (( DRY_RUN )); then
  echo "# 干跑：22 首 ${#SONGS22[@]} 首 × 2 遍、45 首 ${#TRACKS45[@]} 首 × 1 遍；之后跑 quality_metrics.py 出 quality-22.csv / quality-45.csv"
  exit 0
fi
wait

# ---- 数渲染失败（退出码文件不是 0 的，或缺失的）----
failed=0
count=0
shopt -s nullglob
for status in "$OUT"/22/logs/*.exit "$OUT"/45/logs/*.exit; do
  count=$((count + 1))
  if [[ $(cat "$status") != 0 ]]; then
    echo "渲染失败：${status%.exit}.log（退出码 $(cat "$status")）" >&2
    failed=$((failed + 1))
  fi
done
shopt -u nullglob
expected=$(( ${#SONGS22[@]} * 2 + ${#TRACKS45[@]} ))
if (( count != expected )); then
  echo "渲染作业少了：期望 $expected 个、只有 $count 个退出码文件" >&2
  failed=$((failed + 1))
fi
printf 'render_failures\t%s\n' "$failed" >>"$OUT/run.tsv"
(( failed == 0 )) || die "$failed 个渲染作业失败，没有跑指标（日志在 $OUT/22/logs、$OUT/45/logs）"

# ---- 指标 ----
status=0
"$PY" "$SCRIPT_DIR/quality_metrics.py" --renders "$OUT"/22/runs/*/glaze "$OUT"/22/runs/*/delivery \
  --sources "$SONGS22_DIR" --out "$OUT/quality-22.csv" --jobs "$JOBS" || status=1
"$PY" "$SCRIPT_DIR/quality_metrics.py" --renders "$OUT"/45/runs/* --sources "$OUT/45/sources" \
  --labels-auto "$LABELS_AUTO" --labels-override "$LABELS_OVERRIDE" --out "$OUT/quality-45.csv" --jobs "$JOBS" || status=1
printf 'quality_status\t%s\n' "$status" >>"$OUT/run.tsv"
printf 'finished_utc\t%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >>"$OUT/run.tsv"
(( status == 0 )) || die "quality_metrics.py 失败（见上面的输出），没写全 quality-22.csv / quality-45.csv"
echo "完成：$OUT/quality-22.csv、$OUT/quality-45.csv、$OUT/run.tsv"
