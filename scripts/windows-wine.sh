#!/usr/bin/env bash
# 在 Linux 服务器上用真正的 MSVC 编 Windows x64 版、在 Wine 里跑测试（msvc-wine：cl.exe / link.exe / MSBuild 都在 Wine 里跑，
# 与 Windows 上的 Visual Studio 2022 是同一套编译器与 UCRT）。本机 Mac 不编、不测（ADR-002）；没有 Windows 机器时，
# 这是验证「Projucer 生成的 VS2022 工程编得过、测试在 MSVC 下通过」并产出 Windows 安装包的入口。
#
# 用法：scripts/windows-wine.sh plugin               # msbuild 编 Projucer 生成的 Celadon.sln（Release x64）：VST3 + Standalone
#       scripts/windows-wine.sh test [--only 类名]   # CMake + Ninja + cl.exe 编 celadon_tests，在 Wine 里跑（参数原样传给测试程序）
#       scripts/windows-wine.sh installer            # plugin + Inno Setup 安装包（installer/Celadon.iss）
#       scripts/windows-wine.sh ui                   # cl.exe 编界面截图工具，在 Wine + Xvfb 里跑交互检查（tools/ui-snapshot --only check）
#       scripts/windows-wine.sh fetch <本机目录>      # 把服务器上的 Celadon.vst3、Celadon.exe、安装包取回本机
#       scripts/windows-wine.sh all                  # test + installer
# 前置：① 拉了新提交或改过 Celadon.jucer 就先在 Mac 上用 Projucer 9.0.2 重新生成（命令见 docs/BUILDING.md「目录与 Projucer」，Projucer 要写全路径）：Builds/VisualStudio2022
#          随源码同步过去；② 服务器上按 scripts/setup-windows-wine.sh 装好 Wine、msvc-wine（VS2022 / v143）、Wine Mono、Inno Setup。
# 环境：CELADON_WIN_HOST 覆盖目标主机（默认 maxsong@192.168.100.109，需在家里网络或 VPN 下）。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
JUCE_DIR="$(cd "$ROOT/../JUCE" && pwd -P)"
HOST="${CELADON_WIN_HOST:-maxsong@192.168.100.109}"
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=15)

MODE="${1:-}"
case "$MODE" in
  plugin|test|installer|all|ui) shift ;;
  fetch)
    [ "$#" -eq 2 ] || { echo "用法：scripts/windows-wine.sh fetch <本机目录>" >&2; exit 2; }
    ;;
  *)
    sed -n '6,11p' "$0" >&2
    exit 2
    ;;
esac

# 每个工作树一个远端目录（Celadon/ 与 JUCE/ 同级，工程里的 ../JUCE 与 ../../../JUCE 才找得到）。
KEY="$(printf '%s' "$ROOT" | shasum -a 256 | cut -c1-12)"
REMOTE=".cache/celadon-win/$(basename "$ROOT")-$KEY"

if [ "$MODE" = "fetch" ]; then
  DEST="$2"
  mkdir -p "$DEST"
  # 「Standalone Plugin」目录名里有空格，远端路径的写法要两种 rsync 都认：openrsync / 旧版 rsync 把远端路径原样交给远端 shell（空格要自己转义）；
  # rsync ≥ 3.2.4 默认自己转义空格等 shell 字符，我们写的反斜杠被再转义一次，成了路径里的字面字符，找不到目录、退出码 23
  # （2026-10-05 Mac 上踩过，Spark 上同样复现）。所以用 ? 通配这个空格：两种写法下 ? 都不被转义，都由远端 shell 展开成带空格的那一个目录名
  # （展开结果不会再被拆开）。不用 --secluded-args（-s，那样才由远端 rsync 自己展开通配）：macOS 的 openrsync 不认这个参数。
  rsync -a -e "${SSH[*]}" \
    "$HOST:$REMOTE/Celadon/Builds/VisualStudio2022/x64/Release/VST3/Celadon.vst3" \
    "$HOST:$REMOTE/Celadon/Builds/VisualStudio2022/x64/Release/Standalone?Plugin/Celadon.exe" \
    "$DEST/"
  # 只取当前版本（Celadon.jucer 的 version）的安装包：服务器上的输出目录里可能还留着旧版本的。
  VERSION="$(sed -n 's/^ *version="\([0-9.]*\)".*/\1/p' "$ROOT/Celadon.jucer" | head -1)"
  rsync -a -e "${SSH[*]}" "$HOST:$REMOTE/Celadon/installer/Output/Celadon-$VERSION-Windows-x64-Setup.exe" "$DEST/" 2>/dev/null \
    || echo "服务器上没有 Celadon-$VERSION-Windows-x64-Setup.exe：先跑 scripts/windows-wine.sh installer" >&2
  ls -la "$DEST"
  exit 0
fi

if [ "$MODE" != "test" ] && [ "$MODE" != "ui" ]; then
  VCXPROJ="$ROOT/Builds/VisualStudio2022/Celadon_SharedCode.vcxproj"
  if [ ! -f "$VCXPROJ" ]; then
    echo "没有 Builds/VisualStudio2022：先在 Mac 上用 Projucer 9.0.2 重新生成并同步过来（命令见 docs/BUILDING.md「目录与 Projucer」；Projucer 不在 PATH 里，要写全路径）" >&2
    exit 1
  fi
  if [ "$ROOT/Celadon.jucer" -nt "$VCXPROJ" ]; then
    echo "Celadon.jucer 比 Builds/VisualStudio2022 新：先在 Mac 上用 Projucer 9.0.2 重新生成并同步过来（命令见 docs/BUILDING.md「目录与 Projucer」；Projucer 不在 PATH 里，要写全路径）" >&2
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

[ -x "$MSVC/bin/x64/cl" ] || { echo "服务器上没有 ${MSVC}：先跑 scripts/setup-windows-wine.sh" >&2; exit 1; }
export PATH="$MSVC/bin/x64:$PATH"
export WINEDEBUG=-all

configure_cmake() {
  # 静态运行库（/MT，与插件相同）：测试程序在 Wine 里不依赖 VC++ 运行库 DLL。
  if [ ! -f "$BUILD/build.ninja" ]; then
    CC=cl CXX=cl nice -n 10 cmake -S Celadon -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=Windows \
      -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
  fi
}

run_tests() {
  configure_cmake
  nice -n 10 cmake --build "$BUILD" --target celadon_tests
  # shellcheck disable=SC2086
  nice -n 10 wine64 "$BUILD/celadon_tests.exe" $CELADON_ARGS
}

run_ui_check() {
  # 界面要真的建窗口、画出来：单独的 Wine 前缀（默认 X11 驱动，不用编译前缀里给 Inno 安装程序设的 null 驱动），借 Xvfb 当显示器。
  # 截图工具会改写前缀里 %APPDATA%\Celadon 的界面设置，与编译前缀隔开。存下的检查截图在构建目录的 ui-check/ 里。
  configure_cmake
  nice -n 10 cmake --build "$BUILD" --target celadon_ui_snapshot
  command -v xvfb-run >/dev/null || { echo "服务器上没有 xvfb-run：sudo apt install xvfb" >&2; return 1; }
  mkdir -p "$BUILD/ui-check"
  (
    export WINEPREFIX="$HOME/.cache/celadon-win/prefix-ui-check" WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d"
    if [ ! -f "$WINEPREFIX/system.reg" ]; then
      xvfb-run -a wine64 wineboot -i
      /usr/lib/x86_64-linux-gnu/wine/wineserver -w
    fi
    cd "$BUILD/ui-check"
    nice -n 10 xvfb-run -a -s "-screen 0 1920x1080x24" wine64 "$BUILD/celadon_ui_snapshot.exe" --out . --only check
  )
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
  echo "msbuild 退出码 ${status}；本工程源码的警告 / 错误 $(printf '%s' "$own" | grep -c . || true) 条；JUCE 模块警告 $juce 条（完整日志：${log}）"

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
  # 前缀留着下次复用；null 图形驱动：安装向导即使静默也要建窗口。mscoree / mshtml 置空：不装 Wine Mono / Gecko；
  # 禁掉 winemenubuilder：不然装一次就在服务器的 Linux 菜单里留一个 Celadon.desktop。
  local setup="$PWD/$1"
  local prefix="$HOME/.cache/celadon-win/prefix-install-test"
  local out=Celadon/Builds/VisualStudio2022/x64/Release
  (
    export WINEPREFIX="$prefix" WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d"
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

    # 重装、这次只选 VST3（/TYPE=vst3）：独立版要被清掉，VST3 照旧在。
    wine64 "$setup" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /TYPE=vst3
    /usr/lib/x86_64-linux-gnu/wine/wineserver -w
    cmp "$out/VST3/Celadon.vst3/Contents/x86_64-win/Celadon.vst3" "$vst3/Contents/x86_64-win/Celadon.vst3"
    if [ -e "$app/Celadon.exe" ]; then
      echo "只装 VST3 重装后，上次的 Celadon.exe 还在" >&2
      exit 1
    fi
    echo "重装（只选 VST3）：独立版已清掉，VST3 仍在"

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
  ui)        run_ui_check ;;
esac
REMOTE_SCRIPT
status=$?
set -e
exit "$status"
