#!/usr/bin/env bash
# 把 Xcode Release 编出的通用二进制（arm64 + x86_64）拆成两个 macOS 安装包：Apple Silicon（M 芯片）与 Intel 各一个 .pkg。
# 先按 docs/BUILDING.md 编好 Release 通用版（Builds/MacOSX/build/Release/），再：
#   scripts/package-mac.sh [输出目录]          # 默认 installer/Output/（不入库）
# 每个包里三个可选组件（安装器「自定」里可取消）：
#   VST3 → /Library/Audio/Plug-Ins/VST3/Celadon.vst3；AU → /Library/Audio/Plug-Ins/Components/Celadon.component；
#   独立运行版 → /Applications/Celadon.app。最低 macOS 12；Intel 包在 Apple Silicon 上会要求装 Rosetta（按芯片选包）。
# 签名：没有 Apple Developer ID，二进制是 ad-hoc 签名、安装包不签名也不公证——别人第一次打开 .pkg 会被 Gatekeeper 拦，
# 到「系统设置 → 隐私与安全性」点「仍要打开」。只在 Mac 上跑（pkgbuild / productbuild / lipo / codesign 是系统自带工具，不编译）。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
REL="$ROOT/Builds/MacOSX/build/Release"
OUT="${1:-$ROOT/installer/Output}"
VERSION="$(sed -n 's/^ *version="\([0-9.]*\)".*/\1/p' "$ROOT/Celadon.jucer" | head -1)"
[ -n "$VERSION" ] || { echo "Celadon.jucer 里读不到 version" >&2; exit 1; }

for b in Celadon.vst3 Celadon.component Celadon.app; do
  exe="$REL/$b/Contents/MacOS/Celadon"
  [ -f "$exe" ] || { echo "没有 $REL/$b：先编 Release 通用版（docs/BUILDING.md）" >&2; exit 1; }
  archs="$(lipo -archs "$exe")"
  case "$archs" in *arm64*x86_64*|*x86_64*arm64*) ;; *) echo "$b 不是通用二进制（$archs）：编的时候要带 -destination 'generic/platform=macOS' ONLY_ACTIVE_ARCH=NO" >&2; exit 1 ;; esac
  plugin_version="$(defaults read "$REL/$b/Contents/Info.plist" CFBundleShortVersionString)"
  [ "$plugin_version" = "$VERSION" ] || { echo "$b 的版本 $plugin_version 与 Celadon.jucer 的 $VERSION 不一致：先重新编" >&2; exit 1; }
done

WORK="$(mktemp -d "${TMPDIR:-/tmp}/celadon-pkg.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$OUT"

# 一个组件 = 一个 bundle + 安装位置。
component_pkg() {  # component_pkg <架构> <bundle> <安装位置> <标识后缀>
  local arch="$1" bundle="$2" location="$3" suffix="$4"
  local root="$WORK/$arch/root-$suffix" plist="$WORK/$arch/$suffix.plist"
  mkdir -p "$root"
  # 整个 bundle 拷过来，再用 lipo -thin 把主程序拆成这一个架构（ditto --arch 不一定拆：macOS 27 上实测照原样拷了通用二进制）；
  # 拆完重新 ad-hoc 签名，再严格校验。
  ditto "$REL/$bundle" "$root/$bundle"
  local exe="$root/$bundle/Contents/MacOS/Celadon"
  lipo -thin "$arch" "$exe" -output "$exe.thin"
  mv "$exe.thin" "$exe"
  codesign --force --sign - --timestamp=none "$root/$bundle"
  codesign --verify --strict "$root/$bundle"
  [ "$(lipo -archs "$root/$bundle/Contents/MacOS/Celadon")" = "$arch" ] || { echo "$bundle 拆出的不是单一 $arch" >&2; exit 1; }
  # 组件清单两项改掉（插件 bundle 的清单里默认没有 BundleIsRelocatable，要新加）：
  #   BundleIsRelocatable = false：否则安装器会把新版装到机器上已有的同 ID bundle 所在的地方（例如某个构建目录），而不是安装位置；
  #   BundleIsVersionChecked = false：否则目标位置已有更高版本时安装器不覆盖——0.1.9.0（开发时的号）比 0.1.8.5 高，会被悄悄跳过。
  pkgbuild --analyze --root "$root" "$plist" >/dev/null
  local i=0 key
  while /usr/libexec/PlistBuddy -c "Print :$i" "$plist" >/dev/null 2>&1; do
    for key in BundleIsRelocatable BundleIsVersionChecked; do
      /usr/libexec/PlistBuddy -c "Delete :$i:$key" "$plist" 2>/dev/null || true
      /usr/libexec/PlistBuddy -c "Add :$i:$key bool false" "$plist"
    done
    i=$((i + 1))
  done
  pkgbuild --quiet --root "$root" --component-plist "$plist" --install-location "$location" \
           --identifier "me.maxsong.celadon.$suffix" --version "$VERSION" "$WORK/$arch/celadon-$suffix.pkg"
}

for arch in arm64 x86_64; do
  if [ "$arch" = arm64 ]; then label="AppleSilicon"; title="Apple Silicon（M 芯片）"; else label="Intel"; title="Intel 芯片"; fi
  mkdir -p "$WORK/$arch"
  component_pkg "$arch" Celadon.vst3      /Library/Audio/Plug-Ins/VST3       vst3
  component_pkg "$arch" Celadon.component /Library/Audio/Plug-Ins/Components au
  component_pkg "$arch" Celadon.app       /Applications                      app

  cat > "$WORK/$arch/distribution.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>Celadon $VERSION · $title</title>
    <organization>me.maxsong</organization>
    <options customize="allow" require-scripts="false" hostArchitectures="$arch"/>
    <domains enable_localSystem="true" enable_currentUserHome="false" enable_anywhere="false"/>
    <volume-check>
        <allowed-os-versions>
            <os-version min="12.0"/>
        </allowed-os-versions>
    </volume-check>
    <choices-outline>
        <line choice="vst3"/>
        <line choice="au"/>
        <line choice="app"/>
    </choices-outline>
    <choice id="vst3" title="VST3 插件 / VST3 plug-in" description="装到 /Library/Audio/Plug-Ins/VST3（FL Studio、Ableton Live 等）">
        <pkg-ref id="me.maxsong.celadon.vst3"/>
    </choice>
    <choice id="au" title="AU 插件 / Audio Unit" description="装到 /Library/Audio/Plug-Ins/Components（Logic Pro、GarageBand 等）">
        <pkg-ref id="me.maxsong.celadon.au"/>
    </choice>
    <choice id="app" title="独立运行版 / Standalone app" description="装到 /Applications，不开宿主也能用">
        <pkg-ref id="me.maxsong.celadon.app"/>
    </choice>
    <pkg-ref id="me.maxsong.celadon.vst3" version="$VERSION" onConclusion="none">celadon-vst3.pkg</pkg-ref>
    <pkg-ref id="me.maxsong.celadon.au" version="$VERSION" onConclusion="none">celadon-au.pkg</pkg-ref>
    <pkg-ref id="me.maxsong.celadon.app" version="$VERSION" onConclusion="none">celadon-app.pkg</pkg-ref>
</installer-gui-script>
EOF

  product="$OUT/Celadon-$VERSION-macOS-$label.pkg"
  productbuild --quiet --distribution "$WORK/$arch/distribution.xml" --package-path "$WORK/$arch" --version "$VERSION" "$product"
  echo "$product"
done
