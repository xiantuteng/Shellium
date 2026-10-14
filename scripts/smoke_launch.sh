#!/usr/bin/env bash
set -euo pipefail

# 安装包启动冒烟测试：启动打包后的程序，等待若干秒后确认它仍在运行，然后结束它。
#
# 打包类故障（漏带动态库、Qt 平台插件缺失、rpath 仍指向构建机）都表现为启动即退出，
# 在构建机上直接运行未打包的程序是发现不了的。GUI 程序正常情况下不会自行退出，
# 所以“到时间仍存活”就是通过条件。
#
# 用法：smoke_launch.sh <等待秒数> <可执行文件> [参数...]

wait_seconds="$1"
shift

"$@" &
app_pid=$!

for _ in $(seq "${wait_seconds}"); do
    sleep 1
    if ! kill -0 "${app_pid}" 2>/dev/null; then
        set +e
        wait "${app_pid}"
        exit_code=$?
        set -e
        printf '冒烟测试失败：%s 启动后 %s 秒内退出，退出码 %s\n' "$1" "${wait_seconds}" "${exit_code}" >&2
        exit 1
    fi
done

kill "${app_pid}" 2>/dev/null || true
wait "${app_pid}" 2>/dev/null || true
printf '冒烟测试通过：%s 持续运行 %s 秒\n' "$1" "${wait_seconds}"
