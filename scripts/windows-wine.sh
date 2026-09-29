#!/usr/bin/env bash
# 在 Linux 服务器上用真正的 MSVC 编 Windows x64 版、在 Wine 里跑测试（msvc-wine：cl.exe / link.exe / MSBuild 都在 Wine 里跑，
# 与 Windows 上的 Visual Studio 2022 是同一套编译器与 UCRT）。本机 Mac 不编、不测（ADR-002）；没有 Windows 机器时，
# 这是验证「Projucer 生成的 VS2022 工程编得过、测试在 MSVC 下通过」并产出 Windows 安装包的入口。
#
# 用法：scripts/windows-wine.sh plugin               # msbuild 编 Projucer 生成的 Celadon.sln（Release x64）：VST3 + Standalone
#       scripts/windows-wine.sh test [--only 类名]   # CMake + Ninja + cl.exe 编 celadon_tests，在 Wine 里跑（参数原样传给测试程序）
#       scripts/windows-wine.sh installer            # plugin + Inno Setup 安装包（installer/Celadon.iss）
#       scripts/windows-wine.sh fetch <本机目录>      # 把服务器上的 Celadon.vst3、Celadon.exe、安装包取回本机
#       scripts/windows-wine.sh all                  # test + installer
# 前置：① 改过 Celadon.jucer 就先在本机用 Projucer 9.0.2 重新生成（Projucer --resave Celadon.jucer）：Builds/VisualStudio2022
#          随源码同步过去；② 服务器上按 scripts/setup-windows-wine.sh 装好 Wine、msvc-wine（VS2022 / v143）、Wine Mono、Inno Setup。
# 环境：CELADON_WIN_HOST 覆盖目标主机（默认 maxsong@192.168.100.109，需在家里网络或 VPN 下）。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
JUCE_DIR="$(cd "$ROOT/../JUCE" && pwd -P)"
HOST="${CELADON_WIN_HOST:-maxsong@192.168.100.109}"
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=15)

MODE="${1:-}"
case "$MODE" in
  plugin|test|installer|all) shift ;;
  fetch)
    [ "$#" -eq 2 ] || { echo "用法：scripts/windows-wine.sh fetch <本机目录>" >&2; exit 2; }
    ;;
  *)
    sed -n '5,9p' "$0" >&2
    exit 2
    ;;
esac

# 每个工作树一个远端目录（Celadon/ 与 JUCE/ 同级，工程里的 ../JUCE 与 ../../../JUCE 才找得到）。
KEY="$(printf '%s' "$ROOT" | shasum -a 256 | cut -c1-12)"
REMOTE=".cache/celadon-win/$(basename "$ROOT")-$KEY"

if [ "$MODE" = "fetch" ]; then
  DEST="$2"
  mkdir -p "$DEST"
  rsync -a -e "${SSH[*]}" \
    "$HOST:$REMOTE/Celadon/Builds/VisualStudio2022/x64/Release/VST3/Celadon.vst3" \
    "$HOST:$REMOTE/Celadon/Builds/VisualStudio2022/x64/Release/Standalone Plugin/Celadon.exe" \
    "$DEST/"
  rsync -a -e "${SSH[*]}" "$HOST:$REMOTE/Celadon/installer/Output/" "$DEST/" 2>/dev/null || true
  ls -la "$DEST"
  exit 0
fi

if [ "$MODE" != "test" ]; then
  VCXPROJ="$ROOT/Builds/VisualStudio2022/Celadon_SharedCode.vcxproj"
  if [ ! -f "$VCXPROJ" ]; then
    echo "没有 Builds/VisualStudio2022：先用 Projucer 9.0.2 重新生成（Projucer --resave Celadon.jucer）" >&2
    exit 1
  fi
  if [ "$ROOT/Celadon.jucer" -nt "$VCXPROJ" ]; then
    echo "Celadon.jucer 比 Builds/VisualStudio2022 新：先用 Projucer 9.0.2 重新生成（Projucer --resave Celadon.jucer）" >&2
    exit 1
  fi
fi

# 本机锁（与 test-linux.sh 分开：Linux 与 Windows 两台服务器可以同时跑）。macOS 没有 flock，用 mkdir 锁。
# shellcheck source=paths.sh
source "$ROOT/scripts/paths.sh"
TMP_ROOT="$(celadon_tmp_root)"
LOCK="$TMP_ROOT/celadon-win-$KEY.lock"
until mkdir "$LOCK" 2>/dev/null; do
  if [ -f "$LOCK/pid" ] && ! kill -0 "$(cat "$LOCK/pid" 2>/dev/null)" 2>/dev/null; then
    rm -rf "$LOCK"
    continue
  fi
  sleep 2
done
echo $$ > "$LOCK/pid"
trap 'rm -rf "$LOCK"' EXIT

"${SSH[@]}" "$HOST" "mkdir -p '$REMOTE/Celadon' '$REMOTE/JUCE'"

# 源码 + JuceLibraryCode + VS2022 工程；Mac 工程、编译产物、档案与音频不过网。按内容比较（理由同 test-linux.sh）。
# 远端 Builds/VisualStudio2022 下的 x64/（msbuild 产物）与 installer/Output/ 不删，增量编。
rsync -rlpD --checksum --delete \
  --exclude 'Builds/MacOSX/' --exclude '/build/' --exclude '.DS_Store' --exclude '.claude/' --exclude '.git/' \
  --exclude 'artifacts/' --exclude 'docs/' \
  --filter 'protect Builds/VisualStudio2022/x64/' --filter 'protect Builds/VisualStudio2022/.vs/' \
  --filter 'protect installer/Output/' \
  -e "${SSH[*]}" "$ROOT"/ "$HOST:$REMOTE/Celadon/"

rsync -a --delete --delete-excluded \
  --include '/LICENSE.md' --include '/modules/***' \
  --exclude '*' \
  -e "${SSH[*]}" "$JUCE_DIR"/ "$HOST:$REMOTE/JUCE/"

set +e
"${SSH[@]}" "$HOST" \
  "CELADON_REMOTE=$(printf '%q' "$REMOTE") CELADON_MODE=$(printf '%q' "$MODE") CELADON_ARGS=$(printf '%q' "$*") bash -euo pipefail -s" <<'REMOTE_SCRIPT'
cd "$HOME/$CELADON_REMOTE"
MSVC="$HOME/opt/msvc2022"
BUILD="$HOME/.cache/celadon-win-build/$(basename "$CELADON_REMOTE")"
mkdir -p "$BUILD" "$HOME/.cache/celadon-locks"
exec 9>"$HOME/.cache/celadon-locks/win-$(basename "$CELADON_REMOTE").lock"
flock 9

[ -x "$MSVC/bin/x64/cl" ] || { echo "服务器上没有 $MSVC：先跑 scripts/setup-windows-wine.sh" >&2; exit 1; }
export PATH="$MSVC/bin/x64:$PATH"
export WINEDEBUG=-all

run_tests() {
  # 静态运行库（/MT，与插件相同）：测试程序在 Wine 里不依赖 VC++ 运行库 DLL。
  if [ ! -f "$BUILD/build.ninja" ]; then
    CC=cl CXX=cl nice -n 10 cmake -S Celadon -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=Windows \
      -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
  fi
  nice -n 10 cmake --build "$BUILD" --target celadon_tests
  # shellcheck disable=SC2086
  nice -n 10 wine64 "$BUILD/celadon_tests.exe" $CELADON_ARGS
}

build_plugin() {
  # msvc-wine 的 msbuild 包装只给 VS 18 设了工具集目录（*_180）；VS2022 的 v170 属性要的是 *_170。
  set +u
  # shellcheck disable=SC1091
  . "$MSVC/bin/x64/msvcenv.sh"
  set -u
  export VCInstallDir_170="${MSVCBASE}\\" VCToolsInstallDir_170="${MSVCDIR}\\"

  local log="$BUILD/msbuild.log"
  local status=0
  # /m:1：Wine 里 MSBuild 多节点（/m）会中途无声退出；单节点照样并行，Projucer 的工程给 cl.exe 开了 /MP。
  ( cd Celadon/Builds/VisualStudio2022 &&
    nice -n 10 msbuild Celadon.sln /p:Configuration=Release /p:Platform=x64 /m:1 /nologo /v:m ) > "$log" 2>&1 || status=$?

  # 本工程源码（Source\）的警告与错误一律打印；JUCE 模块的只数个数。
  local own juce
  own="$(awk '/: (warning|error) [A-Z]+[0-9]+/ && index($0, "\\Celadon\\Source\\")' "$log" | sort -u)"
  juce="$(awk '/: warning [A-Z]+[0-9]+/ && index($0, "\\JUCE\\modules\\")' "$log" | sort -u | wc -l)"
  grep -E ': (fatal )?error ' "$log" | sort -u | head -40 || true
  [ -n "$own" ] && printf '%s\n' "$own"
  echo "msbuild 退出码 $status；本工程源码的警告 / 错误 $(printf '%s' "$own" | grep -c . || true) 条；JUCE 模块警告 $juce 条（完整日志：$log）"

  if [ "$status" -ne 0 ] || [ -n "$own" ]; then
    return 1
  fi

  local out=Celadon/Builds/VisualStudio2022/x64/Release
  ls -la "$out/VST3/Celadon.vst3/Contents/x86_64-win/Celadon.vst3" "$out/VST3/Celadon.vst3/Contents/Resources/moduleinfo.json" \
         "$out/Standalone Plugin/Celadon.exe"
}

build_installer() {
  local iscc="$HOME/.wine/drive_c/InnoSetup6/ISCC.exe"
  [ -f "$iscc" ] || { echo "Wine 里没有 Inno Setup：先跑 scripts/setup-windows-wine.sh" >&2; return 1; }
  # 版本号取 Celadon.jucer 的 version（JUCERPROJECT 那一行，不是 XML 声明）。
  local version
  version="$(sed -n 's/^ *version="\([0-9.]*\)".*/\1/p' Celadon/Celadon.jucer | head -1)"
  [ -n "$version" ] || { echo "Celadon.jucer 里读不到 version" >&2; return 1; }
  ( cd Celadon/installer && nice -n 10 wine64 "$iscc" /Q "/DAppVersion=$version" Celadon.iss )
  ls -la Celadon/installer/Output/
  smoke_test_installer "Celadon/installer/Output/Celadon-$version-Windows-x64-Setup.exe"
}

smoke_test_installer() {
  # 在单独的 Wine 前缀里（不碰编译用的 ~/.wine）静默安装 → 文件与构建产物逐字节相同 → 静默卸载 → 两处都删干净。
  # 前缀留着下次复用；null 图形驱动：安装向导即使静默也要建窗口。mscoree / mshtml 置空：不装 Wine Mono / Gecko。
  local setup="$PWD/$1"
  local prefix="$HOME/.cache/celadon-win/prefix-install-test"
  local out=Celadon/Builds/VisualStudio2022/x64/Release
  (
    export WINEPREFIX="$prefix" WINEDLLOVERRIDES="mscoree,mshtml="
    if [ ! -f "$prefix/system.reg" ]; then
      wine64 wineboot -i
      wine64 reg add 'HKCU\Software\Wine\Drivers' /v Graphics /d null /f
      /usr/lib/x86_64-linux-gnu/wine/wineserver -w
    fi

    local vst3="$prefix/drive_c/Program Files/Common Files/VST3/Celadon.vst3"
    local app="$prefix/drive_c/Program Files/Celadon"
    wine64 "$setup" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
    /usr/lib/x86_64-linux-gnu/wine/wineserver -w

    cmp "$out/VST3/Celadon.vst3/Contents/x86_64-win/Celadon.vst3" "$vst3/Contents/x86_64-win/Celadon.vst3"
    cmp "$out/VST3/Celadon.vst3/Contents/Resources/moduleinfo.json" "$vst3/Contents/Resources/moduleinfo.json"
    cmp "$out/Standalone Plugin/Celadon.exe" "$app/Celadon.exe"
    echo "安装：VST3 与独立运行版都在，与构建产物逐字节相同"

    wine64 "$app/unins000.exe" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
    /usr/lib/x86_64-linux-gnu/wine/wineserver -w
    if [ -e "$vst3" ] || [ -e "$app/Celadon.exe" ]; then
      echo "卸载后还有残留：$(ls -d "$vst3" "$app/Celadon.exe" 2>/dev/null)" >&2
      exit 1
    fi
    echo "卸载：Common Files\VST3\Celadon.vst3 与 Program Files\Celadon\Celadon.exe 都已删除"
  )
}

case "$CELADON_MODE" in
  test)      run_tests ;;
  plugin)    build_plugin ;;
  installer) build_plugin && build_installer ;;
  all)       run_tests && build_plugin && build_installer ;;
esac
REMOTE_SCRIPT
status=$?
set -e
exit "$status"
