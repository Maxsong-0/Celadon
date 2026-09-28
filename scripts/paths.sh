# 被 scripts/*.sh 用 source 引入（不单独执行）：给出本机临时目录的根，四个脚本的锁与默认构建目录都从这里派生。
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
