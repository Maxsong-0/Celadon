#!/usr/bin/env bash
# 不用 sudo 备齐 JUCE 界面模块在 Linux 上要的开发头（T-099）：celadon_ui_snapshot 与 celadon_processor_tests 要 pkg-config 找到
# freetype2 / fontconfig / x11。共享机（108 的 CI runner 与 testbox 沙盒、DGX）不往系统里 apt 装：这里用 apt-get download 取
# Ubuntu 官方的 -dev 包，dpkg -x 解到 <目标目录>/root，把 .pc 里的路径改到解出来的地方、把 libX.so 开发链接指到系统的运行库
# （运行库各机器都有，只缺 -dev）。照 DGX 上 ~/.cache/celadon-spark/guidev 的做法（2026-10-04 起在用）写成脚本。
# testbox 沙盒里没有 apt 的包列表、apt-get download 取不到包：改用沙盒自带的 testbox-apt（它把 .deb 解到 /cache/apt-root，
# 跨任务保留），目标目录不用，后面改 .pc 与链接、自检照样做在 /cache/apt-root 上。
#
# 用法：scripts/fetch-gui-dev.sh <目标目录>
#   幂等：包已下载就不再下；每次都重新解包、从原样改 .pc 与链接，重复跑结果相同。架构按 dpkg 自动取（amd64 / arm64 …）。
#   最后打印要设的环境变量（export 开头的行），例如：
#     eval "$(scripts/fetch-gui-dev.sh "$HOME/.cache/celadon-guidev" | grep '^export ')"
#   GitHub Actions 里：scripts/fetch-gui-dev.sh "$dir" | sed -n 's/^export //p' >> "$GITHUB_ENV"
#   机器上没有 pkg-config 时顺带解一份 pkgconf，并打印 PKG_CONFIG（CMake 的 FindPkgConfig 认它）。
#
# 包清单：JUCE 9.0.2 docs/Linux Dependencies.md 里界面模块要的（freetype、fontconfig、x11 与几个 X 扩展；xcomposite 只在
# JUCE_USE_XRENDER=1 时用，照文档一并备上），加上 pkg-config 解析它们的 Requires / Requires.private 时要读到的 .pc
# （xcb、xau、xdmcp、x11proto、png、zlib、brotli、expat、bz2、uuid）。音频设备、curl、webkit、OpenGL 的包不要：CMake 里都关着。
set -euo pipefail

PACKAGES=(
  libfreetype-dev libfontconfig-dev
  libx11-dev libxcomposite-dev libxcursor-dev libxext-dev libxinerama-dev libxrandr-dev libxrender-dev libxi-dev libxfixes-dev
  libxcb1-dev libxau-dev libxdmcp-dev x11proto-dev
  libpng-dev zlib1g-dev libbrotli-dev libexpat1-dev libbz2-dev uuid-dev
)

die() { echo "fetch-gui-dev.sh：$*" >&2; exit 1; }

[ "$#" -eq 1 ] || { sed -n 2,16p "$0" >&2; exit 2; }
case "$1" in *[[:space:]:#]*) die "目标目录不能含空白、冒号或 #（要拼进 PKG_CONFIG_PATH 与 sed 表达式）：$1" ;; esac

# 包从哪来：有 apt 包列表的机器（CI runner、DGX）用 apt-get download；testbox 沙盒（有 testbox-apt、apt 列表是空的）用 testbox-apt。
TESTBOX_APT_ROOT=/cache/apt-root   # testbox-apt 固定解到这里（testbox guide「系统包的用户态装法」）
SOURCE=apt
if command -v testbox-apt >/dev/null && ! apt-cache show "${PACKAGES[0]}" >/dev/null 2>&1; then
  SOURCE=testbox
fi

if [ "$SOURCE" = apt ]; then
  for tool in apt-get apt-cache dpkg dpkg-deb; do
    command -v "$tool" >/dev/null || die "缺 $tool：这台机器不是 Debian / Ubuntu，或者精简掉了 apt / dpkg。没法按这个脚本取包，改在有 apt 的机器上跑、或请管理员装 ${PACKAGES[*]}"
  done
fi

mkdir -p "$1"
TARGET="$(cd "$1" && pwd -P)"
DEBS="$TARGET/debs"
ROOT="$TARGET/root"
[ "$SOURCE" = testbox ] && ROOT="$TESTBOX_APT_ROOT"
ARCH="$(dpkg --print-architecture 2>/dev/null || true)"
if command -v dpkg-architecture >/dev/null; then
  TRIPLET="$(dpkg-architecture -qDEB_HOST_MULTIARCH)"
else
  TRIPLET="$(gcc -print-multiarch 2>/dev/null || true)"
  [ -n "$TRIPLET" ] || case "$ARCH" in
    amd64) TRIPLET=x86_64-linux-gnu ;;
    arm64) TRIPLET=aarch64-linux-gnu ;;
    *) die "认不出 $ARCH 的多架构目录名（装 dpkg-dev 或 gcc 后再跑）" ;;
  esac
fi
LIBDIR="$ROOT/usr/lib/$TRIPLET"

if [ "$SOURCE" = testbox ]; then
  # 1～2（testbox）：testbox-apt 下载并解包（已装的跳过）；它的输出转到标准错误，标准输出只留最后的 export 行。
  # 机器上没有 pkg-config 时一并装 pkgconf：/cache/apt-root 的 bin 已在沙盒的 PATH 上，装完就能用。
  command -v pkg-config >/dev/null || PACKAGES+=(pkgconf)
  echo "testbox 沙盒：用 testbox-apt 装 ${PACKAGES[*]}（解到 $ROOT）" >&2
  testbox-apt install "${PACKAGES[@]}" >&2 || die "testbox-apt install 失败（看上面哪个包装不上）"
  [ -d "$LIBDIR/pkgconfig" ] || die "testbox-apt 装完了，$LIBDIR/pkgconfig 却不在：testbox-apt 解包的位置变了？改脚本里的 TESTBOX_APT_ROOT"
  command -v pkg-config >/dev/null || die "装了 pkgconf 还是找不到 pkg-config（$ROOT/usr/bin 不在 PATH 上？）"
fi

# 没有 pkg-config 的机器：顺带取 pkgconf（程序在 pkgconf-bin 里，它依赖的 libpkgconfN 的版本号按 apt 的依赖查）。
NEED_PKGCONF=0
if [ "$SOURCE" = apt ] && ! command -v pkg-config >/dev/null; then
  NEED_PKGCONF=1
  PKGCONF_LIB="$(apt-cache depends pkgconf-bin 2>/dev/null | sed -n 's/^ *Depends: \(libpkgconf[0-9][0-9]*\)$/\1/p' | head -n 1)"
  [ -n "$PKGCONF_LIB" ] || die "机器上没有 pkg-config，apt 的缓存里也查不到 pkgconf-bin 依赖的 libpkgconf（apt 列表是空的？）"
  PACKAGES+=(pkgconf-bin "$PKGCONF_LIB")
fi

if [ "$SOURCE" = apt ]; then
  mkdir -p "$DEBS" "$ROOT"

  # 1. 下载（已有同名包的跳过）。apt-get download 不要 root，只要 apt 的包列表在（/var/lib/apt/lists）。
  missing=()
  for pkg in "${PACKAGES[@]}"; do
    compgen -G "$DEBS/${pkg}_*_${ARCH}.deb" >/dev/null || compgen -G "$DEBS/${pkg}_*_all.deb" >/dev/null || missing+=("$pkg")
  done
  if [ "${#missing[@]}" -gt 0 ]; then
    echo "下载：${missing[*]}" >&2
    ( cd "$DEBS" && apt-get download "${missing[@]}" ) || die "apt-get download 失败（看上面哪个包找不到：包名在这个 Ubuntu 版本里改了，就改脚本里的 PACKAGES；网络不通就换一台能上 Ubuntu 源的机器跑，再把 $TARGET 整个拷过来）"
  fi

  # 2. 解包（覆盖式，重复跑结果相同）。
  for deb in "$DEBS"/*.deb; do
    dpkg-deb -x "$deb" "$ROOT"
  done
fi

# 3. .pc 里写死的 /usr 改到解包目录（只改 prefix 一类的变量；${prefix}/… 的相对写法跟着走）。第 2 步每次都解出原样的 .pc，
#    所以这一步每次都从原样改起；testbox 沙盒里上次改过的 .pc 已不以「=/usr」开头、不会再改。重复跑都不叠加。
for dir in "$LIBDIR/pkgconfig" "$ROOT/usr/share/pkgconfig"; do
  [ -d "$dir" ] || continue
  for pc in "$dir"/*.pc; do
    [ -e "$pc" ] || continue
    sed -i -E "s#^(prefix|exec_prefix|libdir|includedir|sharedlibdir)=/usr#\\1=$ROOT/usr#" "$pc"
  done
done

# 4. -dev 包里的 libX.so 是指向同目录运行库（libX.so.6 …）的相对链接，运行库没解进来，链接悬空：改指系统里的同名运行库。
#    第 2 步每次都把链接解回原样，这一步每次重做；testbox 沙盒里上次改好的链接已不悬空，跳过。
unresolved=()
if [ -d "$LIBDIR" ]; then
  for link in "$LIBDIR"/*.so; do
    [ -L "$link" ] || continue
    [ -e "$link" ] && continue                       # 指到存在的文件
    name="$(basename "$(readlink "$link")")"
    # 指向同目录里另一个开发链接（libpng.so -> libpng16.so）：那一个在它自己那一轮改，这条链跟着通。
    if [ -L "$LIBDIR/$name" ]; then continue; fi
    found=""
    for sysdir in "/usr/lib/$TRIPLET" "/lib/$TRIPLET"; do
      if [ -e "$sysdir/$name" ]; then found="$sysdir/$name"; break; fi
    done
    if [ -n "$found" ]; then ln -sfn "$found" "$link"; else unresolved+=("$(basename "$link") -> $name"); fi
  done
fi

# pkgconf 的程序要它自己的 libpkgconf：包一层脚本只给它设 LD_LIBRARY_PATH，不影响别的程序。
PKG_CONFIG_LINE=""
if [ "$NEED_PKGCONF" -eq 1 ]; then
  mkdir -p "$TARGET/bin"
  cat > "$TARGET/bin/pkg-config" <<EOF
#!/bin/sh
LD_LIBRARY_PATH="$LIBDIR\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}" exec "$ROOT/usr/bin/pkgconf" "\$@"
EOF
  chmod +x "$TARGET/bin/pkg-config"
  PKG_CONFIG_LINE="export PKG_CONFIG=$TARGET/bin/pkg-config"
fi

PCPATH="$LIBDIR/pkgconfig:$ROOT/usr/share/pkgconfig"

# 5. 自检：CMake 要的三个模块能解析（cflags 会顺着 Requires.private 读到每一份 .pc）。
PKGCONFIG_BIN="pkg-config"
[ "$NEED_PKGCONF" -eq 1 ] && PKGCONFIG_BIN="$TARGET/bin/pkg-config"
if ! PKG_CONFIG_PATH="$PCPATH${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" "$PKGCONFIG_BIN" --cflags --libs freetype2 fontconfig x11 >/dev/null; then
  die "pkg-config 解析 freetype2 / fontconfig / x11 失败（上面说缺哪份 .pc：把对应的 -dev 包加进 PACKAGES 再跑）"
fi

if [ "${#unresolved[@]}" -gt 0 ]; then
  echo "注意：这些开发链接找不到系统运行库（只有链接时用到它们才会出错；CMake 只链接 freetype / fontconfig / X11）：" >&2
  printf '  %s\n' "${unresolved[@]}" >&2
fi

echo "界面开发头已备好：$ROOT（$ARCH，$TRIPLET）"
echo "export PKG_CONFIG_PATH=$PCPATH${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
[ -z "$PKG_CONFIG_LINE" ] || echo "$PKG_CONFIG_LINE"
