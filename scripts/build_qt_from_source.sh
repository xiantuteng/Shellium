#!/usr/bin/env bash
set -euo pipefail

# 从源码编译 Shellium 用到的 Qt 模块（qtbase、qtsvg、qttools 中的 Linguist），供 Linux aarch64 使用。
#
# 官方 Qt 6.8 的 aarch64 二进制在 Ubuntu 24.04 上构建，要求 GLIBC_2.38，无法在 Ubuntu 22.04 上运行；
# x86_64 的官方二进制在 RHEL 8 上构建（glibc 2.28），直接用即可。这里在构建容器（Ubuntu 22.04）
# 里自行编译，选项向官方二进制看齐：xcb 平台插件、fontconfig、D-Bus、桌面 OpenGL，OpenSSL 按运行时
# 加载（libssl.so.3）。显式打开的特性在缺少系统依赖时会让配置直接失败，而不是悄悄编出一个缺
# xcb 插件、启动即退出的 Qt。ICU、GLib 关闭以减少需要打包的系统库。
#
# 编译约需一小时，CI 中按脚本内容缓存安装目录。
#
# 用法：build_qt_from_source.sh <安装前缀>
# 依赖：cmake、ninja、C/C++ 编译器、perl、python3、curl，以及 release.yml 中列出的 X11/xcb/GL 开发包。

QT_VERSION="6.8.3"
declare -A QT_MODULE_SHA256=(
    [qtbase]="56001b905601bb9023d399f3ba780d7fa940f3e4861e496a7c490331f49e0b80"
    [qtsvg]="35eb516460f00f264eb504baa253432384351cf23fb9980a5857190e8deef438"
    [qttools]="02a4e219248b94f1333df843d25763f35251c1074cdc4fb5bda67d340f8c8b3a"
)

prefix="$1"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
work_dir="$(mktemp -d "${TMPDIR:-/tmp}/shellium-qt.XXXXXX")"
trap 'rm -rf -- "${work_dir}"' EXIT

for module in qtbase qtsvg qttools; do
    archive="${work_dir}/${module}.tar.xz"
    "${script_dir}/fetch_verified_source.sh" \
        "https://download.qt.io/archive/qt/${QT_VERSION%.*}/${QT_VERSION}/submodules/${module}-everywhere-src-${QT_VERSION}.tar.xz" \
        "${QT_MODULE_SHA256[${module}]}" "${archive}"
    tar -xf "${archive}" -C "${work_dir}"
    rm -f -- "${archive}"
done

cmake -S "${work_dir}/qtbase-everywhere-src-${QT_VERSION}" -B "${work_dir}/build-qtbase" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${prefix}" \
    -DQT_BUILD_EXAMPLES=OFF \
    -DQT_BUILD_TESTS=OFF \
    -DFEATURE_xcb=ON \
    -DFEATURE_xkbcommon=ON \
    -DFEATURE_fontconfig=ON \
    -DFEATURE_dbus=ON \
    -DFEATURE_opengl=ON \
    -DINPUT_opengl=desktop \
    -DINPUT_openssl=runtime \
    -DFEATURE_icu=OFF \
    -DFEATURE_glib=OFF \
    -DFEATURE_sql_sqlite=ON \
    -DFEATURE_sql_psql=OFF \
    -DFEATURE_sql_mysql=OFF \
    -DFEATURE_sql_odbc=OFF \
    -DFEATURE_sql_ibase=OFF \
    -DFEATURE_system_sqlite=OFF \
    -DFEATURE_system_harfbuzz=OFF \
    -DFEATURE_system_pcre2=OFF \
    -DFEATURE_system_png=OFF \
    -DFEATURE_system_jpeg=OFF \
    -DFEATURE_system_doubleconversion=OFF
cmake --build "${work_dir}/build-qtbase" --parallel
cmake --install "${work_dir}/build-qtbase"

"${prefix}/bin/qt-cmake" -S "${work_dir}/qtsvg-everywhere-src-${QT_VERSION}" -B "${work_dir}/build-qtsvg" -G Ninja \
    -DQT_BUILD_EXAMPLES=OFF \
    -DQT_BUILD_TESTS=OFF
cmake --build "${work_dir}/build-qtsvg" --parallel
cmake --install "${work_dir}/build-qtsvg"

# 工程只需要 LinguistTools（lupdate/lrelease），其余工具一律关掉，省时间也省依赖。
"${prefix}/bin/qt-cmake" -S "${work_dir}/qttools-everywhere-src-${QT_VERSION}" -B "${work_dir}/build-qttools" -G Ninja \
    -DQT_BUILD_EXAMPLES=OFF \
    -DQT_BUILD_TESTS=OFF \
    -DFEATURE_linguist=ON \
    -DFEATURE_assistant=OFF \
    -DFEATURE_designer=OFF \
    -DFEATURE_distancefieldgenerator=OFF \
    -DFEATURE_pixeltool=OFF \
    -DFEATURE_qdoc=OFF \
    -DFEATURE_qtdiag=OFF \
    -DFEATURE_qtplugininfo=OFF \
    -DFEATURE_clang=OFF
cmake --build "${work_dir}/build-qttools" --parallel
cmake --install "${work_dir}/build-qttools"

[[ -f "${prefix}/plugins/platforms/libqxcb.so" ]] || {
    printf 'Qt 编译完成但缺少 xcb 平台插件：%s\n' "${prefix}/plugins/platforms/libqxcb.so" >&2
    exit 1
}
printf 'Qt %s 已安装到 %s\n' "${QT_VERSION}" "${prefix}"
