#!/usr/bin/env bash

# 检查 App Bundle 内所有原生代码都包含目标架构，且动态库引用不再指向 Homebrew、/usr/local、
# 源码目录等构建机路径，引用的 @rpath/@loader_path/@executable_path 文件也真实存在。
#
# 用法：verify_macos_bundle_dependencies.sh <App 路径> <架构，如 arm64>
#
# 该检查防止在开发机正常、换到用户机器立即崩溃。官方 Qt 安装器的库是 universal 二进制，
# 不带 -arch 的 otool -L 会按架构重复输出整段依赖，第二段的标题行和 LC_ID_DYLIB 会被误当成
# 依赖，所以只读取目标架构的切片。

set -Eeuo pipefail

if [[ $# -ne 2 ]]; then
    printf '用法：%s <App 路径> <架构>\n' "$(basename "$0")" >&2
    exit 2
fi

app_path="$1"
target_arch="$2"

fail() {
    printf '错误：%s\n' "$*" >&2
    exit 1
}

found_macho="false"
while IFS= read -r -d '' candidate_path; do
    file_description="$(file -b "${candidate_path}" 2>/dev/null || true)"
    [[ "${file_description}" == *Mach-O* ]] || continue
    found_macho="true"

    architecture_list="$(lipo -archs "${candidate_path}")"
    [[ " ${architecture_list} " == *" ${target_arch} "* ]] \
        || fail "Bundle 中存在非 ${target_arch} 原生代码：${candidate_path}（${architecture_list}）"

    candidate_directory="$(dirname "${candidate_path}")"
    dependency_index=0
    is_shared_library="false"
    if [[ "${file_description}" == *"dynamically linked shared library"* ]]; then
        is_shared_library="true"
    fi

    while IFS= read -r dependency_path; do
        dependency_index=$((dependency_index + 1))

        # otool 会把 dylib/framework 自身的 LC_ID_DYLIB 作为第一项显示。该值用于其他
        # 二进制建立链接标识，不代表运行时还要从开发机加载另一个文件，因此不能按外部
        # 依赖处理；可执行文件和插件没有该 ID，第一项仍然必须正常校验。
        if [[ "${is_shared_library}" == "true" && "${dependency_index}" -eq 1 ]]; then
            continue
        fi

        case "${dependency_path}" in
            /usr/lib/*|/System/*)
                ;;
            /*)
                # 除系统库外的任何绝对路径都只存在于构建机：Homebrew、/usr/local、源码目录，
                # 以及 CI 上官方 Qt 安装器解压出的任意目录。
                fail "发现未部署的开发机动态库引用：${candidate_path} -> ${dependency_path}"
                ;;
            @rpath/*)
                # macdeployqt 为 App 和 Qt 插件配置的运行时搜索根均指向 Contents/Frameworks。
                # 仅检查字符串已变成 @rpath 不够；目标 Framework 或 dylib 也必须真实存在。
                [[ -e "${app_path}/Contents/Frameworks/${dependency_path#@rpath/}" ]] \
                    || fail "Bundle 缺少 @rpath 依赖：${candidate_path} -> ${dependency_path}"
                ;;
            @loader_path/*)
                [[ -e "${candidate_directory}/${dependency_path#@loader_path/}" ]] \
                    || fail "Bundle 缺少 @loader_path 依赖：${candidate_path} -> ${dependency_path}"
                ;;
            @executable_path/*)
                [[ -e "${app_path}/Contents/MacOS/${dependency_path#@executable_path/}" ]] \
                    || fail "Bundle 缺少 @executable_path 依赖：${candidate_path} -> ${dependency_path}"
                ;;
        esac
    done < <(otool -arch "${target_arch}" -L "${candidate_path}" | tail -n +2 | awk '{print $1}')
done < <(find "${app_path}" -type f -print0)

[[ "${found_macho}" == "true" ]] || fail "App Bundle 内没有找到 Mach-O 可执行文件。"
printf 'Bundle 架构与动态库引用检查通过（%s）\n' "${target_arch}"
