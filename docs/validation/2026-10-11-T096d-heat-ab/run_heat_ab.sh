#!/usr/bin/env bash
# T-096d：自动 Heat 底量 A/B 对照的批脚本（T-093 R6-03；条件与判定口径见 T-096 计划 §13 Q4 / Q8 / Q12）。
# 在 DGX 上、spark-test.sh run 的全机锁里跑：每首歌一个 celadon_render 进程（--replay-override color.heat=0 --metrics-only），
# xargs 并行；跑完用 tools/render/heat_ab_report.py 汇总出每格的 2a / ABX 结论。用法、heat_ab.csv 各列、结论怎么读：同目录 README.md。
#
#   SPARK=~/.cache/celadon-spark/bin/spark-test.sh; SRC=~/.cache/celadon-spark/src/t096-d
#   $SPARK $SRC build celadon_render
#   $SPARK $SRC run bash $SRC/docs/validation/2026-10-11-T096d-heat-ab/run_heat_ab.sh --render '$BUILD/celadon_render' --out ~/.cache/celadon-t096d/heat-ab
#
# 预演（任何机器都能跑）：加 --dry-run，只打印输入核对结果与要跑的每条命令，不建目录、不写文件、不跑渲染。
# 另验「不给新开关时输出逐字节不变」：加 --check-unchanged <main 构建的 celadon_render>，两版同参数（不给 --replay-override）各跑一遍，
# 逐首比 metrics.csv（逐字节）与 metrics.md（去掉「生成时间」一行后逐字节）。
#
# 退出码：0 = 全部跑完且无失败；1 = 有失败（自检没过、某首退出码非 0 或没出 heat_ab.csv、汇总出错、两版输出不同）；2 = 参数或输入不对。
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd -P)"
REPORT_PY="$ROOT/tools/render/heat_ab_report.py"

# ---- 条件与默认路径（改条件只改这里）----
GLAZE="35,100"
OVERRIDE="color.heat=0"
SONGS22_GRID="通用 × 标准"   # 22 首不显式给 --genre / --delivery（默认就是通用 / 标准）：metrics.csv 保持默认列，能与基线逐行对照
HOUSE_DELIVERIES="standard,streaming,club"
EXPECTED_SONGS22=22
EXPECTED_HOUSE9=9
AUDIO_EXTENSIONS="wav wave bwf aif aiff aifc flac"   # 同 celadon_render 读的扩展名
DEFAULT_SONGS22="$HOME/.cache/celadon-t048-22songs/wav"
DEFAULT_SONGS22_EXTRA="$HOME/.cache/celadon-t048-22songs/wav-missing2"   # T-096 从 109 补齐的 2 首（T-097 收尾前不并入 wav/）
DEFAULT_CAL="$HOME/.cache/celadon-t048-cal"
DEFAULT_CORPUS="$ROOT/docs/validation/2026-10-08-T085-codec-tp/corpus.csv"   # 9 首 House（deep / tech / melodic 各 3）
REPO_MANIFEST="$ROOT/docs/validation/2026-10-01-T038-calibration/tempo-manifest.tsv"   # DGX 上 manifest.tsv 的入库副本（找不到时用）
DEFAULT_JOBS=10

usage() {
  cat <<'USAGE'
用法：run_heat_ab.sh --render <celadon_render> --out <输出目录> [选项]
  --render 路径          要测的 celadon_render（带 --replay-override 的这版）
  --out 目录             输出根目录（不能已有 runs/、unchanged/ 或 jobs/：新旧结果混在一起，汇总会报「同一行出现两次」）
  --songs22 目录         22 首门槛曲（默认 ~/.cache/celadon-t048-22songs/wav）
  --songs22-extra 目录   另一个 22 首目录，存在就并进来（默认 ~/.cache/celadon-t048-22songs/wav-missing2；给空字符串不用）
  --cal 目录             Jamendo 45 首（默认 ~/.cache/celadon-t048-cal，<类>/<曲>/<曲>.wav）
  --manifest 文件        BPM 清单（默认 <cal>/manifest.tsv，没有就用仓库里的 tempo-manifest.tsv 副本）
  --corpus 文件          9 首 House 的清单（默认 docs/validation/2026-10-08-T085-codec-tp/corpus.csv）
  --only songs22|house9  只跑一组（默认两组都跑）
  --jobs N               并行进程数（默认 10）
  --allow-partial        22 首不够 22 首、9 首缺几首时照跑（默认停下报错）
  --check-unchanged 路径 另验不给新开关时的输出：这个 celadon_render（main 构建）与 --render 同参数各跑一遍，逐首比 metrics.csv / metrics.md
  --skip-selftest        不先跑 celadon_render --selftest / --selftest-house / --selftest-live 与汇总脚本的 --selftest
  --dry-run              只打印要做的事，不建目录、不写文件、不跑
USAGE
}

fail_usage() { echo "run_heat_ab.sh：$1" >&2; echo "（--help 看用法）" >&2; exit 2; }

RENDER=""; OUT=""; SONGS22="$DEFAULT_SONGS22"; SONGS22_EXTRA="$DEFAULT_SONGS22_EXTRA"; CAL="$DEFAULT_CAL"; MANIFEST=""
CORPUS="$DEFAULT_CORPUS"; ONLY="all"; JOBS="$DEFAULT_JOBS"; ALLOW_PARTIAL=0; BASE_RENDER=""; SKIP_SELFTEST=0; DRY_RUN=0

while [ "$#" -gt 0 ]; do
  case "$1" in
    --render|--out|--songs22|--songs22-extra|--cal|--manifest|--corpus|--only|--jobs|--check-unchanged)
      [ "$#" -ge 2 ] || fail_usage "$1 要跟一个值"
      case "$1" in
        --render) RENDER=$2 ;;
        --out) OUT=$2 ;;
        --songs22) SONGS22=$2 ;;
        --songs22-extra) SONGS22_EXTRA=$2 ;;
        --cal) CAL=$2 ;;
        --manifest) MANIFEST=$2 ;;
        --corpus) CORPUS=$2 ;;
        --only) ONLY=$2 ;;
        --jobs) JOBS=$2 ;;
        --check-unchanged) BASE_RENDER=$2 ;;
      esac
      shift 2 ;;
    --allow-partial) ALLOW_PARTIAL=1; shift ;;
    --skip-selftest) SKIP_SELFTEST=1; shift ;;
    --dry-run) DRY_RUN=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) fail_usage "不认识的参数 $1" ;;
  esac
done

[ -n "$RENDER" ] || fail_usage "要给 --render"
[ -n "$OUT" ] || fail_usage "要给 --out"
case "$ONLY" in all|songs22|house9) ;; *) fail_usage "--only 只认 songs22 / house9" ;; esac
case "$JOBS" in ''|*[!0-9]*|0*) fail_usage "--jobs 要正整数" ;; esac
# spark-test.sh run 会展开命令里的 $BUILD；万一原样传进来（单引号没被展开），这里按环境变量 BUILD 补展开一次。
expand_build() { case "$1" in '$BUILD'*) [ -n "${BUILD:-}" ] && printf '%s' "$BUILD${1#\$BUILD}" && return ;; esac; printf '%s' "$1"; }
RENDER=$(expand_build "$RENDER")
[ -z "$BASE_RENDER" ] || BASE_RENDER=$(expand_build "$BASE_RENDER")
[ -n "$MANIFEST" ] || MANIFEST="$CAL/manifest.tsv"
MANIFEST_NOTE="$MANIFEST"
if [ ! -f "$MANIFEST" ] && [ -f "$REPO_MANIFEST" ]; then
  MANIFEST_NOTE="$REPO_MANIFEST（$MANIFEST 不存在，用仓库副本）"
  MANIFEST="$REPO_MANIFEST"
fi

want() { [ "$ONLY" = all ] || [ "$ONLY" = "$1" ]; }

# ---- 输入核对（纯读）----
PROBLEMS=()     # 输入的问题（真跑时有就停，--allow-partial 只放过「数目不够」）
SHORTFALLS=()   # 数目不够（--allow-partial 可以放过）

SONG_FILES=()
collect_songs() {   # collect_songs <目录>：第一层、扩展名在白名单里的文件（含指向文件的软链），按名字排序
  local dir=$1 path name ext
  [ -n "$dir" ] && [ -d "$dir" ] || return 0
  while IFS= read -r -d '' path; do
    [ -e "$path" ] || { PROBLEMS+=("断开的软链：$path"); continue; }
    name=${path##*/}
    ext=$(printf '%s' "${name##*.}" | tr '[:upper:]' '[:lower:]')
    case " $AUDIO_EXTENSIONS " in *" $ext "*) SONG_FILES+=("$path") ;; esac
  done < <(find "$dir" -mindepth 1 -maxdepth 1 \( -type f -o -type l \) ! -name '.*' -print0 | sort -z)
}

song_stem() { local name=${1##*/}; printf '%s' "${name%.*}"; }

if want songs22; then
  [ -d "$SONGS22" ] || PROBLEMS+=("22 首目录不存在：$SONGS22")
  collect_songs "$SONGS22"
  collect_songs "$SONGS22_EXTRA"
  [ "${#SONG_FILES[@]}" -eq "$EXPECTED_SONGS22" ] \
    || SHORTFALLS+=("22 首目录里找到 ${#SONG_FILES[@]} 首（应为 $EXPECTED_SONGS22；$SONGS22${SONGS22_EXTRA:+ + $SONGS22_EXTRA}）")
  # 每首一个输出目录（目录名 = 歌名）：歌名重了会互相覆盖
  dupes=$(for f in ${SONG_FILES[@]+"${SONG_FILES[@]}"}; do song_stem "$f"; echo; done | sort | uniq -d)
  [ -z "$dupes" ] || PROBLEMS+=("22 首里有重名的歌（去掉扩展名后）：$(echo "$dupes" | tr '\n' ' ')")
fi

HOUSE_CLASSES=(); HOUSE_TRACKS=(); HOUSE_FILES=(); HOUSE_BPMS=()
if want house9; then
  if [ ! -f "$CORPUS" ]; then
    PROBLEMS+=("9 首清单不存在：$CORPUS")
  else
    [ -f "$MANIFEST" ] || PROBLEMS+=("BPM 清单不存在：$MANIFEST")
    while IFS=, read -r cls track _rest; do
      track=${track%$'\r'}
      [ "$cls" = category ] && continue
      [ -n "$cls" ] || continue
      case "$cls" in deep|tech|melodic) ;; *) PROBLEMS+=("corpus.csv 里不认识的类别「$cls」（$track）"); continue ;; esac
      wav="$CAL/$cls/$track/$track.wav"
      bpm=""
      [ -f "$MANIFEST" ] && bpm=$(awk -F'\t' -v t="$track" 'NR > 1 && $2 == t { print $3; exit }' "$MANIFEST")
      [ -e "$wav" ] || { SHORTFALLS+=("缺 House 音频：$wav"); continue; }
      case "$bpm" in ''|*[!0-9.]*) PROBLEMS+=("BPM 清单里没有 $track 的速度（$MANIFEST）"); continue ;; esac
      HOUSE_CLASSES+=("$cls"); HOUSE_TRACKS+=("$track"); HOUSE_FILES+=("$wav"); HOUSE_BPMS+=("$bpm")
    done < "$CORPUS"
    [ "${#HOUSE_TRACKS[@]}" -eq "$EXPECTED_HOUSE9" ] || SHORTFALLS+=("House 组可用 ${#HOUSE_TRACKS[@]} 首（应为 $EXPECTED_HOUSE9）")
  fi
fi

if [ "$DRY_RUN" -eq 0 ]; then
  [ -x "$RENDER" ] || PROBLEMS+=("--render 不是可执行文件：$RENDER")
  [ -z "$BASE_RENDER" ] || [ -x "$BASE_RENDER" ] || PROBLEMS+=("--check-unchanged 不是可执行文件：$BASE_RENDER")
  command -v python3 > /dev/null || PROBLEMS+=("没有 python3（汇总脚本只用标准库）")
  [ -f "$REPORT_PY" ] || PROBLEMS+=("找不到汇总脚本：$REPORT_PY")
  for leftover in runs unchanged jobs; do [ ! -e "$OUT/$leftover" ] || PROBLEMS+=("$OUT 里已有 $leftover/（上一次的结果）：换个 --out，或先手动挪走"); done
fi

print_list() { local title=$1; shift; [ "$#" -gt 0 ] || return 0; echo "$title"; printf '  - %s\n' "$@"; }

echo "== T-096d Heat 底量 A/B 对照（$(date '+%F %T %Z')）"
echo "代码：$ROOT"
echo "被测：$RENDER${BASE_RENDER:+；对照（不给新开关时逐字节核对）：$BASE_RENDER}"
echo "条件：--replay-override $OVERRIDE × GLAZE $GLAZE × 风格 auto × 修坯 auto；22 首 = $SONGS22_GRID；House = 各自子类 × $HOUSE_DELIVERIES（--bpm 取 $MANIFEST_NOTE）"
want songs22 && echo "22 首：${#SONG_FILES[@]} 首"
want house9 && echo "House：${#HOUSE_TRACKS[@]} 首"
print_list "输入问题：" ${PROBLEMS[@]+"${PROBLEMS[@]}"}
print_list "数目不够：" ${SHORTFALLS[@]+"${SHORTFALLS[@]}"}

if [ "$DRY_RUN" -eq 0 ]; then
  if [ "${#PROBLEMS[@]}" -gt 0 ] || { [ "${#SHORTFALLS[@]}" -gt 0 ] && [ "$ALLOW_PARTIAL" -eq 0 ]; }; then
    echo "run_heat_ab.sh：输入不对，没有开跑（数目不够而确实要照跑时加 --allow-partial）" >&2
    exit 2
  fi
fi

# ---- 计划：每个作业 = 一个 celadon_render 进程（一首歌、一个输出目录）----
JOB_LABELS=(); JOB_INPUTS=(); JOB_AUDIO=(); JOB_OUTS=(); JOB_KINDS=(); JOB_COMMANDS=()

quote_words() { local out="" word; for word in "$@"; do out+="$(printf '%q' "$word") "; done; printf '%s' "${out% }"; }

add_job() {   # add_job <种类 heat|base|new> <说明> <音频> <输入目录> <输出目录> <程序> <celadon_render 的网格参数…>
  local kind=$1 label=$2 audio=$3 input=$4 output=$5 program=$6
  shift 6
  JOB_KINDS+=("$kind"); JOB_LABELS+=("$label"); JOB_AUDIO+=("$audio"); JOB_INPUTS+=("$input"); JOB_OUTS+=("$output")
  JOB_COMMANDS+=("$(quote_words "$program" --in "$input" --out "$output" "$@")")
}

if want songs22; then
  for f in ${SONG_FILES[@]+"${SONG_FILES[@]}"}; do
    stem=$(song_stem "$f")
    add_job heat "songs22 · $stem" "$f" "$OUT/in/songs22/$stem" "$OUT/runs/songs22/$stem" "$RENDER" \
      --glaze "$GLAZE" --style auto --clay auto --metrics-only --replay-override "$OVERRIDE"
  done
fi

if want house9; then
  for i in ${HOUSE_TRACKS[@]+"${!HOUSE_TRACKS[@]}"}; do
    add_job heat "house9 · ${HOUSE_CLASSES[$i]} · ${HOUSE_TRACKS[$i]}" "${HOUSE_FILES[$i]}" "$OUT/in/house9/${HOUSE_TRACKS[$i]}" \
      "$OUT/runs/house9/${HOUSE_TRACKS[$i]}" "$RENDER" \
      --glaze "$GLAZE" --style auto --clay auto --genre "${HOUSE_CLASSES[$i]}" --delivery "$HOUSE_DELIVERIES" --bpm "${HOUSE_BPMS[$i]}" \
      --metrics-only --replay-override "$OVERRIDE"
  done
fi

# --check-unchanged：两版同参数、不给 --replay-override。22 首照 T-096 基线网格（含 GLAZE 0 与修坯全关，重放全关行也在）；
# House 走 T-038 的追加列（--genre / --delivery / --bpm），覆盖 csvExtendedFields 里改过判断的那一处。
if [ -n "$BASE_RENDER" ]; then
  for side in base new; do
    program=$RENDER; [ "$side" = base ] && program=$BASE_RENDER
    if want songs22; then
      for f in ${SONG_FILES[@]+"${SONG_FILES[@]}"}; do
        stem=$(song_stem "$f")
        add_job "$side" "不给新开关 · $side · songs22 · $stem" "$f" "$OUT/in/songs22/$stem" "$OUT/unchanged/$side/songs22/$stem" "$program" \
          --glaze 0,35,70,100 --style auto --clay auto,off --metrics-only
      done
    fi
    if want house9; then
      for i in ${HOUSE_TRACKS[@]+"${!HOUSE_TRACKS[@]}"}; do
        add_job "$side" "不给新开关 · $side · house9 · ${HOUSE_TRACKS[$i]}" "${HOUSE_FILES[$i]}" "$OUT/in/house9/${HOUSE_TRACKS[$i]}" \
          "$OUT/unchanged/$side/house9/${HOUSE_TRACKS[$i]}" "$program" \
          --glaze "$GLAZE" --style auto --clay auto,off --genre "${HOUSE_CLASSES[$i]}" --delivery "$HOUSE_DELIVERIES" --bpm "${HOUSE_BPMS[$i]}" \
          --metrics-only
      done
    fi
  done
fi

SELFTESTS=()
if [ "$SKIP_SELFTEST" -eq 0 ]; then
  SELFTESTS=("$(quote_words "$RENDER" --selftest)" "$(quote_words "$RENDER" --selftest-house)" "$(quote_words "$RENDER" --selftest-live)"
             "$(quote_words python3 "$REPORT_PY" --selftest)")
fi
REPORT_COMMAND="$(quote_words python3 "$REPORT_PY" --md "$OUT/REPORT.md" --csv "$OUT/cells.csv" --rows "$OUT/rows.csv" "$OUT/runs")"

echo "作业：${#JOB_COMMANDS[@]} 个（并行 $JOBS）"
if [ "${#JOB_COMMANDS[@]}" -eq 0 ] && [ "$DRY_RUN" -eq 0 ]; then
  echo "run_heat_ab.sh：没有要跑的作业（输入目录是空的？）" >&2
  exit 2
fi

if [ "$DRY_RUN" -eq 1 ]; then
  echo "== 预演：以下命令都不会执行"
  print_list "门槛自检（任一失败就不渲染）：" ${SELFTESTS[@]+"${SELFTESTS[@]}"}
  echo "输入软链（每首一个只含这首歌的目录，--in 指向它）："
  for i in ${JOB_COMMANDS[@]+"${!JOB_COMMANDS[@]}"}; do
    [ "${JOB_KINDS[$i]}" = heat ] && printf '  ln -s %s\n' "$(quote_words "${JOB_AUDIO[$i]}" "${JOB_INPUTS[$i]}/")"
  done
  echo "渲染（每条的 stderr 写进输出目录的 stderr.txt，退出码写 exit_code）："
  for i in ${JOB_COMMANDS[@]+"${!JOB_COMMANDS[@]}"}; do printf '  [%s] %s\n' "${JOB_LABELS[$i]}" "${JOB_COMMANDS[$i]}"; done
  echo "汇总：$REPORT_COMMAND"
  [ "${#PROBLEMS[@]}" -eq 0 ] && [ "${#SHORTFALLS[@]}" -eq 0 ] || echo "（真跑时上面的输入问题会让脚本停下；数目不够可加 --allow-partial）"
  exit 0
fi

# ---- 真跑 ----
started=$(date +%s)
mkdir -p "$OUT" || { echo "run_heat_ab.sh：建不了 $OUT" >&2; exit 2; }
FAILURES=()

sha_of() { if command -v sha256sum > /dev/null; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }

{
  echo "T-096d Heat 底量 A/B 对照"
  echo "开始：$(date '+%F %T %Z')；主机：$(uname -n)"
  echo "代码：$ROOT（提交 $(git -C "$ROOT" rev-parse --short HEAD 2> /dev/null || echo '未知：不是 git 目录')）"
  echo "被测：$RENDER（sha256 $(sha_of "$RENDER")）"
  [ -z "$BASE_RENDER" ] || echo "对照：$BASE_RENDER（sha256 $(sha_of "$BASE_RENDER")）"
  echo "条件：--replay-override $OVERRIDE；GLAZE $GLAZE；风格 auto；修坯 auto；--metrics-only"
  echo "  22 首：$SONGS22_GRID；House：各自子类 × $HOUSE_DELIVERIES；BPM 清单 $MANIFEST_NOTE"
  echo "并行：$JOBS；--allow-partial=$ALLOW_PARTIAL"
  print_list "数目不够（--allow-partial 放过）：" ${SHORTFALLS[@]+"${SHORTFALLS[@]}"}
  if want songs22; then echo "22 首："; for f in ${SONG_FILES[@]+"${SONG_FILES[@]}"}; do echo "  $f"; done; fi
  if want house9; then
    echo "House："
    for i in ${HOUSE_TRACKS[@]+"${!HOUSE_TRACKS[@]}"}; do printf '  %s\t%s\t%s BPM\t%s\n' "${HOUSE_CLASSES[$i]}" "${HOUSE_TRACKS[$i]}" "${HOUSE_BPMS[$i]}" "${HOUSE_FILES[$i]}"; done
  fi
} > "$OUT/run_info.txt"

if [ "${#SELFTESTS[@]}" -gt 0 ]; then
  echo "== 门槛自检"
  n=0
  for command in "${SELFTESTS[@]}"; do
    n=$((n + 1))
    if bash -c "$command" > "$OUT/selftest-$n.log" 2>&1; then
      echo "  通过：$command"
    else
      echo "  失败：$command（日志 $OUT/selftest-$n.log）"
      FAILURES+=("自检失败：$command")
    fi
  done
  if [ "${#FAILURES[@]}" -gt 0 ]; then
    echo "run_heat_ab.sh：门槛自检没过，不渲染" >&2
    exit 1
  fi
fi

echo "== 输入软链"
for i in "${!JOB_COMMANDS[@]}"; do
  link="${JOB_INPUTS[$i]}/${JOB_AUDIO[$i]##*/}"
  mkdir -p "${JOB_INPUTS[$i]}" || exit 2
  [ -L "$link" ] || ln -s "${JOB_AUDIO[$i]}" "$link" || exit 2
done

echo "== 渲染（${#JOB_COMMANDS[@]} 个作业，并行 $JOBS；每个作业的日志在各自输出目录的 stderr.txt）"
mkdir "$OUT/jobs" || exit 2   # 预检已确认它不存在：里面只会有这一次的作业脚本
for i in "${!JOB_COMMANDS[@]}"; do
  script="$OUT/jobs/$(printf '%03d' "$i").sh"
  {
    echo '#!/usr/bin/env bash'
    echo "# ${JOB_LABELS[$i]}"
    echo "mkdir -p $(quote_words "${JOB_OUTS[$i]}") || exit 0"
    echo "${JOB_COMMANDS[$i]} > $(quote_words "${JOB_OUTS[$i]}/stderr.txt") 2>&1"
    echo 'code=$?'
    echo "echo \"\$code\" > $(quote_words "${JOB_OUTS[$i]}/exit_code")"
    echo "echo $(quote_words "${JOB_LABELS[$i]}")\"：退出码 \$code\""
    echo 'exit 0   # 退出码由主脚本从 exit_code 收，一首失败不挡别的'
  } > "$script"
done
find "$OUT/jobs" -name '*.sh' -print0 | sort -z | xargs -0 -n 1 -P "$JOBS" bash

echo "== 收结果"
for i in "${!JOB_COMMANDS[@]}"; do
  output="${JOB_OUTS[$i]}"
  code=$(cat "$output/exit_code" 2> /dev/null || echo "没跑完")
  [ "$code" = 0 ] || FAILURES+=("${JOB_LABELS[$i]}：退出码 $code（$output/stderr.txt）")
  if [ "${JOB_KINDS[$i]}" = heat ] && [ ! -f "$output/heat_ab.csv" ]; then
    FAILURES+=("${JOB_LABELS[$i]}：没出 heat_ab.csv")
  fi
done

if [ -n "$BASE_RENDER" ]; then
  echo "== 不给新开关时两版输出逐字节比较（metrics.csv；metrics.md 去掉「生成时间」一行）"
  same=0
  for i in "${!JOB_COMMANDS[@]}"; do
    [ "${JOB_KINDS[$i]}" = base ] || continue
    a="${JOB_OUTS[$i]}"; b="$OUT/unchanged/new/${a#"$OUT/unchanged/base/"}"
    if [ -f "$a/metrics.csv" ] && [ -f "$b/metrics.csv" ] && [ -f "$a/metrics.md" ] && [ -f "$b/metrics.md" ] \
       && cmp -s "$a/metrics.csv" "$b/metrics.csv" \
       && cmp -s <(grep -v '^- 生成时间：' "$a/metrics.md") <(grep -v '^- 生成时间：' "$b/metrics.md"); then
      same=$((same + 1))
    else
      FAILURES+=("不给新开关时两版输出不同（或缺文件）：$a 与 $b")
    fi
  done
  echo "  相同 $same 首"
fi

echo "== 汇总"
if [ -d "$OUT/runs" ]; then
  if ! bash -c "$REPORT_COMMAND"; then
    FAILURES+=("汇总脚本出错：$REPORT_COMMAND")
  fi
else
  FAILURES+=("没有 runs/ 目录：一首都没出结果")
fi

elapsed=$(( $(date +%s) - started ))
{
  echo "结束：$(date '+%F %T %Z')；用时 $((elapsed / 60)) 分 $((elapsed % 60)) 秒"
  print_list "失败：" ${FAILURES[@]+"${FAILURES[@]}"}
} | tee -a "$OUT/run_info.txt"

if [ "${#FAILURES[@]}" -gt 0 ]; then
  echo "run_heat_ab.sh：有 ${#FAILURES[@]} 处失败（见上；结论表仍在 $OUT/REPORT.md，不可信的行已单独列出）" >&2
  exit 1
fi
echo "全部完成：结论见 $OUT/REPORT.md（每格一行）、$OUT/cells.csv、$OUT/rows.csv；原始数据 $OUT/runs/<组>/<曲目>/heat_ab.csv"
