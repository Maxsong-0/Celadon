#!/usr/bin/env bash
# 把 AU 注册信息里的版本号（Info.plist 的 AudioComponents → version）改成与 JucePlugin_VersionCode 相同的编码。
#
# 为什么要改：JUCE 9.0.2 写这个字段时只取版本号的前三段（juce_PlistOptions.cpp 的 getAUVersionAsHexInteger），
# 0.1.8.8 写成 0.1.8 = 0x108（264），比 v0.6.1 的 0x601（1537）还低。macOS 遇到同一个 aufx Cldn Maxs 装了几份时只认版本最高的那份，
# 所以新版装上了，宿主照样加载旧 AU；第四段也进不了 AU 版本，0.1.8.5 ～ 0.1.8.8 在宿主眼里是同一版。
# 改成 JucePlugin_VersionCode 的编码（a.b.c.d → a×2^24 + b×2^16 + c×2^8 + d）：0.1.8.8 = 0x10808（67592），宿主里显示 1.8.8。
# 四段号每升一次 AU 版本都跟着升，也高于以前装过的任何一版（v0.6.x 是 0x6xx，JUCE 写的 0.1.x.y 是 0x1xx）。
# 三段号的版本 JUCE 本来就写对了，这里算出来的值与它相同。
#
# 两处调用：
#   1. Xcode 构建后（Celadon.jucer 的 Post-Build Shell Script，不带参数）：只在 AU 目标里动手（WRAPPER_EXTENSION = component），
#      早于 JUCE 自己的签名与安装（Release 拷进 ~/Library/Audio/Plug-Ins/Components）。改两个文件：
#      构建出的 Celadon.component/Contents/Info.plist，和 Projucer 生成的 Builds/MacOSX/Info-AU.plist。
#      后者也要改：只改产物的话，Xcode 下次构建发现产物被动过，会从 Info-AU.plist 重新生成（又是 0x108），
#      而生成 Info.plist 这一步与构建后脚本之间没有先后保证（swift-build 只保证它在准备阶段之后）。
#      Projucer 重存工程会把 Info-AU.plist 写回 0x108，下一次构建再改过来。
#   2. scripts/package-mac.sh：scripts/au-version.sh <Info.plist> [<Info.plist> ...]，改完由调用方重新签名。
# 已经是目标值的文件不写（修改时间不变，Xcode 不会因此重做别的步骤）。只在 Mac 上跑（PlistBuddy 是系统自带工具）。
set -euo pipefail
PLISTBUDDY=/usr/libexec/PlistBuddy

if [ "$#" -eq 0 ]; then
  [ "${WRAPPER_EXTENSION:-}" = component ] || exit 0
  source_plist="${INFOPLIST_FILE:?不在 Xcode 构建里：要给 Info.plist 路径}"
  case "$source_plist" in
    /*) ;;
    *) source_plist="${SRCROOT:-${PROJECT_DIR:?}}/${source_plist}" ;;
  esac
  set -- "${TARGET_BUILD_DIR:?}/${INFOPLIST_PATH:?}" "$source_plist"
fi

# 版本号 → JucePlugin_VersionCode 的十进制值（Projucer 的 getVersionAsHexIntegerFromParts，三段或四段，每段 0 ～ 255）。
version_code() {
  local version="$1" pattern='^[0-9]+(\.[0-9]+){2,3}$' code=0 part
  local -a parts
  [[ "$version" =~ $pattern ]] || { echo "版本号「${version}」不是三段或四段数字" >&2; return 1; }
  IFS=. read -r -a parts <<< "$version"
  for part in "${parts[@]}"; do
    part=$((10#$part))
    [ "$part" -le 255 ] || { echo "版本号「${version}」有一段大于 255，AU 版本编不进去" >&2; return 1; }
    code=$((code * 256 + part))
  done
  printf '%d' "$code"
}

for plist in "$@"; do
  [ -f "$plist" ] || { echo "找不到 ${plist}" >&2; exit 1; }
  version="$("$PLISTBUDDY" -c "Print :CFBundleShortVersionString" "$plist" 2>/dev/null)" ||
    { echo "${plist} 里读不到 CFBundleShortVersionString" >&2; exit 1; }
  code="$(version_code "$version")"
  "$PLISTBUDDY" -c "Print :AudioComponents:0:version" "$plist" >/dev/null 2>&1 ||
    { echo "${plist} 里没有 AudioComponents：不是 AU 的 Info.plist" >&2; exit 1; }
  i=0
  while old="$("$PLISTBUDDY" -c "Print :AudioComponents:${i}:version" "$plist" 2>/dev/null)"; do
    if [ "$old" != "$code" ]; then
      "$PLISTBUDDY" -c "Set :AudioComponents:${i}:version ${code}" "$plist"
      now="$("$PLISTBUDDY" -c "Print :AudioComponents:${i}:version" "$plist")"
      [ "$now" = "$code" ] || { echo "${plist} 的 AU 版本没写进去（读回 ${now}，应为 ${code}）" >&2; exit 1; }
      printf 'AU 版本 %s → %s（0x%X，%s）：%s\n' "$old" "$code" "$code" "$version" "$plist"
    fi
    i=$((i + 1))
  done
done
