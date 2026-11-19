#!/usr/bin/env bash
# testbox 打包目录入口（T-121 / T-123）。工作目录 = 打包目录：Celadon/（被测源码）、JUCE/（LICENSE.md + modules/）。
# 用法：./run.sh probe                                   看沙盒里有什么（Xvfb、字体、工具链）
#       ./run.sh tests <base|v3> [celadon_tests 参数…]   编 celadon_tests 并运行；v3 = -march=x86-64-v3
#       ./run.sh processor                               Release 编 celadon_processor_tests 并运行
#       ./run.sh ui <调用清单文件>                        编截图工具；清单一行一次调用（celadon_ui_snapshot 的参数），# 开头是注释
#                                                        行首写「!」的那一行期望非 0 退出（改坏变体用）
#                                                        行首写「@b64 <目录>」把该目录打成 tar.gz 以 base64 印出来（取图）
#                                                        行首写「@sha <目录>」印出该目录每个文件的 sha256（比两次出图是否逐字节相同）
# 环境：JOBS 并行编译数（默认 nproc）；BUILD_ROOT 构建目录的上级（默认 $PWD/build，每个任务全新；
#       开发中的定向检查可设 /cache/…，跨任务增量编）；CELADON_SRC 被测源码的目录名（默认 Celadon；
#       同一个任务里要编两棵树对比时，另一棵放在打包目录的别的名字下，如 Celadon-m0）。
set -uo pipefail

mode="${1:?用法见文件头}"; shift
root="$PWD"
jobs="${JOBS:-$(nproc)}"
build_root="${BUILD_ROOT:-$root/build}"
src="$root/${CELADON_SRC:-Celadon}"

[ -d "$src" ] && [ -d "$root/JUCE/modules" ] || { echo "工作目录要是打包目录（里面有 ${CELADON_SRC:-Celadon}/ 与 JUCE/modules）" >&2; exit 2; }

export PYTHONUSERBASE=/cache/local
export PATH="/cache/local/bin:$PATH"

need_cmake() {
  if ! command -v cmake >/dev/null 2>&1 || ! command -v ninja >/dev/null 2>&1; then
    python3.12 -m pip install --user --quiet cmake ninja || { echo "装 cmake / ninja 失败" >&2; exit 1; }
  fi
}

configure() {   # <构建目录> <额外的 cmake 参数…>
  local build="$1"; shift
  mkdir -p "$build"
  cmake -S "$src" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCELADON_JUCE_DIR=$root/JUCE" "$@" \
    > "$build/configure.log" 2>&1 || { tail -40 "$build/configure.log"; echo "配置失败" >&2; exit 1; }
}

build_targets() {   # <构建目录> <目标…>
  local build="$1"; shift
  cmake --build "$build" -j "$jobs" --target "$@" > "$build/build.log" 2>&1 \
    || { grep -n -E 'error|Error|FAILED' "$build/build.log" | head -60; tail -40 "$build/build.log"; echo "编译失败" >&2; exit 1; }
  echo "编译警告行数：$(grep -c -i 'warning:' "$build/build.log" || true)"
  grep -i 'warning:' "$build/build.log" | grep -v '/JUCE/' | sort -u | head -20 || true
}

gui_env() {
  local guidev="${GUIDEV:-/cache/celadon-guidev}"
  eval "$("$src/scripts/fetch-gui-dev.sh" "$guidev" | grep '^export ')"
  if ! command -v xvfb-run >/dev/null 2>&1; then
    testbox-apt install xvfb xauth x11-xkb-utils xkb-data >/dev/null 2>&1 || true
  fi
  if ! fc-list 2>/dev/null | grep -q -i 'Noto Sans CJK'; then
    testbox-apt install fonts-noto-cjk fontconfig >/dev/null 2>&1 || true
    mkdir -p "$HOME/.fonts"
    [ -d /cache/apt-root/usr/share/fonts ] && ln -sfn /cache/apt-root/usr/share/fonts "$HOME/.fonts/apt-root"
    fc-cache -f >/dev/null 2>&1 || true
  fi
}

case "$mode" in
  probe)
    echo "arch=$(uname -m) nproc=$(nproc)"
    gcc --version | head -1
    for t in cmake ninja xvfb-run Xvfb xauth fc-list pkg-config testbox-apt python3.12 taskset; do printf '%s: %s\n' "$t" "$(command -v "$t" || echo 没有)"; done
    echo "字体总数：$(fc-list 2>/dev/null | wc -l)"
    fc-list 2>/dev/null | grep -i -E 'cjk|noto sans|dejavu sans mono|liberation mono' | head -12
    ls /cache 2>/dev/null | head -20
    df -h /cache /work /tmp 2>/dev/null
    ;;

  tests)
    tier="${1:?tests <base|v3>}"; shift
    need_cmake
    flags=""
    [ "$tier" = "v3" ] && flags="-march=x86-64-v3"
    build="$build_root/tests-$tier"
    configure "$build" "-DCMAKE_CXX_FLAGS=$flags"
    build_targets "$build" celadon_tests
    "$build/celadon_tests" "$@" > "$build/tests.log" 2>&1
    status=$?
    sed -n 's/^==== \(celadon_tests: .*\) ====$/结果（'"$tier"' '"$(uname -m)"'）：\1/p' "$build/tests.log"
    awk '/^Starting tests in: /{c=$0;s=0} /^!!! Test / && n<200 {if(!s){print c;s=1} print "    " $0; n++}' "$build/tests.log"
    tail -5 "$build/tests.log"
    echo "celadon_tests（$tier）退出码 $status"
    exit "$status"
    ;;

  processor)
    need_cmake
    gui_env
    build="$build_root/processor"
    configure "$build" -DCELADON_TSAN=OFF
    grep -q celadon_processor_tests "$build/build.ninja" || { echo "没生成 celadon_processor_tests：界面开发头没找到" >&2; exit 1; }
    build_targets "$build" celadon_processor_tests
    "$build/celadon_processor_tests" "$@" > "$build/processor.log" 2>&1
    status=$?
    grep -E '^====|^!!! ' "$build/processor.log" | head -60
    tail -5 "$build/processor.log"
    echo "celadon_processor_tests 退出码 $status"
    exit "$status"
    ;;

  ui)
    list="${1:?ui <调用清单文件>}"
    need_cmake
    gui_env
    build="$build_root/ui"
    configure "$build"
    grep -q celadon_ui_snapshot "$build/build.ninja" || { echo "没生成 celadon_ui_snapshot：界面开发头没找到" >&2; exit 1; }
    build_targets "$build" celadon_ui_snapshot
    overall=0
    n=0
    while IFS= read -r line || [ -n "$line" ]; do
      case "$line" in ''|'#'*) continue ;; esac
      n=$((n + 1))
      if [ "${line#@sha }" != "$line" ]; then
        dir="${line#@sha }"
        echo "::sha-begin:: $dir"
        (cd "$dir" && find . -type f | LC_ALL=C sort | xargs sha256sum)
        echo "::sha-end:: $dir"
        continue
      fi
      if [ "${line#@b64 }" != "$line" ]; then
        dir="${line#@b64 }"
        echo "::b64-begin:: $dir"
        tar -C "$dir" -czf - . | base64 -w 76
        echo "::b64-end:: $dir"
        continue
      fi
      expect_fail=0
      if [ "${line#! }" != "$line" ]; then expect_fail=1; line="${line#! }"; fi
      echo "::group::ui-$n $line"
      # shellcheck disable=SC2086
      xvfb-run -a "$build/celadon_ui_snapshot" $line
      status=$?
      echo "::endgroup::"
      echo "调用 $n（$line）退出码 $status"
      if [ "$expect_fail" = 1 ]; then
        [ "$status" -ne 0 ] || { echo "调用 $n 期望失败却通过了"; overall=1; }
      else
        [ "$status" -eq 0 ] || overall=1
      fi
    done < "$list"
    echo "ui 总退出码 $overall"
    exit "$overall"
    ;;

  *)
    sed -n '2,12p' "$0" >&2
    exit 2
    ;;
esac
