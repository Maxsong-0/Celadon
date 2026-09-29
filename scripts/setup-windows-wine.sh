#!/usr/bin/env bash
# 在 Linux 测试服务器上装好 scripts/windows-wine.sh 要用的东西。只需装一次；重跑是安全的（已装的跳过）。
# 在服务器上运行，不在 Mac 上：
#   ssh maxsong@192.168.100.109 'bash -s' < scripts/setup-windows-wine.sh
#
# 装什么（都装在 maxsong 名下，只有第 1 步要 sudo；不碰 ghrunner 与 runner 服务）：
#   1. Ubuntu 的 wine64 + msitools（sudo apt）；
#   2. msvc-wine（github.com/mstorsjo/msvc-wine，钉在下面的提交）下载的 Visual Studio 2022 生成工具 → ~/opt/msvc2022：
#      MSVC v143（x64 目标；x86 目标给 JUCE 的 Win32「VST3 Manifest Helper」）、Windows SDK、MSBuild。
#      下载即表示接受微软的 Visual Studio / Windows SDK 许可（--accept-license，2026-09-29 Max 同意）；装出来的编译器不可再分发；
#   3. Wine Mono（MSBuild 是 .NET Framework 程序），版本与 Wine 10 配套；
#   4. Inno Setup 6.7.3 → Wine 里的 C:\InnoSetup6（安装包用）。它的安装程序要建窗口：先把 Wine 的图形驱动设成 null（不需要显示器）。
set -euo pipefail

MSVC_WINE_COMMIT=514f8ea
MSVC_DIR="$HOME/opt/msvc2022"
CACHE="$HOME/.cache/celadon-win"
WINE_MONO_VERSION=9.4.0
INNO_VERSION=6.7.3
INNO_SHA256=9c73c3bae7ed48d44112a0f48e66742c00090bdb5bef71d9d3c056c66e97b732
export WINEDEBUG=-all
mkdir -p "$HOME/src" "$HOME/opt" "$CACHE"

# 1. Wine 与 msitools
if ! command -v wine64 >/dev/null || ! command -v msiextract >/dev/null; then
  sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends wine64 msitools winbind ca-certificates
fi
wine64 --version

# 2. VS2022 生成工具（--major 17 = VS2022 / v143；msvc-wine 默认是 18）
if [ ! -x "$MSVC_DIR/bin/x64/cl" ] || [ ! -x "$MSVC_DIR/bin/x86/cl" ]; then
  [ -d "$HOME/src/msvc-wine" ] || git clone https://github.com/mstorsjo/msvc-wine.git "$HOME/src/msvc-wine"
  git -C "$HOME/src/msvc-wine" checkout -q "$MSVC_WINE_COMMIT"
  ( cd "$HOME/src/msvc-wine" &&
    ./vsdownload.py --major 17 --accept-license --architecture x64 x86 --cache "$CACHE/vs-cache" --dest "$MSVC_DIR" &&
    ./install.sh "$MSVC_DIR" )
fi

# 3. Wine Mono
if [ ! -d "$HOME/.wine/drive_c/windows/mono" ]; then
  curl -fsSL -o "$CACHE/wine-mono-$WINE_MONO_VERSION-x86.msi" \
    "https://dl.winehq.org/wine/wine-mono/$WINE_MONO_VERSION/wine-mono-$WINE_MONO_VERSION-x86.msi"
  wine64 msiexec /i "$CACHE/wine-mono-$WINE_MONO_VERSION-x86.msi" /qn
fi

# 4. Inno Setup
if [ ! -f "$HOME/.wine/drive_c/InnoSetup6/ISCC.exe" ]; then
  wine64 reg add 'HKCU\Software\Wine\Drivers' /v Graphics /d null /f
  /usr/lib/x86_64-linux-gnu/wine/wineserver -k || true   # 让驱动设置生效（只结束本用户的 Wine 进程；Ubuntu 不把它放进 PATH）
  installer="$CACHE/innosetup-$INNO_VERSION.exe"
  curl -fsSL -o "$installer" \
    "https://github.com/jrsoftware/issrc/releases/download/is-${INNO_VERSION//./_}/innosetup-$INNO_VERSION.exe"
  echo "$INNO_SHA256  $installer" | sha256sum -c -
  wine64 "$installer" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CURRENTUSER '/DIR=C:\InnoSetup6'
fi

# 自检
PATH="$MSVC_DIR/bin/x64:$PATH" cl 2>&1 | head -1
"$MSVC_DIR/bin/x64/msbuild" -version | tail -1
ls "$HOME/.wine/drive_c/InnoSetup6/ISCC.exe"
echo "Windows 工具链就绪"
