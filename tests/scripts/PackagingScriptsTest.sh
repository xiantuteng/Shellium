#!/usr/bin/env bash
set -euo pipefail

# 打包脚本的离线回归测试，不需要 Windows、MSVC 或真实安装包。
#
# bundle_windows_runtime.sh 只在 CI 的 Windows runner 上真正运行，出错要等一次完整发版才发现。
# 这里用假的 objdump/cygpath 构造“导入表”，在 macOS/Linux 上验证依赖闭包、大小写、插件子目录、
# 系统 DLL 判定顺序与 OpenSSL 运行时加载这几条容易写错的规则；smoke_launch.sh 同样覆盖
# “启动即退出必须失败”的判定。

SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../scripts" && pwd)"
WORK_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/shellium-packaging-test.XXXXXX")"
trap 'rm -rf -- "${WORK_ROOT}"' EXIT

failures=0
fail_case() {
    printf 'FAIL %s\n' "$*" >&2
    failures=$((failures + 1))
}
pass_case() {
    printf 'PASS %s\n' "$*"
}

# 假 objdump：被测脚本调用 objdump -p <文件>，这里把文件中的每一行 “IMPORT 名称” 输出成
# 真实 objdump 的 “DLL Name: 名称” 格式。
TOOLS_DIR="${WORK_ROOT}/tools"
mkdir -p "${TOOLS_DIR}"
cat > "${TOOLS_DIR}/objdump" <<'EOF'
#!/usr/bin/env bash
sed -n 's/^IMPORT /	DLL Name: /p' "$2"
EOF
# 假 cygpath：-u 把 SYSTEMROOT 映射到测试用的 Windows 目录；-w 原样返回路径。
cat > "${TOOLS_DIR}/cygpath" <<'EOF'
#!/usr/bin/env bash
if [[ "$1" == "-w" ]]; then
    printf '%s\n' "$2"
else
    printf '%s\n' "${FAKE_WINDOWS_ROOT}"
fi
EOF
# 假 dumpbin：输出与真实 dumpbin /dependents 相同的版式（CRLF、缩进、普通与延迟加载两段）。
cat > "${TOOLS_DIR}/dumpbin" <<'EOF'
#!/usr/bin/env bash
for arg in "$@"; do pe="$arg"; done
printf 'Dump of file %s\r\n\r\nFile Type: EXECUTABLE IMAGE\r\n\r\n' "$pe"
printf '  Image has the following dependencies:\r\n\r\n'
printf '    Qt6Core.dll\r\n    KERNEL32.dll\r\n\r\n'
printf '  Image has the following delay load dependencies:\r\n\r\n'
printf '    dwmapi.dll\r\n\r\n'
printf '  Summary\r\n\r\n        1000 .data\r\n'
EOF
chmod +x "${TOOLS_DIR}/objdump" "${TOOLS_DIR}/cygpath" "${TOOLS_DIR}/dumpbin"

# 在指定路径创建一个假 PE，参数 2 起为它导入的 DLL 名称。
make_pe() {
    local path="$1"
    shift
    mkdir -p "$(dirname "${path}")"
    : > "${path}"
    local name
    for name in "$@"; do
        printf 'IMPORT %s\n' "${name}" >> "${path}"
    done
}

# 为单个用例准备 staging、DLL 搜索目录与 System32 三个目录，路径通过全局变量返回。
new_case() {
    CASE_DIR="${WORK_ROOT}/$1"
    STAGING="${CASE_DIR}/staging"
    RUNTIME="${CASE_DIR}/msys/bin"
    export FAKE_WINDOWS_ROOT="${CASE_DIR}/Windows"
    SYSTEM32="${FAKE_WINDOWS_ROOT}/System32"
    mkdir -p "${STAGING}" "${RUNTIME}" "${SYSTEM32}"
    make_pe "${SYSTEM32}/kernel32.dll"
}

# 参数为额外的 DLL 搜索目录，排在 RUNTIME 之后。
run_bundle() {
    PATH="${TOOLS_DIR}:${PATH}" OBJDUMP="${TOOLS_DIR}/objdump" \
        bash "${SCRIPTS_DIR}/bundle_windows_runtime.sh" "${STAGING}" "${RUNTIME}" "$@" \
        > "${CASE_DIR}/output.txt" 2>&1
}

# 用例 1：间接依赖必须递归补齐，且 DLL 名称按 Windows 规则大小写不敏感。
new_case closure
make_pe "${STAGING}/Shellium.exe" libssh.dll KERNEL32.dll api-ms-win-crt-runtime-l1-1-0.dll
make_pe "${RUNTIME}/LIBSSH.DLL" libcrypto-3-x64.dll
make_pe "${RUNTIME}/libcrypto-3-x64.dll" kernel32.dll
if run_bundle && [[ -f "${STAGING}/LIBSSH.DLL" && -f "${STAGING}/libcrypto-3-x64.dll" ]]; then
    pass_case "递归补齐间接依赖"
else
    fail_case "递归补齐间接依赖：$(cat "${CASE_DIR}/output.txt")"
fi

# 用例 2：Qt 插件在子目录，它的依赖要放到 EXE 旁边（加载器从 EXE 目录找）。
new_case plugin
make_pe "${STAGING}/Shellium.exe" kernel32.dll
make_pe "${STAGING}/plugins/platforms/qwindows.dll" libc++.dll
make_pe "${RUNTIME}/libc++.dll" kernel32.dll
if run_bundle && [[ -f "${STAGING}/libc++.dll" ]]; then
    pass_case "插件依赖复制到 EXE 目录"
else
    fail_case "插件依赖复制到 EXE 目录：$(cat "${CASE_DIR}/output.txt")"
fi

# 用例 3：搜索目录里有的 DLL 即使构建机 System32 也有，也必须打进包。
new_case prefer-runtime
make_pe "${STAGING}/Shellium.exe" zlib1.dll
make_pe "${RUNTIME}/zlib1.dll" kernel32.dll
make_pe "${SYSTEM32}/zlib1.dll"
if run_bundle && [[ -f "${STAGING}/zlib1.dll" ]]; then
    pass_case "搜索目录优先于 System32"
else
    fail_case "搜索目录优先于 System32：$(cat "${CASE_DIR}/output.txt")"
fi

# 用例 4：哪里都找不到的 DLL 必须让脚本失败，并报出缺失的名称。
new_case missing
make_pe "${STAGING}/Shellium.exe" kernel32.dll notfound.dll
if run_bundle; then
    fail_case "缺失依赖必须失败，但脚本成功了"
elif grep -q 'notfound.dll' "${CASE_DIR}/output.txt"; then
    pass_case "缺失依赖导致失败"
else
    fail_case "缺失依赖的报错没有包含 DLL 名称：$(cat "${CASE_DIR}/output.txt")"
fi

# 用例 5：OpenSSL TLS 插件运行时 LoadLibrary 加载 libssl，导入表里没有，也要补齐。
new_case openssl
make_pe "${STAGING}/Shellium.exe" kernel32.dll
make_pe "${STAGING}/plugins/tls/qopensslbackend.dll" kernel32.dll
make_pe "${RUNTIME}/libssl-3-x64.dll" kernel32.dll
if run_bundle && [[ -f "${STAGING}/libssl-3-x64.dll" ]]; then
    pass_case "TLS 插件带上 libssl"
else
    fail_case "TLS 插件带上 libssl：$(cat "${CASE_DIR}/output.txt")"
fi

# 用例 6：只保留 SQLite 驱动；其他驱动的客户端库不存在，留着会让依赖检查失败。
new_case sqldrivers
make_pe "${STAGING}/Shellium.exe" kernel32.dll
make_pe "${STAGING}/plugins/sqldrivers/qsqlite.dll" kernel32.dll
make_pe "${STAGING}/plugins/sqldrivers/qsqlpsql.dll" libpq.dll
if run_bundle && [[ -f "${STAGING}/plugins/sqldrivers/qsqlite.dll" \
        && ! -e "${STAGING}/plugins/sqldrivers/qsqlpsql.dll" ]]; then
    pass_case "只保留 SQLite 驱动"
else
    fail_case "只保留 SQLite 驱动：$(cat "${CASE_DIR}/output.txt")"
fi

# 用例 7：多个搜索目录按顺序查找。Qt 的 DLL 在 Qt/bin，VC 运行库在 Redist 目录，都要补齐；
# 同名 DLL 取第一个目录里的那份；System32 里也有的 vcruntime140.dll 仍要从 Redist 打进包。
new_case multi-dirs
QT_BIN="${CASE_DIR}/qt/bin"
CRT_DIR="${CASE_DIR}/redist/Microsoft.VC143.CRT"
mkdir -p "${QT_BIN}" "${CRT_DIR}"
make_pe "${STAGING}/Shellium.exe" Qt6Core.dll libssh.dll
make_pe "${RUNTIME}/libssh.dll" vcruntime140.dll
printf 'IMPORT kernel32.dll\n# vcpkg\n' > "${RUNTIME}/zlib1.dll"
make_pe "${QT_BIN}/Qt6Core.dll" vcruntime140.dll zlib1.dll
printf 'IMPORT kernel32.dll\n# qt\n' > "${QT_BIN}/zlib1.dll"
make_pe "${CRT_DIR}/vcruntime140.dll" kernel32.dll
make_pe "${SYSTEM32}/vcruntime140.dll"
if run_bundle "${QT_BIN}" "${CRT_DIR}" \
        && [[ -f "${STAGING}/Qt6Core.dll" && -f "${STAGING}/vcruntime140.dll" ]] \
        && grep -q '# vcpkg' "${STAGING}/zlib1.dll"; then
    pass_case "多个搜索目录按顺序查找"
else
    fail_case "多个搜索目录按顺序查找：$(cat "${CASE_DIR}/output.txt")"
fi

# 用例 8：dumpbin 包装输出与 objdump 相同的 “DLL Name:” 格式，含延迟加载依赖，并去掉 CR。
dumpbin_output="$(PATH="${TOOLS_DIR}:${PATH}" bash "${SCRIPTS_DIR}/dumpbin_as_objdump.sh" -p "${WORK_ROOT}/any.exe")"
expected_output="$(printf '\tDLL Name: Qt6Core.dll\n\tDLL Name: KERNEL32.dll\n\tDLL Name: dwmapi.dll')"
if [[ "${dumpbin_output}" == "${expected_output}" ]]; then
    pass_case "dumpbin 包装输出导入表"
else
    fail_case "dumpbin 包装输出导入表：$(printf '%s' "${dumpbin_output}" | od -c | head -5)"
fi

# 用例 9：启动后立即退出的程序必须判为失败；持续运行的程序判为通过，并且等待时间有上限。
if bash "${SCRIPTS_DIR}/smoke_launch.sh" 2 false > "${WORK_ROOT}/smoke-exit.txt" 2>&1; then
    fail_case "启动即退出必须失败"
else
    pass_case "启动即退出判为失败"
fi
started=${SECONDS}
if bash "${SCRIPTS_DIR}/smoke_launch.sh" 2 sleep 30 > "${WORK_ROOT}/smoke-alive.txt" 2>&1 \
        && (( SECONDS - started < 10 )); then
    pass_case "持续运行判为通过并按时结束"
else
    fail_case "持续运行判为通过并按时结束：$(cat "${WORK_ROOT}/smoke-alive.txt")"
fi

# 用例 10：NSIS 安装脚本能编译。它只在 Windows 发版时运行，曾因 SetShellVarContext 写在
# Section/Function 之外而从未成功生成过安装器。本机没装 makensis 时跳过。
if command -v makensis >/dev/null 2>&1; then
    nsis_stage="${WORK_ROOT}/nsis-stage"
    mkdir -p "${nsis_stage}/plugins/platforms"
    : > "${nsis_stage}/Shellium.exe"
    : > "${nsis_stage}/plugins/platforms/qwindows.dll"
    if makensis -INPUTCHARSET UTF8 -WX \
            "-DSTAGING_DIR=${nsis_stage}" -DSHELLIUM_VERSION=0.0.0 \
            "-DOUTPUT_FILE=${WORK_ROOT}/Shellium-0.0.0-setup-x64.exe" \
            "${SCRIPTS_DIR}/../packaging/windows/Shellium.nsi" > "${WORK_ROOT}/nsis.txt" 2>&1 \
            && [[ -s "${WORK_ROOT}/Shellium-0.0.0-setup-x64.exe" ]]; then
        pass_case "NSIS 安装脚本可编译"
    else
        fail_case "NSIS 安装脚本可编译：$(grep -i -E 'error|warning' "${WORK_ROOT}/nsis.txt")"
    fi
else
    printf 'SKIP NSIS 安装脚本可编译（未安装 makensis）\n'
fi

if [[ "${failures}" -gt 0 ]]; then
    printf '%d 个用例失败\n' "${failures}" >&2
    exit 1
fi
printf '全部用例通过\n'
