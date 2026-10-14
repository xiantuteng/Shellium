#!/usr/bin/env bash
set -euo pipefail

# 构建失败时把日志中的错误片段作为 GitHub Actions 错误注解输出。
#
# 公开仓库的 job 日志需要登录才能下载，注解却可以匿名通过 check-runs API 读取。把关键日志放进
# 注解，未登录的协作者和本地自动化工具也能直接看到失败原因。整段合成一条注解：每个步骤最多只显示
# 10 条错误注解，逐行输出会被截断。
#
# GitHub 只保留注解开头约 4 KB，而 Ninja 的进度行会把真正的编译错误挤出去，所以优先摘取含
# error/错误/FAILED 的行及其上下文，摘不到时再退回日志末尾。
#
# 用法：ci_report_log_tail.sh <日志文件>

log_file="$1"
max_bytes=3800

[[ -f "${log_file}" ]] || exit 0

excerpt="$(grep -n -i -E -B2 -A6 'error|错误|FAILED|undefined reference|fatal' "${log_file}" \
    | grep -v -E '^[0-9]+[-:]\[[0-9]+/[0-9]+\] (Building|Generating|Running|Linking|Automatic)' || true)"
if [[ -z "${excerpt}" ]]; then
    excerpt="$(tail -n 40 "${log_file}")"
fi
excerpt="$(printf '%s' "${excerpt}" | head -c "${max_bytes}")"

# 按 workflow command 规则转义 %、CR、LF，否则多行内容会在第一个换行处被截断。
message="$(printf '%s\n' "${excerpt}" | sed -e 's/%/%25/g' -e 's/\r/%0D/g' | awk '{printf "%s%%0A", $0}')"
printf '::error title=%s::%s\n' "$(basename "${log_file}")" "${message}"
