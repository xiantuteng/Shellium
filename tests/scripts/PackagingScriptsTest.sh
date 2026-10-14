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
        # 安装器曾只有“选目录 + 安装”两页，用户无从选择快捷方式，安装器和快捷方式也都是默认图标。
        # 页数取 makensis 的统计行：欢迎、许可、目录、组件、安装、完成共 6 页；组件为程序（必选）、
        # 开始菜单、桌面 3 个。
        if grep -q -E '^Install: 6 pages .* 3 sections \(1 required\)' "${WORK_ROOT}/nsis.txt"; then
            pass_case "NSIS 安装器提供组件选择与完成页"
        else
            fail_case "NSIS 安装器页面或组件不符：$(grep -E '^Install:' "${WORK_ROOT}/nsis.txt")"
        fi
        if command -v python3 >/dev/null 2>&1; then
            # ICO 里每个尺寸的图像数据都必须原样出现在安装器 EXE 的图标资源中。
            if python3 -I - "${SCRIPTS_DIR}/../packaging/windows/Shellium.ico" \
                    "${WORK_ROOT}/Shellium-0.0.0-setup-x64.exe" <<'EOF'
import struct, sys
ico = open(sys.argv[1], 'rb').read()
exe = open(sys.argv[2], 'rb').read()
count = struct.unpack('<H', ico[4:6])[0]
for i in range(count):
    size, offset = struct.unpack('<II', ico[14 + 16 * i:22 + 16 * i])
    if ico[offset:offset + size] not in exe:
        sys.exit(1)
EOF
            then
                pass_case "NSIS 安装器使用 Shellium 图标"
            else
                fail_case "NSIS 安装器没有嵌入 Shellium.ico"
            fi
        else
            printf 'SKIP NSIS 安装器图标（未安装 python3）\n'
        fi
    else
        fail_case "NSIS 安装脚本可编译：$(grep -i -E 'error|warning' "${WORK_ROOT}/nsis.txt")"
    fi
else
    printf 'SKIP NSIS 安装脚本可编译（未安装 makensis）\n'
fi

# 用例 11：Bundle 里任何一个 Mach-O 的最低系统高于发布目标都必须失败，并报出文件名。
# Homebrew 瓶子按构建机系统标注最低系统，只改 deployment target 时 Shellium 本体合格、
# 依赖库却不合格，所以要用两个不同最低系统的真实 Mach-O 验证逐个检查。没有 clang 时跳过。
if [[ "$(uname -s)" == "Darwin" ]] && command -v clang >/dev/null 2>&1; then
    min_os_arch="$(uname -m)"
    min_os_app="${WORK_ROOT}/MinOs.app/Contents"
    mkdir -p "${min_os_app}/MacOS" "${min_os_app}/Frameworks"
    printf 'int main(void) { return 0; }\n' > "${WORK_ROOT}/min_os_main.c"
    printf 'int shellium_dep(void) { return 1; }\n' > "${WORK_ROOT}/min_os_dep.c"
    printf 'not a binary\n' > "${min_os_app}/Info.plist"
    clang -arch "${min_os_arch}" -mmacosx-version-min=14.0 \
        -o "${min_os_app}/MacOS/MinOs" "${WORK_ROOT}/min_os_main.c"
    clang -arch "${min_os_arch}" -mmacosx-version-min=15.0 -dynamiclib \
        -o "${min_os_app}/Frameworks/libdep.dylib" "${WORK_ROOT}/min_os_dep.c"
    check_min_os() {
        bash "${SCRIPTS_DIR}/verify_macos_min_version.sh" "${WORK_ROOT}/MinOs.app" "$1" "${min_os_arch}" \
            > "${WORK_ROOT}/min-os.txt" 2>&1
    }
    if check_min_os 15.0 && check_min_os 15; then
        pass_case "最低系统不高于目标时通过"
    else
        fail_case "最低系统不高于目标时通过：$(cat "${WORK_ROOT}/min-os.txt")"
    fi
    if check_min_os 14.0; then
        fail_case "依赖库最低系统高于目标必须失败，但脚本成功了"
    elif grep -q 'libdep.dylib' "${WORK_ROOT}/min-os.txt" \
            && ! grep -q 'MacOS/MinOs' "${WORK_ROOT}/min-os.txt"; then
        pass_case "依赖库最低系统高于目标导致失败"
    else
        fail_case "最低系统报错没有准确指出 libdep.dylib：$(cat "${WORK_ROOT}/min-os.txt")"
    fi
else
    printf 'SKIP 最低系统检查（非 macOS 或未安装 clang）\n'
fi

# 用例 12：Windows EXE 资源。没有 ICON 资源时资源管理器、任务栏和快捷方式都是默认图标；
# rc.exe 按构建机 ANSI 代码页（CI 为 1252）读源文件，UTF-8 中文 FileDescription 在任务管理器
# 里显示为乱码。只在 Windows 上编译，所以这里静态检查模板与图标文件。
rc_template="${SCRIPTS_DIR}/../packaging/windows/Shellium.rc.in"
app_icon="${SCRIPTS_DIR}/../packaging/windows/Shellium.ico"
if [[ "$(head -n 1 "${rc_template}")" == '#pragma code_page(65001)' ]]; then
    pass_case "RC 模板首行声明 UTF-8 代码页"
else
    fail_case "RC 模板首行必须是 #pragma code_page(65001)"
fi
non_ascii_values="$(grep -E '^[[:space:]]*VALUE ' "${rc_template}" | LC_ALL=C grep -E '[^ -~]' || true)"
if [[ -z "${non_ascii_values}" ]]; then
    pass_case "RC 版本信息字符串为纯 ASCII"
else
    fail_case "RC 版本信息含非 ASCII 字符：${non_ascii_values}"
fi
# ICO 头：保留字 0、类型 1、条目数；条目第一个字节是宽度，0 表示 256 像素。
# 图标文件缺失时 od 失败，不能让 pipefail 直接终止整个测试，而要落到下面的 FAIL 分支。
ico_header="$(od -A n -t u1 -N 6 "${app_icon}" 2>/dev/null | tr -s ' ' | sed 's/^ //;s/ $//' || true)"
ico_count="$(od -A n -t u2 -j 4 -N 2 "${app_icon}" 2>/dev/null | tr -d ' ' || true)"
has_256=0
for (( i = 0; i < ${ico_count:-0}; i++ )); do
    width="$(od -A n -t u1 -j $(( 6 + 16 * i )) -N 1 "${app_icon}" | tr -d ' ')"
    [[ "${width}" == 0 ]] && has_256=1
done
if grep -q -E '^IDI_ICON1 ICON "@CMAKE_CURRENT_SOURCE_DIR@/packaging/windows/Shellium\.ico"' "${rc_template}" \
        && [[ "${ico_header}" == "0 0 1 0 "* && "${has_256}" == 1 ]]; then
    pass_case "EXE 嵌入含 256 像素的应用图标"
else
    fail_case "EXE 图标资源缺失或 Shellium.ico 无 256 像素图像（头：${ico_header}）"
fi

# 用例 13：官方 Qt 的库是 universal 二进制，不带 -arch 的 otool -L 按架构重复输出整段依赖，
# 第二段里插件自身的 LC_ID_DYLIB（macdeployqt 留下的 Bundle 内绝对路径）曾被当成未部署的
# 开发机依赖，arm64 打包因此失败。单架构的 Homebrew 库只有一段，看不出这个问题，所以用
# 真实的双架构 dylib 验证；同时保留一个确实引用外部绝对路径的插件，确认检查没有被放松。
if [[ "$(uname -s)" == "Darwin" ]] && command -v clang >/dev/null 2>&1; then
    deps_arch="$(uname -m)"
    deps_app="${WORK_ROOT}/Deps.app"
    mkdir -p "${deps_app}/Contents/MacOS" "${deps_app}/Contents/Frameworks" \
        "${deps_app}/Contents/PlugIns/platforms" "${WORK_ROOT}/outside"
    printf 'int main(void) { return 0; }\n' > "${WORK_ROOT}/deps_main.c"
    printf 'int shellium_core(void) { return 1; }\n' > "${WORK_ROOT}/deps_core.c"
    printf 'int shellium_core(void);\nint shellium_plugin(void) { return shellium_core(); }\n' \
        > "${WORK_ROOT}/deps_plugin.c"
    universal=(-arch arm64 -arch x86_64)
    clang "${universal[@]}" -dynamiclib -install_name @rpath/libcore.dylib \
        -o "${deps_app}/Contents/Frameworks/libcore.dylib" "${WORK_ROOT}/deps_core.c"
    clang "${universal[@]}" -o "${deps_app}/Contents/MacOS/Deps" "${WORK_ROOT}/deps_main.c"
    deps_plugin="${deps_app}/Contents/PlugIns/platforms/libqcocoa.dylib"
    clang "${universal[@]}" -dynamiclib -install_name "${deps_plugin}" \
        -o "${deps_plugin}" "${WORK_ROOT}/deps_plugin.c" "${deps_app}/Contents/Frameworks/libcore.dylib"
    check_deps() {
        bash "${SCRIPTS_DIR}/verify_macos_bundle_dependencies.sh" "${deps_app}" "${deps_arch}" \
            > "${WORK_ROOT}/deps.txt" 2>&1
    }
    if check_deps; then
        pass_case "universal 插件自身的 install id 不算外部依赖"
    else
        fail_case "universal 插件自身的 install id 不算外部依赖：$(cat "${WORK_ROOT}/deps.txt")"
    fi

    clang "${universal[@]}" -dynamiclib -install_name "${WORK_ROOT}/outside/libleak.dylib" \
        -o "${WORK_ROOT}/outside/libleak.dylib" "${WORK_ROOT}/deps_core.c"
    clang "${universal[@]}" -dynamiclib -install_name @rpath/libleaky.dylib \
        -o "${deps_app}/Contents/PlugIns/platforms/libleaky.dylib" "${WORK_ROOT}/deps_plugin.c" \
        "${WORK_ROOT}/outside/libleak.dylib"
    if check_deps; then
        fail_case "引用 Bundle 外绝对路径的插件必须失败，但脚本成功了"
    elif grep -q 'libleaky.dylib -> .*outside/libleak.dylib' "${WORK_ROOT}/deps.txt"; then
        pass_case "引用 Bundle 外绝对路径的插件导致失败"
    else
        fail_case "外部依赖报错没有指出 libleak.dylib：$(cat "${WORK_ROOT}/deps.txt")"
    fi
else
    printf 'SKIP Bundle 依赖检查（非 macOS 或未安装 clang）\n'
fi

if [[ "${failures}" -gt 0 ]]; then
    printf '%d 个用例失败\n' "${failures}" >&2
    exit 1
fi
printf '全部用例通过\n'
