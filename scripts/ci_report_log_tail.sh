#!/usr/bin/env bash
set -euo pipefail

# 构建失败时把日志末尾作为 GitHub Actions 错误注解输出。
#
# 公开仓库的 job 日志需要登录才能下载，注解却可以匿名通过 check-runs API 读取。把关键日志放进
# 注解，未登录的协作者和本地自动化工具也能直接看到失败原因。整段日志合成一条注解：每个步骤最多
# 只显示 10 条错误注解，逐行输出会被截断。
#
# 用法：ci_report_log_tail.sh <日志文件> [行数，默认 80]

log_file="$1"
lines="${2:-80}"

[[ -f "${log_file}" ]] || exit 0

# 按 workflow command 规则转义 %、CR、LF，否则多行内容会在第一个换行处被截断。
message="$(tail -n "${lines}" "${log_file}" | sed -e 's/%/%25/g' -e 's/\r/%0D/g' | awk '{printf "%s%%0A", $0}')"
printf '::error title=%s 末尾 %s 行::%s\n' "$(basename "${log_file}")" "${lines}" "${message}"
