# 被 scripts/*.sh 用 source 引入（不单独执行）：给出本机临时目录的根，四个脚本的锁与默认构建目录都从这里派生；
# 另给出 test-linux.sh / render-linux.sh 同步工作树到测试服务器时共用的排除项。
#
# 为什么不直接用 $TMPDIR：ssh / launchd / cron 启动的进程里 TMPDIR 可能没设或不同，几个脚本会拿到不同的锁、
# 互不排队（同时起两组 clang），scratch-build 还会找不到测试构建里的 libceladon_juce.a（2026-09-25 v0.3 评审 R2-01）。
# macOS 上取「本用户固定的系统临时目录」（getconf DARWIN_USER_TEMP_DIR，与环境变量无关）；拿不到时才退回 $TMPDIR 或 /tmp。
celadon_tmp_root() {
  local dir=""
  dir="$(getconf DARWIN_USER_TEMP_DIR 2>/dev/null || true)"
  if [ -z "$dir" ]; then
    dir="${TMPDIR:-/tmp}"
  fi
  printf '%s' "${dir%/}"
}

# test-linux.sh 与 render-linux.sh 把工作树 rsync 到测试服务器时的排除项（两边以前各抄一份，改一边容易漏另一边）。
# 只挡服务器上用不到的：Mac 端的 Xcode 工程与构建产物、预览 / 渲染包（artifacts/，几百 MB）、验证日志（docs/，与编译无关）、
# 系统垃圾文件与本机工作树（.claude/）。
# JuceLibraryCode/ 必须过网（BUG-005）：它是入库的 Projucer 输出（61 个文件约 250 KB，.jucer 重存后应逐字节不变，见 docs/BUILDING.md），
# CMake 的 celadon_ui_snapshot 强制包含其中的 JucePluginDefines.h（插件名、厂商、版本等 JucePlugin_* 宏的唯一出处）。
# 以前把它当生成物排除，服务器上截图工具的每个编译单元都找不到这个头。CMake 没把 JuceLibraryCode/ 放进头文件搜索路径，
# 同步过去的 JuceHeader.h 不会顶掉截图工具自己生成的那份。
# shellcheck disable=SC2034  # 由 source 本文件的 test-linux.sh / render-linux.sh 使用
CELADON_SOURCE_RSYNC_EXCLUDES=(
  --exclude 'Builds/' --exclude 'build/' --exclude 'artifacts/' --exclude 'docs/'
  --exclude '.DS_Store' --exclude '.claude/'
)
