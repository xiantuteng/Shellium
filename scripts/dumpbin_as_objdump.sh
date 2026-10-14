#!/usr/bin/env bash
set -euo pipefail

# 把 MSVC dumpbin 的依赖列表转换成 objdump -p 的 “DLL Name:” 格式，供 bundle_windows_runtime.sh
# 在 MSVC 构建中使用。MSVC 工具链自带 dumpbin，x64 与 ARM64 都可用，不必另装 binutils/LLVM。
#
# 用法与 objdump 相同：dumpbin_as_objdump.sh -p <PE 文件>
# dumpbin 需在 PATH 中（先执行 vcvars / msvc-dev-cmd）。在 Git Bash 中运行，需要 cygpath。

[[ "$1" == "-p" ]] || { echo "只支持 -p" >&2; exit 2; }
pe_file="$2"

# Git Bash 会把以 / 开头的参数当成路径改写，/dependents 会变成 C:/Program Files/Git/dependents。
# 同时列出普通依赖与延迟加载依赖：Qt 等库延迟加载的 DLL 运行时同样需要。
MSYS2_ARG_CONV_EXCL='*' dumpbin /nologo /dependents "$(cygpath -w "${pe_file}")" \
    | tr -d '\r' \
    | awk '
        /has the following (delay load )?dependencies/ { listing = 1; next }
        /^[[:space:]]*Summary/ { listing = 0 }
        listing && $1 ~ /\.[A-Za-z0-9]+$/ && NF == 1 { printf "\tDLL Name: %s\n", $1 }
      '
