#!/usr/bin/env bash

# Shellium macOS Apple Silicon 一键发布脚本。
#
# 正式模式会执行“Release 编译 → 运行时依赖部署 → Developer ID 签名 → DMG 制作 →
# Apple 公证 → 公证票据装订 → Gatekeeper 验证”的完整链路。脚本遵循失败即停止原则：任何
# 一个安全检查不通过都不会留下看似可发布的正式 DMG，避免误把未签名或未公证产物交付给用户。
#
# 本地开发者在尚未取得 Developer ID 证书时，可以使用 --adhoc 验证构建和依赖打包流程。
# 该模式的文件名会显式包含 adhoc，且绝不能作为正式对外发布物。

set -Eeuo pipefail

# 解析脚本自身位置，而不是依赖调用者当前目录。这样从 Finder、终端或 CI 任意目录调用时，
# CMake 的源码路径、构建路径和产物路径仍然保持一致。
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# 以下配置均允许通过同名环境变量覆盖。默认值适合当前仓库的一键构建，同时保留发布者替换
# Bundle Identifier、最低系统版本、签名身份和公证钥匙串配置的能力。
BUILD_DIR="${SHELLIUM_BUILD_DIR:-${PROJECT_ROOT}/build-release-arm64}"
DIST_DIR="${SHELLIUM_DIST_DIR:-${PROJECT_ROOT}/dist}"
BUNDLE_IDENTIFIER="${SHELLIUM_BUNDLE_IDENTIFIER:-com.shellium.Shellium}"
NOTARY_PROFILE="${SHELLIUM_NOTARY_PROFILE:-Shellium-notary}"
SIGNING_IDENTITY="${SHELLIUM_SIGNING_IDENTITY:-}"
PARALLEL_JOBS="${SHELLIUM_BUILD_JOBS:-4}"

# 当前 Homebrew 的 libssh、libvterm、libsodium、spdlog 和 fmt 均以 macOS 26.0 为最低版本
# 构建。默认值必须忠实反映已部署二进制的真实要求，不能在 Info.plist 中虚报更低兼容版本。
# 如果以后改用针对旧系统重新编译的依赖，可以通过环境变量安全下调此值。
DEPLOYMENT_TARGET="${SHELLIUM_DEPLOYMENT_TARGET:-26.0}"

# 发布版本默认从顶层 CMakeLists.txt 的 project(...) 声明读取，确保 DMG 文件名、App Bundle
# 元数据与工程版本一致。环境变量仍可用于发布候选版本等特殊场景。
PROJECT_VERSION="${SHELLIUM_VERSION:-$(sed -nE 's/^project\(Shellium VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' "${PROJECT_ROOT}/CMakeLists.txt" | head -n 1)}"

# mode=release 表示正式 Developer ID 发布；mode=adhoc 仅用于本机验证。notarize=false 只允许
# 与显式 --skip-notarization 搭配，避免因网络临时问题而静默跳过用户所要求的 Apple 公证。
MODE="release"
NOTARIZE="true"
CHECK_ONLY="false"
TEMP_ROOT=""

# 输出统一格式的阶段提示，便于人在终端查看，也便于 CI 日志按阶段定位失败原因。
log() {
    printf '\n==> %s\n' "$*"
}

# 输出错误信息并返回非零状态。所有调用点都通过该函数说明失败原因和修复方向，避免用户只
# 看到下游工具晦涩的退出码。
fail() {
    printf '\n错误：%s\n' "$*" >&2
    exit 1
}

# 展示脚本参数和安全边界。正式发布所需的敏感密码不通过参数或环境变量传递，而是预先保存
# 到 macOS 钥匙串，防止密码进入 shell 历史、进程列表或 CI 明文日志。
usage() {
    cat <<'EOF'
用法：
  ./scripts/package_macos_dmg.sh [--check] [--adhoc] [--skip-notarization]

模式：
  无参数                 构建、Developer ID 签名、公证并验证正式 arm64 DMG
  --check                只检查本机工具、证书和公证凭据，不生成文件
  --adhoc                生成仅供本机测试的临时签名 DMG，不能对外发布
  --skip-notarization    生成已签名但未公证的 DMG，仅用于受控测试

可选环境变量：
  SHELLIUM_BUNDLE_IDENTIFIER   Bundle Identifier，默认 com.shellium.Shellium
  SHELLIUM_SIGNING_IDENTITY    完整 Developer ID Application 身份；仅有一个时可自动识别
  SHELLIUM_NOTARY_PROFILE      notarytool 钥匙串配置名，默认 Shellium-notary
  SHELLIUM_DEPLOYMENT_TARGET   最低 macOS 版本，当前依赖默认要求 26.0
  SHELLIUM_BUILD_DIR           Release 构建目录
  SHELLIUM_DIST_DIR            DMG 输出目录
  SHELLIUM_BUILD_JOBS          并行编译任务数，默认 4
EOF
}

# 删除脚本通过 mktemp 创建的唯一临时目录。只允许删除带 shellium-package. 前缀且位于系统
# 临时目录中的路径，从而即使变量意外为空或被篡改，也不会递归删除项目或用户目录。
cleanup() {
    if [[ -n "${TEMP_ROOT}" && -d "${TEMP_ROOT}" && "$(basename "${TEMP_ROOT}")" == shellium-package.* ]]; then
        rm -rf -- "${TEMP_ROOT}"
    fi
}
trap cleanup EXIT

# 检查命令是否存在，并给出缺失命令的安装提示。参数 1 是可执行文件名，参数 2 是面向用户的
# 工具来源说明；函数没有返回内容，失败时会立即终止脚本。
require_command() {
    local command_name="$1"
    local install_hint="$2"

    command -v "${command_name}" >/dev/null 2>&1 \
        || fail "缺少 ${command_name}。${install_hint}"
}

# 验证正整数配置，主要防止并行任务数为空、为零或包含 shell 特殊字符。参数 1 是配置名称，
# 参数 2 是待验证值；检查通过时不输出内容。
require_positive_integer() {
    local variable_name="$1"
    local variable_value="$2"

    [[ "${variable_value}" =~ ^[1-9][0-9]*$ ]] \
        || fail "${variable_name} 必须是正整数，当前值为：${variable_value}"
}

# 识别钥匙串内唯一有效的 Developer ID Application 身份。若用户显式设置身份，则同时验证
# 该身份确实可用于 codesign；若存在多个候选身份，则要求明确选择，避免意外使用错误团队证书。
resolve_signing_identity() {
    local identity_output
    local -a detected_identities=()

    identity_output="$(security find-identity -v -p codesigning 2>/dev/null || true)"

    if [[ -n "${SIGNING_IDENTITY}" ]]; then
        grep -Fq "\"${SIGNING_IDENTITY}\"" <<<"${identity_output}" \
            || fail "指定的签名身份在钥匙串中无效或缺少私钥：${SIGNING_IDENTITY}"
        [[ "${SIGNING_IDENTITY}" == Developer\ ID\ Application:* ]] \
            || fail "正式站外分发必须使用 Developer ID Application 证书。"
        return
    fi

    while IFS= read -r identity; do
        [[ -n "${identity}" ]] && detected_identities+=("${identity}")
    done < <(sed -nE 's/^[[:space:]]*[0-9]+\) [[:xdigit:]]+ "(Developer ID Application:[^"]+)".*/\1/p' <<<"${identity_output}")

    case "${#detected_identities[@]}" in
        0)
            fail "未找到 Developer ID Application 证书及私钥。请先安装有效发布证书。"
            ;;
        1)
            SIGNING_IDENTITY="${detected_identities[0]}"
            ;;
        *)
            printf '%s\n' "检测到多个 Developer ID Application 身份：" >&2
            printf '  - %s\n' "${detected_identities[@]}" >&2
            fail "请通过 SHELLIUM_SIGNING_IDENTITY 指定本次发布使用的完整身份。"
            ;;
    esac
}

# 通过 notarytool 读取一次公证历史来验证钥匙串配置。该操作不会上传文件或修改服务器状态，
# 但可以提前发现配置名错误、凭据失效、Team ID 不匹配或网络不可达，避免构建结束才失败。
validate_notary_profile() {
    local validation_output

    if ! validation_output="$(xcrun notarytool history \
        --keychain-profile "${NOTARY_PROFILE}" \
        --output-format json 2>&1)"; then
        printf '%s\n' "${validation_output}" >&2
        fail "公证钥匙串配置 ${NOTARY_PROFILE} 验证失败。请重新执行 notarytool store-credentials。"
    fi
}

# 执行不产生构建文件的前置检查。此函数验证平台、工具链、版本配置、签名身份与公证凭据；
# 正式模式的任何关键条件缺失都会失败，而 adhoc 模式只跳过外部发布凭据检查。
preflight() {
    log "检查 Apple Silicon 发布环境"

    [[ "$(uname -s)" == "Darwin" ]] || fail "该脚本只能在 macOS 上运行。"
    [[ "$(uname -m)" == "arm64" ]] || fail "必须在 Apple Silicon 原生 arm64 终端中运行。"
    [[ -n "${PROJECT_VERSION}" ]] || fail "无法从 CMakeLists.txt 读取项目版本。"
    [[ "${BUNDLE_IDENTIFIER}" =~ ^[A-Za-z0-9][A-Za-z0-9.-]+$ ]] \
        || fail "Bundle Identifier 格式无效：${BUNDLE_IDENTIFIER}"
    [[ "${DEPLOYMENT_TARGET}" =~ ^[0-9]+\.[0-9]+$ ]] \
        || fail "最低系统版本必须使用 major.minor 格式，例如 26.0。"
    require_positive_integer "SHELLIUM_BUILD_JOBS" "${PARALLEL_JOBS}"

    require_command cmake "请安装 CMake 3.21 或更高版本。"
    require_command ninja "请安装 Ninja。"
    require_command pkg-config "请安装 pkg-config。"
    require_command macdeployqt "请安装 Qt 6.8 或更高版本，并把 Qt bin 目录加入 PATH。"
    require_command qtpaths "请安装 Qt 6.8 或更高版本，并把 Qt bin 目录加入 PATH。"
    require_command codesign "请安装 Apple Command Line Tools。"
    require_command hdiutil "该工具应随 macOS 提供。"
    require_command ditto "该工具应随 macOS 提供。"
    require_command otool "请安装 Apple Command Line Tools。"
    require_command lipo "请安装 Apple Command Line Tools。"
    require_command install_name_tool "请安装 Apple Command Line Tools。"
    require_command xcrun "请安装 Apple Command Line Tools。"
    require_command spctl "该工具应随 macOS 提供。"

    # CMake 会执行更精确的版本检查；这里先确认所有 pkg-config 包存在，使缺失依赖时的报错
    # 更聚焦，用户无需从很长的 CMake 配置日志中反推具体缺少哪个包。
    pkg-config --exists 'libssh >= 0.10' || fail "缺少 libssh 0.10 或更高版本。"
    pkg-config --exists 'vterm >= 0.3' || fail "缺少 libvterm 0.3 或更高版本。"
    pkg-config --exists 'libsodium >= 1.0.18' || fail "缺少 libsodium 1.0.18 或更高版本。"
    pkg-config --exists 'spdlog >= 1.12' || fail "缺少 spdlog 1.12 或更高版本。"

    if [[ "${MODE}" == "release" ]]; then
        resolve_signing_identity
        if [[ "${NOTARIZE}" == "true" ]]; then
            validate_notary_profile
        fi
        printf '签名身份：%s\n' "${SIGNING_IDENTITY}"
        printf '公证配置：%s\n' "${NOTARY_PROFILE}"
    else
        printf '%s\n' "警告：adhoc 模式产物只用于本机验证，不能对外发布。"
    fi

    printf 'Bundle ID：%s\n' "${BUNDLE_IDENTIFIER}"
    printf '最低系统：macOS %s\n' "${DEPLOYMENT_TARGET}"
    printf '目标架构：arm64\n'
}

# 判断指定文件是否为 Mach-O 二进制。参数 1 是文件路径；返回 0 表示 Mach-O，返回 1 表示
# 普通资源文件。使用 file 而非扩展名是因为 Qt Framework 主二进制通常没有文件扩展名。
is_macho_file() {
    local candidate_path="$1"
    file -b "${candidate_path}" 2>/dev/null | grep -q 'Mach-O'
}

# 验证 App Bundle 内所有原生代码都包含 arm64 架构，并检查其动态库引用不再指向 Homebrew、
# /usr/local 或源码目录。参数 1 是 App 路径；该检查防止在开发机正常、换到用户机器立即崩溃。
verify_bundle_architecture_and_dependencies() {
    local app_path="$1"
    local candidate_path
    local candidate_directory
    local architecture_list
    local dependency_path
    local dependency_index
    local resolved_dependency_path
    local is_shared_library
    local found_macho="false"

    while IFS= read -r -d '' candidate_path; do
        if ! is_macho_file "${candidate_path}"; then
            continue
        fi
        found_macho="true"
        architecture_list="$(lipo -archs "${candidate_path}")"
        [[ " ${architecture_list} " == *" arm64 "* ]] \
            || fail "Bundle 中存在非 arm64 原生代码：${candidate_path}（${architecture_list}）"

        candidate_directory="$(dirname "${candidate_path}")"
        dependency_index=0
        is_shared_library="false"
        if file -b "${candidate_path}" | grep -q 'dynamically linked shared library'; then
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
                /opt/homebrew/*|/usr/local/*|"${PROJECT_ROOT}"/*)
                    fail "发现未部署的开发机动态库引用：${candidate_path} -> ${dependency_path}"
                    ;;
                @rpath/*)
                    # macdeployqt 为 App 和 Qt 插件配置的运行时搜索根均指向 Contents/Frameworks。
                    # 仅检查字符串已变成 @rpath 不够；目标 Framework 或 dylib 也必须真实存在。
                    resolved_dependency_path="${app_path}/Contents/Frameworks/${dependency_path#@rpath/}"
                    [[ -e "${resolved_dependency_path}" ]] \
                        || fail "Bundle 缺少 @rpath 依赖：${candidate_path} -> ${dependency_path}"
                    ;;
                @loader_path/*)
                    resolved_dependency_path="${candidate_directory}/${dependency_path#@loader_path/}"
                    [[ -e "${resolved_dependency_path}" ]] \
                        || fail "Bundle 缺少 @loader_path 依赖：${candidate_path} -> ${dependency_path}"
                    ;;
                @executable_path/*)
                    resolved_dependency_path="${app_path}/Contents/MacOS/${dependency_path#@executable_path/}"
                    [[ -e "${resolved_dependency_path}" ]] \
                        || fail "Bundle 缺少 @executable_path 依赖：${candidate_path} -> ${dependency_path}"
                    ;;
            esac
        done < <(otool -L "${candidate_path}" | tail -n +2 | awk '{print $1}')
    done < <(find "${app_path}" -type f -print0)

    [[ "${found_macho}" == "true" ]] || fail "App Bundle 内没有找到 Mach-O 可执行文件。"
}

# 配置并编译 Release App。脚本显式固定 arm64 和最低系统版本，避免复用旧 CMake Cache 时
# 意外生成 x86_64、Universal Binary 或带 Debug 行为的安装包。
build_release_app() {
    log "配置 arm64 Release 工程"
    cmake -S "${PROJECT_ROOT}" -B "${BUILD_DIR}" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTING=OFF \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="${DEPLOYMENT_TARGET}" \
        -DSHELLIUM_BUNDLE_IDENTIFIER="${BUNDLE_IDENTIFIER}"

    log "编译 Shellium.app"
    cmake --build "${BUILD_DIR}" --target Shellium --parallel "${PARALLEL_JOBS}"
}

# 使用当前发布模式重新签署一个被手工改写过的 Mach-O 文件。参数 1 是文件路径；正式模式
# 使用 Hardened Runtime 和安全时间戳，adhoc 模式只恢复 Apple Silicon 运行所需的临时签名。
sign_repaired_macho() {
    local macho_path="$1"

    if [[ "${MODE}" == "release" ]]; then
        codesign --force --options runtime --timestamp --sign "${SIGNING_IDENTITY}" "${macho_path}"
    else
        codesign --force --sign - "${macho_path}"
    fi
}

# 在内层代码修改完成后重新签署最外层 App。参数 1 是 App 路径；这里不使用 --deep，原因是
# 所有嵌套代码已经由 macdeployqt 或 sign_repaired_macho 分别签好，外层签名只负责重新密封
# Bundle 资源。这样可以避免 --deep 用统一参数覆盖 Framework/插件已有的正确签名配置。
resign_outer_app() {
    local app_path="$1"

    if [[ "${MODE}" == "release" ]]; then
        codesign --force --options runtime --timestamp --sign "${SIGNING_IDENTITY}" "${app_path}"
    else
        codesign --force --sign - "${app_path}"
    fi
}

# 修复 Homebrew 独立 qtsvg formula 的部署。参数 1 是 App，参数 2 是 Qt 库目录，参数 3 是
# Qt 插件目录。macdeployqt 会复制 libqsvgicon，却无法跨 qtbase/qtsvg formula 定位 Framework；
# 本函数复制 QtSvg、把其 Homebrew 绝对依赖改成 Bundle 内 @rpath，并按由内到外顺序重新签名。
repair_qt_svg_deployment() {
    local app_path="$1"
    local qt_libdir="$2"
    local qt_plugins_dir="$3"
    local source_plugin="${qt_plugins_dir}/iconengines/libqsvgicon.dylib"
    local bundled_plugin="${app_path}/Contents/PlugIns/iconengines/libqsvgicon.dylib"
    local source_framework="${qt_libdir}/QtSvg.framework"
    local bundled_framework="${app_path}/Contents/Frameworks/QtSvg.framework"
    local bundled_binary="${bundled_framework}/Versions/A/QtSvg"
    local dependency_path
    local bundled_dependency_path

    # 未安装 SVG 插件或 macdeployqt 没有选择该插件时无需引入额外 Framework。该分支让使用
    # 官方 Qt 安装器、精简 Qt 构建等环境仍可正常打包。
    if [[ ! -f "${source_plugin}" || ! -f "${bundled_plugin}" ]]; then
        return
    fi

    [[ -d "${source_framework}" ]] \
        || fail "检测到 Qt SVG 插件但缺少 QtSvg.framework，请安装与 Qt 版本一致的 qtsvg。"

    ditto "${source_framework}" "${bundled_framework}"
    [[ -f "${bundled_binary}" ]] || fail "复制后未找到 QtSvg Framework 主二进制。"

    # 统一 Framework 自身 ID，使插件无论 App 安装到哪个目录都通过 @rpath 加载 Bundle 内副本。
    install_name_tool -id '@rpath/QtSvg.framework/Versions/A/QtSvg' "${bundled_binary}"

    while IFS= read -r dependency_path; do
        case "${dependency_path}" in
            /opt/homebrew/*.framework/*|/usr/local/*.framework/*)
                # 取最后一个 /lib/ 之后的 Framework 相对路径。例如 Homebrew 的
                # /opt/.../lib/QtGui.framework/... 会转换成 @rpath/QtGui.framework/...。
                bundled_dependency_path="${dependency_path##*/lib/}"
                install_name_tool -change "${dependency_path}" \
                    "@rpath/${bundled_dependency_path}" "${bundled_binary}"
                ;;
        esac
    done < <(otool -L "${bundled_binary}" | tail -n +3 | awk '{print $1}')

    sign_repaired_macho "${bundled_binary}"
    resign_outer_app "${app_path}"
}

# 把 App 安装到临时暂存目录并调用 macdeployqt 收集 Qt Framework、插件和第三方动态库。
# 参数 1 是暂存根目录，返回值通过全局 STAGED_APP 传递，避免使用命令替换吞掉部署日志。
deploy_runtime_dependencies() {
    local stage_root="$1"
    local -a deploy_arguments
    local package_name
    local package_libdir
    local qt_libdir
    local qt_plugins_dir

    log "安装 App Bundle 到隔离暂存目录"
    cmake --install "${BUILD_DIR}" --prefix "${stage_root}"
    STAGED_APP="${stage_root}/Shellium.app"
    [[ -d "${STAGED_APP}" ]] || fail "CMake 安装后未找到 ${STAGED_APP}。"

    deploy_arguments=("${STAGED_APP}" -always-overwrite -verbose=1)

    # Homebrew 会把 QtSvg 等附加 Qt 模块安装到统一的 QT_INSTALL_LIBS，而 macdeployqt 默认
    # 只搜索 qtbase 自身目录。显式加入该目录，保证自动部署的 SVG 插件能同时找到 QtSvg。
    qt_libdir="$(qtpaths --query QT_INSTALL_LIBS)"
    [[ -d "${qt_libdir}" ]] || fail "Qt 库目录不存在：${qt_libdir}"
    deploy_arguments+=("-libpath=${qt_libdir}")

    # Homebrew 的 qtsvg 是独立于 qtbase 的 formula，但插件位于统一目录。记录插件路径供
    # macdeployqt 结束后的兼容修复使用；官方 Qt 或没有 qtsvg 的环境不会进入该修复分支。
    qt_plugins_dir="$(qtpaths --query QT_INSTALL_PLUGINS)"

    # macdeployqt 对 Homebrew 非 Qt 动态库的自动搜索可能受 Cellar 版本路径影响。显式传入
    # 每个包的 libdir 可确保 libssh 等依赖及其间接依赖被复制并改写安装名。
    for package_name in libssh vterm libsodium spdlog; do
        package_libdir="$(pkg-config --variable=libdir "${package_name}")"
        deploy_arguments+=("-libpath=${package_libdir}")
    done

    if [[ "${MODE}" == "release" ]]; then
        # -sign-for-notarization 会同时启用 Hardened Runtime、安全时间戳，并按正确顺序签署
        # Framework、插件、动态库和最外层 App，优于仅对外层 Bundle 使用 codesign --deep。
        deploy_arguments+=("-sign-for-notarization=${SIGNING_IDENTITY}")
    else
        # 不传 codesign 选项时 macdeployqt 默认执行 ad-hoc 签名，可修复部署工具改写 Mach-O
        # 安装名后失效的链接器签名，保证 Apple Silicon 本机能够启动验证。
        printf '%s\n' "正在执行 ad-hoc 签名；该签名不具备发布信任链。"
    fi

    log "部署 Qt 与第三方运行时依赖"
    macdeployqt "${deploy_arguments[@]}"

    # macdeployqt 在 Homebrew 分包 Qt 环境下会输出一次 QtSvg 无法解析的诊断，但仍返回成功。
    # 修复函数必须紧接部署执行，后续严格依赖检查会再次确认 Framework 已存在且无绝对路径。
    repair_qt_svg_deployment "${STAGED_APP}" "${qt_libdir}" "${qt_plugins_dir}"
}

# 验证最外层 App 及所有嵌套代码签名。参数 1 是 App 路径；正式模式额外检查 Hardened Runtime
# 标志和 Developer ID Authority，防止证书选错或部署工具没有应用公证所需选项。
verify_app_signature() {
    local app_path="$1"
    local signature_details

    log "验证 App Bundle 架构、依赖与签名"
    verify_bundle_architecture_and_dependencies "${app_path}"
    codesign --verify --deep --strict --verbose=2 "${app_path}"

    if [[ "${MODE}" == "release" ]]; then
        signature_details="$(codesign --display --verbose=4 "${app_path}" 2>&1)"
        grep -Fq 'Authority=Developer ID Application:' <<<"${signature_details}" \
            || fail "App 未使用 Developer ID Application 证书签名。"
        grep -Eq '^flags=.*runtime' <<<"${signature_details}" \
            || fail "App 签名没有启用 Hardened Runtime。"
    fi
}

# 创建带 Applications 快捷方式的压缩 DMG。参数 1 是已部署 App，参数 2 是临时 DMG 路径；
# 正式输出只会在全部验证通过后移动到 dist，避免中途失败留下半成品。
create_dmg() {
    local app_path="$1"
    local output_dmg="$2"
    local dmg_root="${TEMP_ROOT}/dmg-root"

    mkdir -p "${dmg_root}"
    ditto "${app_path}" "${dmg_root}/Shellium.app"
    ln -s /Applications "${dmg_root}/Applications"

    log "创建压缩 DMG"
    hdiutil create \
        -volname "Shellium" \
        -srcfolder "${dmg_root}" \
        -format UDZO \
        -ov \
        "${output_dmg}"

    if [[ "${MODE}" == "release" ]]; then
        # DMG 是用户实际下载的最外层容器，因此也使用同一 Developer ID 身份和安全时间戳签名。
        # 这能让 Gatekeeper 校验容器在下载后未被替换或篡改。
        codesign --force --timestamp --sign "${SIGNING_IDENTITY}" "${output_dmg}"
        codesign --verify --verbose=2 "${output_dmg}"
    fi

    hdiutil verify "${output_dmg}"
}

# 提交 DMG 给 Apple 公证服务，等待明确 Accepted 后装订票据。参数 1 是 DMG 路径；任何
# Invalid、超时或网络错误都会保留错误状态并终止，不会把未公证文件伪装成最终产物。
notarize_and_staple_dmg() {
    local dmg_path="$1"
    local submission_output

    log "提交 Apple 公证并等待结果"
    if ! submission_output="$(xcrun notarytool submit "${dmg_path}" \
        --keychain-profile "${NOTARY_PROFILE}" \
        --wait \
        --timeout 30m \
        --output-format json)"; then
        printf '%s\n' "${submission_output}" >&2
        fail "Apple 公证提交失败。"
    fi
    printf '%s\n' "${submission_output}"

    # 不依赖 jq，直接匹配 notarytool 稳定输出的 JSON status 字段。这样脚本不增加额外发布
    # 依赖，同时严格确认服务端最终状态为 Accepted，而不是只根据进程退出码推断成功。
    if ! grep -Eq '"status"[[:space:]]*:[[:space:]]*"Accepted"' <<<"${submission_output}"; then
        fail "Apple 公证未返回 Accepted，请根据上方 submission id 查询详细日志。"
    fi

    log "装订并验证 Apple 公证票据"
    xcrun stapler staple "${dmg_path}"
    xcrun stapler validate "${dmg_path}"
}

# 对最终交付物执行 Gatekeeper 和完整性检查。参数 1 是 DMG 路径；正式且已公证模式要求
# spctl 接受主签名，未公证受控测试模式只做签名与镜像校验并明确输出安全警告。
verify_final_artifact() {
    local dmg_path="$1"

    log "执行最终安装包验证"
    hdiutil verify "${dmg_path}"

    if [[ "${MODE}" == "release" && "${NOTARIZE}" == "true" ]]; then
        spctl --assess --type open --context context:primary-signature --verbose=2 "${dmg_path}"
    elif [[ "${MODE}" == "release" ]]; then
        printf '%s\n' "警告：DMG 已签名但未公证，Gatekeeper 可能阻止其他用户直接打开。"
    else
        printf '%s\n' "警告：这是 ad-hoc 本地验证产物，不具备 Developer ID 信任链。"
    fi
}

# 解析少量互斥参数。未知参数直接失败，避免拼写错误导致脚本意外按正式模式执行。
while [[ "$#" -gt 0 ]]; do
    case "$1" in
        --check)
            CHECK_ONLY="true"
            ;;
        --adhoc)
            MODE="adhoc"
            NOTARIZE="false"
            ;;
        --skip-notarization)
            NOTARIZE="false"
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage >&2
            fail "未知参数：$1"
            ;;
    esac
    shift
done

preflight

if [[ "${CHECK_ONLY}" == "true" ]]; then
    log "前置条件检查完成"
    exit 0
fi

TEMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/shellium-package.XXXXXX")"
STAGED_APP=""
mkdir -p "${DIST_DIR}"

if [[ "${MODE}" == "release" ]]; then
    OUTPUT_DMG="${DIST_DIR}/Shellium-${PROJECT_VERSION}-macos-arm64.dmg"
    WORK_DMG="${TEMP_ROOT}/Shellium-${PROJECT_VERSION}-macos-arm64.dmg"
else
    OUTPUT_DMG="${DIST_DIR}/Shellium-${PROJECT_VERSION}-macos-arm64-adhoc.dmg"
    WORK_DMG="${TEMP_ROOT}/Shellium-${PROJECT_VERSION}-macos-arm64-adhoc.dmg"
fi

build_release_app
deploy_runtime_dependencies "${TEMP_ROOT}/stage"
verify_app_signature "${STAGED_APP}"
create_dmg "${STAGED_APP}" "${WORK_DMG}"

if [[ "${MODE}" == "release" && "${NOTARIZE}" == "true" ]]; then
    notarize_and_staple_dmg "${WORK_DMG}"
fi

verify_final_artifact "${WORK_DMG}"

# 只有构建、签名、公证和所有校验均成功到达这里时，才替换 dist 中的同版本产物。mv 在同一
# 文件系统内完成得足够快，可避免用户或后续自动化读取到正在写入的 DMG。
mv -f -- "${WORK_DMG}" "${OUTPUT_DMG}"
printf '\n构建完成：%s\n' "${OUTPUT_DMG}"
