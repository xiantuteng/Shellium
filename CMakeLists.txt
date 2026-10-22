cmake_minimum_required(VERSION 3.21)

project(Shellium VERSION 0.3.8 LANGUAGES CXX)

# 正式 macOS 安装包必须使用稳定且可配置的 Bundle Identifier。这里提供一个可直接构建的
# 默认值，同时允许发布脚本通过 -DSHELLIUM_BUNDLE_IDENTIFIER=... 覆盖；这样企业或个人发布者
# 可以在不修改源码的情况下换成自己控制的反向域名，并保证后续版本升级时应用身份保持稳定。
set(SHELLIUM_BUNDLE_IDENTIFIER "com.shellium.Shellium" CACHE STRING
    "Shellium macOS App Bundle 的唯一标识符")

# Qt 的元对象编译器负责生成信号、槽以及属性系统所需代码。开启这些自动化选项后，
# 新增 QObject 子类时无需手工维护 moc 文件列表，可减少跨平台构建差异。
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Widgets Concurrent Network Sql Svg LinguistTools)

# macOS 的自定义标题行依赖 Qt 6.9 新增的 Qt::ExpandedClientAreaHint 才能伸进交通灯所在的
# 标题区。用 6.8 构建不会报错，但窗口顶部会多出一条系统标题栏、标题行整体下移（0.3.3/0.3.4
# 的 DMG 就是这样发出去的），所以在配置阶段直接拒绝。
if(APPLE AND Qt6_VERSION VERSION_LESS 6.9)
    message(FATAL_ERROR
        "macOS builds need Qt >= 6.9 (found ${Qt6_VERSION}): the custom title bar relies on "
        "Qt::ExpandedClientAreaHint to extend into the native title bar area.")
endif()

# 业务目标只链接项目内部的稳定依赖名。Windows/vcpkg 通常提供 CMake Config Package，
# macOS Homebrew 与 Linux 发行版则普遍提供 pkg-config；先尝试前者、再统一回退到后者，
# 可以让三平台共用其余 CMake 而不在每个测试目标中散落平台判断。
# libssh 下限是 0.11：代码用到 sftp_aio、SSH_OPTIONS_PROXYJUMP、ssh_channel_get_exit_state、
# ssh_channel_request_pty_size_modes 与 ssh_pki_export_privkey_base64_format，0.10 只会在编译期报错。
find_package(libssh 0.11 CONFIG QUIET)
find_package(libvterm 0.3 CONFIG QUIET)
find_package(sodium 1.0.18 CONFIG QUIET)
find_package(unofficial-sodium CONFIG QUIET)
find_package(unofficial-libvterm CONFIG QUIET)
find_package(spdlog 1.12 CONFIG QUIET)
# vcpkg 的 minizip 只提供 CMake Config（MINIZIP::minizip），Windows 上没有 pkg-config 可回退。
find_package(minizip CONFIG QUIET)

find_package(PkgConfig QUIET)

function(shellium_resolve_dependency stable_name package_name minimum_version)
    set(candidate_targets ${ARGN})
    foreach(candidate IN LISTS candidate_targets)
        if(TARGET "${candidate}")
            add_library("ShelliumDependencies::${stable_name}" INTERFACE IMPORTED GLOBAL)
            set_property(TARGET "ShelliumDependencies::${stable_name}"
                PROPERTY INTERFACE_LINK_LIBRARIES "${candidate}")
            return()
        endif()
    endforeach()

    if(NOT PkgConfig_FOUND)
        message(FATAL_ERROR
            "找不到 ${package_name} ${minimum_version}+：请通过 vcpkg/CMake Config 安装，"
            "或在 POSIX 平台安装 pkg-config 元数据。")
    endif()
    string(TOUPPER "${stable_name}" pkg_prefix)
    pkg_check_modules("${pkg_prefix}" REQUIRED IMPORTED_TARGET
        "${package_name}>=${minimum_version}")
    add_library("ShelliumDependencies::${stable_name}" INTERFACE IMPORTED GLOBAL)
    set_property(TARGET "ShelliumDependencies::${stable_name}"
        PROPERTY INTERFACE_LINK_LIBRARIES "PkgConfig::${pkg_prefix}")
endfunction()

shellium_resolve_dependency(libssh libssh 0.11 libssh::libssh ssh)
shellium_resolve_dependency(libvterm vterm 0.3
    libvterm::libvterm vterm::vterm unofficial::libvterm vterm)
shellium_resolve_dependency(libsodium libsodium 1.0.18
    sodium::sodium unofficial-sodium::sodium sodium)
shellium_resolve_dependency(spdlog spdlog 1.12
    spdlog::spdlog spdlog::spdlog_header_only)

# Excel .xlsx 本质上是 ZIP 容器。复用 minizip 处理 ZIP 的目录、Deflate 解压和写入，避免
# 在业务代码里重复实现压缩格式；ExcelHostCodec 只在此基础上解析与生成功能所需的 XML。
# 通过与其他三方库相同的 Config/pkg-config 双路径解析，macOS、Linux 和 vcpkg Windows 环境
# 都可以使用同一个稳定的 ShelliumDependencies::minizip 目标。
shellium_resolve_dependency(minizip minizip 1.2.8
    minizip::minizip MINIZIP::minizip MINIZIP::MINIZIP unofficial::minizip minizip)
# 某些 Linux/Windows 构建会把 minizip 编译为静态库，此时其 pkg-config 的私有依赖 zlib
# 不一定会自动传递给最终可执行文件；显式挂接可以避免“动态库环境能过、静态发布包链接失败”。
find_package(ZLIB REQUIRED)
set_property(TARGET ShelliumDependencies::minizip APPEND PROPERTY
    INTERFACE_LINK_LIBRARIES ZLIB::ZLIB)

# 原生 socket 平台层是 SSH/SFTP/转发共同依赖的基础设施。独立静态库让正式程序和各测试
# 目标复用完全相同的实现，防止只给 GUI 目标补了 Windows 源码而测试目标仍然链接失败。
add_library(ShelliumPlatform STATIC
    src/platform/NativeSocket.cpp
    src/platform/NativeSocket.h
    src/platform/SecureFileWriter.cpp
    src/platform/SecureFileWriter.h
)
target_include_directories(ShelliumPlatform PUBLIC src)
target_link_libraries(ShelliumPlatform PUBLIC Qt6::Core)
if(WIN32)
    target_compile_definitions(ShelliumPlatform PUBLIC
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
    target_link_libraries(ShelliumPlatform PUBLIC Ws2_32 Advapi32)
endif()

# 所有 libssh 使用者都可能通过嵌套会话或转发间接调用平台 socket 层。把依赖挂到项目内
# 稳定目标上，可覆盖正式程序与现有多个拆分测试目标，同时避免重复维护测试清单。
set_property(TARGET ShelliumDependencies::libssh APPEND PROPERTY
    INTERFACE_LINK_LIBRARIES ShelliumPlatform)

# 终端等宽字体随可执行文件一起分发，不依赖用户机器上装了什么。只要走系统字体，缺字体的机器
# 会被静默替换成别的字形，字宽与行高随之改变，终端网格就对不齐了。列表与 BundledFonts.cpp 的
# 清单必须保持一致；许可证与两个 Medium 改名的原因见 resources/fonts/NOTICE.md。
set(SHELLIUM_BUNDLED_FONT_FILES
    resources/fonts/SourceCodePro-Regular.ttf
    resources/fonts/SourceCodeProMedium-Regular.ttf
    resources/fonts/FiraMono-Regular.ttf
    resources/fonts/FiraMonoMedium-Regular.ttf
    resources/fonts/Inconsolata-g.ttf
    resources/fonts/AnonymousPro-Regular.ttf
    resources/fonts/UbuntuMono-Regular.ttf
    resources/fonts/DroidSansMono.ttf
    resources/fonts/DejaVuSansMono.ttf
    resources/fonts/PTMono-Regular.ttf
    resources/fonts/CascadiaCode-Regular.ttf
    resources/fonts/FiraCode-Regular.ttf
    resources/fonts/JetBrainsMono-Regular.ttf
    resources/fonts/MesloLGS-Regular.ttf
)

# 字体资源需要同时编进正式程序和设置界面测试目标，否则测试里 BundledFonts::registerAll()
# 会因为资源缺失而全部失败，测出来的“没有内置字体”只是构建配置问题，不是真实回归。
function(shellium_add_bundled_fonts target)
    qt_add_resources("${target}" "shellium_bundled_fonts"
        PREFIX "/"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES ${SHELLIUM_BUNDLED_FONT_FILES}
    )
endfunction()

# 关于页的“开源软件许可”对话框从 :/licenses/ 读取许可证全文。安装包里分发了 Qt、libssh 等
# 动态库与内置字体，它们的许可证要求随程序附带版权声明与许可证原文；编进资源而不是放在包外，
# 是为了无论用户怎样拷贝 App 或解压 ZIP，声明都不会和程序分离。字体许可证沿用
# resources/fonts/licenses/ 原位置，映射到 :/licenses/fonts/。组件清单见
# OpenSourceLicensesDialog.cpp，新增依赖的流程见 resources/licenses/README.md。
file(GLOB SHELLIUM_LICENSE_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/licenses/*.txt")
file(GLOB SHELLIUM_FONT_LICENSE_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts/licenses/*.txt")
function(shellium_add_license_notices target)
    qt_add_resources("${target}" "shellium_license_notices"
        PREFIX "/licenses"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources/licenses"
        FILES ${SHELLIUM_LICENSE_FILES}
    )
    qt_add_resources("${target}" "shellium_font_license_notices"
        PREFIX "/licenses/fonts"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts/licenses"
        FILES ${SHELLIUM_FONT_LICENSE_FILES}
    )
endfunction()

# 主窗口把标签页、终端、SSH、SFTP、端口转发和 AI 助手串在一起，任何界面级回归测试都必须
# 链接同一套实现。源文件清单因此集中一处，测试目标复用它，避免清单在两处慢慢分叉后测试
# 编译的是一个与正式程序不同的窗口。
set(SHELLIUM_APP_SOURCES
    src/app/MainWindow.cpp
    src/app/MainWindow.h
    src/ai/AiAssistantWidget.cpp
    src/ai/AiAssistantWidget.h
    src/ai/AiChatTypes.h
    src/ai/AiClient.cpp
    src/ai/AiClient.h
    src/ai/AiSettings.h
    src/ai/AiSettingsService.cpp
    src/ai/AiSettingsService.h
    src/ai/CommandRiskClassifier.cpp
    src/ai/CommandRiskClassifier.h
    src/ai/LocalAgentCli.cpp
    src/ai/LocalAgentCli.h
    src/ai/LocalCliClient.cpp
    src/ai/LocalCliClient.h
    src/core/ActivityLog.cpp
    src/core/ActivityLog.h
    src/core/ConnectionTarget.cpp
    src/core/ConnectionTarget.h
    src/core/ExcelHostCodec.cpp
    src/core/ExcelHostCodec.h
    src/i18n/LocalizationService.cpp
    src/i18n/LocalizationService.h
    src/i18n/Retranslate.h
    src/forwarding/PortForwardManagerWidget.cpp
    src/forwarding/PortForwardManagerWidget.h
    src/forwarding/PortForwardRule.h
    src/forwarding/PortForwardRuntime.cpp
    src/forwarding/PortForwardRuntime.h
    src/forwarding/Socks5Protocol.cpp
    src/forwarding/Socks5Protocol.h
    src/core/ConnectionConfigCodec.cpp
    src/core/ConnectionConfigCodec.h
    src/productivity/ProductivityModel.cpp
    src/productivity/ProductivityModel.h
    src/productivity/ProductivityWidget.cpp
    src/productivity/ProductivityWidget.h
    src/productivity/QuickCommandPopup.cpp
    src/productivity/QuickCommandPopup.h
    src/security/Vault.cpp
    src/security/Vault.h
    src/security/SshKeyManager.cpp
    src/security/SshKeyManager.h
    src/settings/BundledFonts.cpp
    src/settings/BundledFonts.h
    src/settings/TerminalSettings.cpp
    src/settings/TerminalSettings.h
    src/storage/ApplicationDatabase.cpp
    src/storage/ApplicationDatabase.h
    src/sftp/RemoteFileManagerWidget.cpp
    src/sftp/RemoteFileManagerWidget.h
    src/sftp/SftpResumeLedger.h
    src/sftp/SftpService.cpp
    src/sftp/SftpService.h
    src/ssh/KnownHostsManager.h
    src/ssh/ShellShim.cpp
    src/ssh/ShellShim.h
    src/ssh/SshCommandParser.cpp
    src/ssh/SshCommandParser.h
    src/ssh/SshSessionNode.cpp
    src/ssh/SshSessionNode.h
    src/ssh/SshSessionStack.cpp
    src/ssh/SshSessionStack.h
    src/ssh/SshTunnelBroker.cpp
    src/ssh/SshTunnelBroker.h
    src/terminal/LocalShellWorker.cpp
    src/terminal/LocalShellWorker.h
    src/terminal/TerminalEchoFilter.cpp
    src/terminal/TerminalEchoFilter.h
    src/terminal/TerminalPane.cpp
    src/terminal/TerminalSearchBar.cpp
    src/terminal/TerminalSearchBar.h
    src/terminal/TerminalSearch.h
    src/terminal/TerminalPane.h
    src/terminal/TerminalView.cpp
    src/terminal/TerminalView.h
    src/terminal/VtermBridge.cpp
    src/terminal/VtermBridge.h
    src/terminal/TerminalWorkspace.cpp
    src/terminal/TerminalWorkspace.h
    src/terminal/TerminalWorkspaceGroup.cpp
    src/terminal/TerminalWorkspaceGroup.h
    src/ui/ConnectionDialog.cpp
    src/ui/ConnectionDialog.h
    src/ui/HostManagerWidget.cpp
    src/ui/HostManagerWidget.h
    src/ui/HostDistributionIcon.h
    src/ui/OpenSourceLicensesDialog.cpp
    src/ui/OpenSourceLicensesDialog.h
    src/ui/SettingsDialog.cpp
    src/ui/SettingsDialog.h
    src/ui/SettingsWidgets.cpp
    src/ui/SettingsWidgets.h
    src/ui/SessionTabBar.cpp
    src/ui/SessionTabBar.h
    src/ui/SessionTabStrip.cpp
    src/ui/SessionTabStrip.h
    src/ui/SessionTabDropOverlay.cpp
    src/ui/SessionTabDropOverlay.h
    src/ui/SshKeyManagerDialog.cpp
    src/ui/SshKeyManagerDialog.h
    src/ui/UiIcons.h
    src/ui/VaultDialog.cpp
    src/ui/VaultDialog.h
    src/update/UpdateChecker.cpp
    src/update/UpdateChecker.h
    src/update/UpdatePrompt.cpp
    src/update/UpdatePrompt.h
)

qt_add_executable(Shellium
    src/main.cpp
    ${SHELLIUM_APP_SOURCES}
)

# 将带透明圆角安全区的 512px 主图标编译进 Qt 资源系统，供 QApplication 在所有平台统一
# 设置窗口主图标。运行时不直接解码 1024px 母图，是因为窗口标题、任务切换器和消息框并不
# 需要这么高的单张分辨率；512px 足以覆盖高 DPI 显示，同时可把常驻 RGBA 缓冲从约 4 MiB
# 降到约 1 MiB。资源编译进可执行文件后也不依赖当前工作目录，开发与打包启动路径保持一致。
qt_add_resources(Shellium "shellium_brand_assets"
    PREFIX "/"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES
        # 下拉箭头必须使用真实矢量折线；QSS 的 CSS 边框三角写法不属于 Qt 的稳定能力，
        # 在 macOS/Fusion 组合下会退化成截图中的小方块。深浅主题分别提供同色系资源，
        # 由主题样式在运行时选择，保证两种背景上都有合适对比度。
        resources/icons/chevron-down-dark.svg
        resources/icons/chevron-down-light.svg
        resources/icons/shellium-app-icon-macos-512.png
)

# 主机侧栏图标：真实发行版官方标志的单色矢量版（源自 simple-icons 项目，CC0 授权，仅
# 保留可识别的品牌轮廓），运行时按 HostRecord.distributionId 选取并着色，见
# HostDistributionIcon.h。未收录的发行版继续退回该文件里手绘的通用 Linux 图标。
qt_add_resources(Shellium "shellium_distribution_icons"
    PREFIX "/"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES
        resources/icons/distros/almalinux.svg
        resources/icons/distros/alpinelinux.svg
        resources/icons/distros/archlinux.svg
        resources/icons/distros/artixlinux.svg
        resources/icons/distros/centos.svg
        resources/icons/distros/debian.svg
        resources/icons/distros/deepin.svg
        resources/icons/distros/devuan.svg
        resources/icons/distros/elementary.svg
        resources/icons/distros/endeavouros.svg
        resources/icons/distros/fedora.svg
        resources/icons/distros/gentoo.svg
        resources/icons/distros/kalilinux.svg
        resources/icons/distros/linuxmint.svg
        resources/icons/distros/manjaro.svg
        resources/icons/distros/mxlinux.svg
        resources/icons/distros/nixos.svg
        resources/icons/distros/opensuse.svg
        resources/icons/distros/photon.svg
        resources/icons/distros/popos.svg
        resources/icons/distros/raspberrypi.svg
        resources/icons/distros/redhat.svg
        resources/icons/distros/rockylinux.svg
        resources/icons/distros/slackware.svg
        resources/icons/distros/solus.svg
        resources/icons/distros/suse.svg
        resources/icons/distros/ubuntu.svg
        resources/icons/distros/voidlinux.svg
        resources/icons/distros/zorin.svg
)

shellium_add_bundled_fonts(Shellium)
shellium_add_license_notices(Shellium)

# 界面翻译：源语言为源码里的简体中文 tr() 字面量，无需 .ts。每新增一门语言就在 i18n/ 下
# 添加 shellium_<locale>.ts 并登记到此列表，同时同步 LocalizationService 的
# supportedLanguages()/availableLanguages()。构建时 lrelease 会把 .ts 编成 .qm 并注入
# Qt 资源前缀 /i18n，运行时由 LocalizationService 从 :/i18n/ 装载，因此打包不依赖外部文件。
#
# 开发者改动可见文案后运行：
#   cmake --build <build-dir> --target update_translations
# 即可用 lupdate 刷新 .ts；CI 可据其 git diff 是否为空判断有无未提取文案。
# 全部受支持语言的 .ts 清单，供正式程序与需要内嵌 .qm 的测试目标共用（新增语言只改这里
# 和 LocalizationService 的 supportedLanguages()/availableLanguages()）。
set(SHELLIUM_TS_FILES
    i18n/shellium_ar.ts
    i18n/shellium_de.ts
    i18n/shellium_es.ts
    i18n/shellium_fr.ts
    i18n/shellium_it.ts
    i18n/shellium_ja.ts
    i18n/shellium_ko.ts
    i18n/shellium_pt_BR.ts
    i18n/shellium_ro.ts
    i18n/shellium_ru.ts
    i18n/shellium_uk.ts
    i18n/shellium_zh_CN.ts
    i18n/shellium_zh_TW.ts
)

qt_add_translations(Shellium
    TS_FILES ${SHELLIUM_TS_FILES}
    RESOURCE_PREFIX "/i18n"
    NO_GENERATE_PLURALS_TS_FILE
    LUPDATE_OPTIONS -no-obsolete -locations none
)

target_include_directories(Shellium PRIVATE src)
# 设置页“关于”与侧边栏显示的版本号只认 project(VERSION)，避免源码里再手写一份后漏改。
target_compile_definitions(Shellium PRIVATE SHELLIUM_VERSION="${PROJECT_VERSION}")
target_link_libraries(Shellium PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Concurrent
    Qt6::Network
    Qt6::Sql
    Qt6::Svg
    ShelliumDependencies::libssh
    ShelliumDependencies::libvterm
    ShelliumDependencies::libsodium
    ShelliumDependencies::minizip
    ShelliumDependencies::spdlog
)

if(WIN32)
    # ConPTY、Winsock 与 Windows 安全描述符属于系统 SDK。编译定义避免 windows.h 宏污染
    # std::min/max，并确保所有 Win32 路径与进程 API 固定走 Unicode 宽字符版本。
    target_compile_definitions(Shellium PRIVATE
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
    target_link_libraries(Shellium PRIVATE Kernel32 Ws2_32 Advapi32 Userenv)
    set_target_properties(Shellium PROPERTIES WIN32_EXECUTABLE TRUE)

    if(MSVC)
        # Shellium.rc 已显式嵌入包含 DPI、长路径与执行权限声明的自定义 manifest。MSVC 链接器
        # 默认还会生成一份 ID 为 1 的 manifest，最终在 cvtres 阶段因资源重复报 CVT1100。
        # 目标级 /MANIFEST:NO 只禁止链接器自动生成清单，不影响 RC 中的自定义清单与版本资源；
        # 该选项限定在 MSVC，避免传给 MinGW 或 Linux 到 Windows 的交叉链接器。
        target_link_options(Shellium PRIVATE "/MANIFEST:NO")
    endif()

    # 将 asInvoker、Per-Monitor-V2 DPI、长路径和版本属性直接嵌入 PE。生成到构建目录而非
    # 修改源码文件，可让版本号始终跟随 project()，也避免多配置构建相互覆盖。
    enable_language(RC)
    configure_file(packaging/windows/Shellium.rc.in
                   "${CMAKE_CURRENT_BINARY_DIR}/Shellium.rc" @ONLY)
    target_sources(Shellium PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/Shellium.rc")
    # RC 引用的图标与清单不在 CMake 的依赖扫描范围内，显式声明后替换图标才会触发重新编译资源。
    set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/Shellium.rc" PROPERTIES
        OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/Shellium.ico;${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/Shellium.manifest")

    if(NOT CMAKE_CROSSCOMPILING)
        # 原生 Windows 构建使用 Qt 官方部署脚本收集 DLL 与插件。交叉构建刻意不走此分支，
        # 因为 Linux 主机无法运行目标平台 windeployqt，必须由发布镜像递归扫描 PE imports。
        # MSVC 下 windeployqt 的“编译器运行时”是把 vc_redist 安装程序整个拷进包里（二十多 MB，
        # 还要用户再装一遍）。改为由 scripts/bundle_windows_runtime.sh 把 vcruntime/msvcp 等 DLL
        # 以 app-local 方式放到 EXE 旁边。MinGW 仍让 windeployqt 复制 libgcc/libstdc++。
        set(shellium_deploy_runtime_option "")
        if(MSVC)
            set(shellium_deploy_runtime_option NO_COMPILER_RUNTIME)
        endif()
        qt_generate_deploy_app_script(
            TARGET Shellium
            OUTPUT_SCRIPT shellium_qt_deploy_script
            NO_UNSUPPORTED_PLATFORM_ERROR
            ${shellium_deploy_runtime_option}
        )
    endif()
endif()

if(UNIX AND NOT APPLE)
    # Linux 的 forkpty 位于独立 libutil；macOS 已由系统 C 库提供，无需额外链接。
    target_link_libraries(Shellium PRIVATE util)
endif()

if(APPLE)
    # 原生 Cocoa 标题栏桥接仅在 macOS 编译。Objective-C++ 文件只负责调整 NSWindow
    # 展示属性，业务界面仍保持跨平台 Qt 实现，Linux 构建不会引入 AppKit 依赖。
    enable_language(OBJCXX)
    target_sources(Shellium PRIVATE
        src/ui/MacWindowChrome.h
        src/ui/MacWindowChrome.mm
    )
    target_link_libraries(Shellium PRIVATE "-framework AppKit")

    # Finder、Dock 与 LaunchServices 从 App Bundle 的 Info.plist 和 Resources 读取 .icns，
    # 不会使用 Qt 的 qrc 路径。将平台图标作为 Bundle 资源加入目标，并只在属性中写文件名，
    # 可让 CMake 同时生成正确的 CFBundleIconFile 条目和最终
    # Resources/Shellium-macos.icns 文件，避免元数据与磁盘文件名发生偏差。
    set(SHELLIUM_MACOS_ICON
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/Shellium-macos.icns")
    set_source_files_properties("${SHELLIUM_MACOS_ICON}" PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
    )
    target_sources(Shellium PRIVATE "${SHELLIUM_MACOS_ICON}")

    # 生成具有稳定版本信息的标准 macOS App Bundle。签名身份和公证凭据属于发布者私密配置，
    # 因此不固化在 CMake 工程中，而是由 scripts/package_macos_dmg.sh 在部署阶段安全注入。
    set_target_properties(Shellium PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Shellium"
        MACOSX_BUNDLE_GUI_IDENTIFIER "${SHELLIUM_BUNDLE_IDENTIFIER}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_COPYRIGHT "Copyright © 2026 Shellium contributors"
        # 这里必须与 SHELLIUM_MACOS_ICON 的实际文件名完全一致；仅把新资源复制进 Bundle、
        # 却继续在 Info.plist 中引用旧名称，会让 Finder 与 Dock 回退到旧图标或通用应用图标。
        MACOSX_BUNDLE_ICON_FILE "Shellium-macos.icns"
    )
endif()

if(WIN32)
    # Windows staging 根目录直接放 EXE，Qt 插件目录与 qt.conf 可在部署阶段按标准结构加入。
    install(TARGETS Shellium RUNTIME DESTINATION .)
    if(DEFINED shellium_qt_deploy_script)
        # Qt 部署目录默认取 CMAKE_INSTALL_BINDIR（bin）与 Qt 自身的安装布局（MSYS2 下插件在
        # share/qt6/plugins），而 Shellium.exe 安装在根目录：Qt DLL 进了 bin/，加载器在 EXE 旁边
        # 找不到，程序无法启动。部署支持脚本只在变量未设置时才填默认值，这里先固定为扁平布局，
        # 与 packaging/windows/qt.conf 一致。
        install(CODE [[
            set(QT_DEPLOY_BIN_DIR ".")
            set(QT_DEPLOY_PLUGINS_DIR "plugins")
            set(QT_DEPLOY_QML_DIR "qml")
            set(QT_DEPLOY_TRANSLATIONS_DIR "translations")
        ]])
        # 部署脚本必须排在 install(TARGETS) 之后执行，否则 staging 中尚无主程序可供分析。
        install(SCRIPT "${shellium_qt_deploy_script}")
    endif()
else()
    install(TARGETS Shellium
        BUNDLE DESTINATION .
        RUNTIME DESTINATION bin
    )
endif()

if(WIN32)
    # CPack ZIP 只收集程序文件，不接触 QStandardPaths 下的数据库、保险箱、日志和用户 SSH
    # 配置；单用户 NSIS 安装器由独立脚本生成，以避开 CPack 模板固定的管理员执行级别。
    # CMake 自带 NSIS 模板固定请求管理员权限，不符合单用户安装约束；这里只保留 ZIP 生成器，
    # 正式安装器由 packaging/windows/Shellium.nsi 明确以 RequestExecutionLevel user 生成。
    set(CPACK_GENERATOR "ZIP")
    set(CPACK_PACKAGE_NAME "Shellium")
    set(CPACK_PACKAGE_VENDOR "Shellium contributors")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_FILE_NAME "Shellium-${PROJECT_VERSION}-win64")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "Shellium")
    set(CPACK_NSIS_INSTALL_ROOT "$LOCALAPPDATA\\Programs")
    set(CPACK_NSIS_MODIFY_PATH OFF)
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_DISPLAY_NAME "Shellium")
    set(CPACK_NSIS_PACKAGE_NAME "Shellium")
    set(CPACK_NSIS_EXECUTABLES_DIRECTORY ".")
    set(CPACK_NSIS_MUI_FINISHPAGE_RUN "Shellium.exe")
    include(CPack)
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Test Sql)
    # 独立验证应用级光标策略，避免依赖数据库、SSH 或用户配置即可发现按钮交互回归。
    # 独立验证全局滚动条策略，无需数据库、网络或真实终端会话。
    qt_add_executable(ShelliumScrollBarTests tests/ScrollBarPolicyTest.cpp)
    target_include_directories(ShelliumScrollBarTests PRIVATE src)
    target_link_libraries(ShelliumScrollBarTests PRIVATE Qt6::Widgets Qt6::Test)
    add_test(NAME ShelliumScrollBarTests COMMAND ShelliumScrollBarTests)

    qt_add_executable(ShelliumButtonCursorTests tests/ButtonCursorTest.cpp)
    target_include_directories(ShelliumButtonCursorTests PRIVATE src)
    target_link_libraries(ShelliumButtonCursorTests PRIVATE Qt6::Widgets Qt6::Test)
    add_test(NAME ShelliumButtonCursorTests COMMAND ShelliumButtonCursorTests)

    # 悬停提示的位置与字号由应用级策略接管，独立验证无需数据库或会话。
    qt_add_executable(ShelliumToolTipTests tests/ToolTipPolicyTest.cpp)
    target_include_directories(ShelliumToolTipTests PRIVATE src)
    target_link_libraries(ShelliumToolTipTests PRIVATE Qt6::Widgets Qt6::Test)
    add_test(NAME ShelliumToolTipTests COMMAND ShelliumToolTipTests)

    # 平台 socket 抽象单独测试，Windows 回环 TCP pair 与 POSIX socketpair 共用同一契约。
    # 该目标不引入 libssh，能把所有权或错误码映射故障定位到最小范围。
    qt_add_executable(ShelliumNativeSocketTests
        tests/NativeSocketTest.cpp
    )
    target_link_libraries(ShelliumNativeSocketTests PRIVATE
        Qt6::Core Qt6::Test ShelliumPlatform)
    add_test(NAME ShelliumNativeSocketTests COMMAND ShelliumNativeSocketTests)

    # 发布流水线脚本（Windows DLL 补齐、启动冒烟判定）只在 CI 发版时真正运行，这里用假导入表
    # 离线回归，改脚本后本地 ctest 即可发现问题。原生 Windows 没有 POSIX shell，跳过。
    if(NOT WIN32)
        find_program(SHELLIUM_BASH bash)
        if(SHELLIUM_BASH)
            add_test(NAME ShelliumPackagingScriptsTests
                COMMAND "${SHELLIUM_BASH}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/PackagingScriptsTest.sh")
        endif()
    endif()

    qt_add_executable(ShelliumCoreTests
        tests/SshCommandParserTest.cpp
        src/core/ConnectionTarget.cpp
        src/core/ConnectionTarget.h
        src/core/HostRecord.h
        src/security/Vault.cpp
        src/security/Vault.h
        src/settings/TerminalSettings.cpp
        src/settings/TerminalSettings.h
        src/storage/ApplicationDatabase.cpp
        src/storage/ApplicationDatabase.h
        src/ssh/ShellShim.cpp
        src/ssh/ShellShim.h
        src/ssh/SshCommandParser.cpp
        src/ssh/SshCommandParser.h
        src/terminal/TerminalEchoFilter.cpp
        src/terminal/TerminalEchoFilter.h
        src/terminal/TerminalView.cpp
        src/terminal/TerminalView.h
        src/terminal/VtermBridge.cpp
        src/terminal/VtermBridge.h
        src/terminal/LocalShellWorker.cpp
        src/terminal/LocalShellWorker.h
        src/ui/SettingsWidgets.cpp
        src/ui/SettingsWidgets.h
        src/ui/VaultDialog.cpp
        src/ui/VaultDialog.h
    )
    target_include_directories(ShelliumCoreTests PRIVATE src)
    target_link_libraries(ShelliumCoreTests PRIVATE
        Qt6::Core
        Qt6::Widgets
        Qt6::Sql
        Qt6::Test
        ShelliumDependencies::libssh
        ShelliumDependencies::libvterm
        ShelliumDependencies::libsodium
    )
    if(UNIX AND NOT APPLE)
        target_link_libraries(ShelliumCoreTests PRIVATE util)
    endif()

    add_test(NAME ShelliumCoreTests COMMAND ShelliumCoreTests)

    # 持久化层独立成一个测试可执行文件：QTEST_MAIN 会展开出 main，同一目标里放两个测试类
    # 必然重复定义 main。该目标只依赖 Core/Sql/Test，不需要 GUI、libssh 与 libvterm。
    qt_add_executable(ShelliumStorageTests
        tests/ApplicationDatabaseTest.cpp
        src/core/ConnectionTarget.cpp
        src/core/ConnectionTarget.h
        src/core/HostRecord.h
        src/storage/ApplicationDatabase.cpp
        src/storage/ApplicationDatabase.h
    )
    target_include_directories(ShelliumStorageTests PRIVATE src)
    target_link_libraries(ShelliumStorageTests PRIVATE
        Qt6::Core
        Qt6::Sql
        Qt6::Test
    )

    add_test(NAME ShelliumStorageTests COMMAND ShelliumStorageTests)

    # 多语言基础设施测试：语言码解析、翻译资源装载与设置字段规范化。只依赖 Core/Gui/Sql，
    # 不引入 libssh/libvterm。翻译目录由下方 qt_add_translations 编进本目标自己的 Qt 资源，
    # 测试才能独立断言 :/i18n/shellium_en.qm 真的被装载并生效。
    qt_add_executable(ShelliumLocalizationTests
        tests/LocalizationTest.cpp
        src/i18n/LocalizationService.cpp
        src/i18n/LocalizationService.h
        src/core/ConnectionTarget.cpp
        src/core/ConnectionTarget.h
        src/core/HostRecord.h
        src/settings/TerminalSettings.cpp
        src/settings/TerminalSettings.h
        src/storage/ApplicationDatabase.cpp
        src/storage/ApplicationDatabase.h
    )
    target_include_directories(ShelliumLocalizationTests PRIVATE src)
    target_link_libraries(ShelliumLocalizationTests PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Sql
        Qt6::Test
    )
    qt_add_translations(ShelliumLocalizationTests
        TS_FILES ${SHELLIUM_TS_FILES}
        RESOURCE_PREFIX "/i18n"
        NO_GENERATE_PLURALS_TS_FILE
        LUPDATE_TARGET ShelliumLocalizationTests_lupdate
        LRELEASE_TARGET ShelliumLocalizationTests_lrelease
        LUPDATE_OPTIONS -no-obsolete -locations none
    )
    add_test(NAME ShelliumLocalizationTests COMMAND ShelliumLocalizationTests)

    # 连接管理工具单独测试：密钥生成依赖 libssh，配置持久化依赖 SQLite，但不需要 GUI。
    # 独立目标避免给核心解析测试继续叠加职责，并可单独复现文件权限或格式兼容问题。
    qt_add_executable(ShelliumConnectionToolsTests
        tests/ConnectionToolsTest.cpp
        src/core/ConnectionConfigCodec.cpp
        src/core/ConnectionConfigCodec.h
        src/core/ConnectionTarget.cpp
        src/core/ConnectionTarget.h
        src/core/HostRecord.h
        src/forwarding/PortForwardRule.h
        src/forwarding/Socks5Protocol.cpp
        src/forwarding/Socks5Protocol.h
        src/security/SshKeyManager.cpp
        src/security/SshKeyManager.h
        src/storage/ApplicationDatabase.cpp
        src/storage/ApplicationDatabase.h
    )
    target_include_directories(ShelliumConnectionToolsTests PRIVATE src)
    target_link_libraries(ShelliumConnectionToolsTests PRIVATE
        Qt6::Core Qt6::Network Qt6::Sql Qt6::Test ShelliumDependencies::libssh)
    add_test(NAME ShelliumConnectionToolsTests COMMAND ShelliumConnectionToolsTests)

    # 效率工具测试覆盖 Snippet 树、历史补全、多目标信号以及 TerminalPane 到数据库的真实
    # 接线。由于 Pane 同时引用完整 SSH 栈，这里显式列出相关实现；测试本身不连接网络。
    qt_add_executable(ShelliumProductivityTests
        tests/ProductivityTest.cpp
        src/core/ConnectionTarget.cpp
        src/core/ConnectionTarget.h
        src/core/HostRecord.h
        src/productivity/ProductivityModel.cpp
        src/productivity/ProductivityModel.h
        src/productivity/ProductivityWidget.cpp
        src/productivity/ProductivityWidget.h
        src/productivity/QuickCommandPopup.cpp
        src/productivity/QuickCommandPopup.h
        src/settings/TerminalSettings.cpp
        src/settings/TerminalSettings.h
        src/storage/ApplicationDatabase.cpp
        src/storage/ApplicationDatabase.h
        src/ssh/ShellShim.cpp
        src/ssh/ShellShim.h
        src/ssh/SshCommandParser.cpp
        src/ssh/SshCommandParser.h
        src/ssh/SshSessionNode.cpp
        src/ssh/SshSessionNode.h
        src/ssh/SshSessionStack.cpp
        src/ssh/SshSessionStack.h
        src/ssh/SshTunnelBroker.cpp
        src/ssh/SshTunnelBroker.h
        src/terminal/LocalShellWorker.cpp
        src/terminal/LocalShellWorker.h
        src/terminal/TerminalEchoFilter.cpp
        src/terminal/TerminalEchoFilter.h
        src/terminal/TerminalPane.cpp
        src/terminal/TerminalSearchBar.cpp
        src/terminal/TerminalSearchBar.h
        src/terminal/TerminalSearch.h
        src/terminal/TerminalPane.h
        src/terminal/TerminalView.cpp
        src/terminal/TerminalView.h
        src/terminal/VtermBridge.cpp
        src/terminal/VtermBridge.h
        src/ui/SettingsWidgets.cpp
        src/ui/SettingsWidgets.h
    )
    target_include_directories(ShelliumProductivityTests PRIVATE src)
    target_link_libraries(ShelliumProductivityTests PRIVATE
        Qt6::Core
        Qt6::Concurrent
        Qt6::Sql
        Qt6::Widgets
        Qt6::Test
        ShelliumDependencies::libssh
        ShelliumDependencies::libvterm
    )
    if(UNIX AND NOT APPLE)
        target_link_libraries(ShelliumProductivityTests PRIVATE util)
    endif()
    add_test(NAME ShelliumProductivityTests COMMAND ShelliumProductivityTests)

    # 端口转发侧栏改成卡片式列表后，选中态读取、双击、右键菜单全部换成了 QListWidget
    # 的 item API；这里之前完全没有界面测试覆盖，新目标只需要数据库和 SSH 栈骨架，
    # 不需要真正建立网络连接。
    qt_add_executable(ShelliumPortForwardTests
        tests/PortForwardManagerWidgetTest.cpp
        src/core/ConnectionTarget.cpp
        src/core/ConnectionTarget.h
        src/core/HostRecord.h
        src/forwarding/PortForwardManagerWidget.cpp
        src/forwarding/PortForwardManagerWidget.h
        src/forwarding/PortForwardRule.h
        src/forwarding/PortForwardRuntime.cpp
        src/forwarding/PortForwardRuntime.h
        src/forwarding/Socks5Protocol.cpp
        src/forwarding/Socks5Protocol.h
        src/ssh/ShellShim.cpp
        src/ssh/ShellShim.h
        src/ssh/SshSessionNode.cpp
        src/ssh/SshSessionNode.h
        src/ssh/SshSessionStack.cpp
        src/ssh/SshSessionStack.h
        src/storage/ApplicationDatabase.cpp
        src/storage/ApplicationDatabase.h
        src/ui/UiIcons.h
    )
    target_include_directories(ShelliumPortForwardTests PRIVATE src)
    target_link_libraries(ShelliumPortForwardTests PRIVATE
        Qt6::Core Qt6::Gui Qt6::Network Qt6::Widgets Qt6::Sql Qt6::Test
        ShelliumDependencies::libssh)
    add_test(NAME ShelliumPortForwardTests COMMAND ShelliumPortForwardTests)

    # 标题栏标签控件测试需要真实的 QTabBar 布局与窗口曝光，因此依赖 Qt6::Widgets 和
    # 平台 GUI；macOS 上还需要 Objective-C++ 的原生标题栏桥接。
    # 主窗口外壳回归：启动空态、"+"新建终端、"+"跟随最后一个标签，以及断线提示按 x 关闭
    # 标签页这条跨 TerminalPane→TerminalWorkspace→MainWindow 的完整路径。目标复用正式程序
    # 的源文件清单，因此测试到的就是用户实际启动的那个窗口。
    qt_add_executable(ShelliumMainWindowShellTests
        tests/MainWindowShellTest.cpp
        ${SHELLIUM_APP_SOURCES}
    )
    qt_add_resources(ShelliumMainWindowShellTests "shellium_brand_assets_shell_tests"
        PREFIX "/"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES
            resources/icons/chevron-down-dark.svg
            resources/icons/chevron-down-light.svg
            resources/icons/shellium-app-icon-macos-512.png
    )
    qt_add_resources(ShelliumMainWindowShellTests "shellium_distribution_icons_shell_tests"
        PREFIX "/"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES
            resources/icons/distros/almalinux.svg
            resources/icons/distros/alpinelinux.svg
            resources/icons/distros/archlinux.svg
            resources/icons/distros/artixlinux.svg
            resources/icons/distros/centos.svg
            resources/icons/distros/debian.svg
            resources/icons/distros/deepin.svg
            resources/icons/distros/devuan.svg
            resources/icons/distros/elementary.svg
            resources/icons/distros/endeavouros.svg
            resources/icons/distros/fedora.svg
            resources/icons/distros/gentoo.svg
            resources/icons/distros/kalilinux.svg
            resources/icons/distros/linuxmint.svg
            resources/icons/distros/manjaro.svg
            resources/icons/distros/mxlinux.svg
            resources/icons/distros/nixos.svg
            resources/icons/distros/opensuse.svg
            resources/icons/distros/photon.svg
            resources/icons/distros/popos.svg
            resources/icons/distros/raspberrypi.svg
            resources/icons/distros/redhat.svg
            resources/icons/distros/rockylinux.svg
            resources/icons/distros/slackware.svg
            resources/icons/distros/solus.svg
            resources/icons/distros/suse.svg
            resources/icons/distros/ubuntu.svg
            resources/icons/distros/voidlinux.svg
            resources/icons/distros/zorin.svg
    )
    shellium_add_bundled_fonts(ShelliumMainWindowShellTests)
    shellium_add_license_notices(ShelliumMainWindowShellTests)
    target_include_directories(ShelliumMainWindowShellTests PRIVATE src)
    target_link_libraries(ShelliumMainWindowShellTests PRIVATE
        Qt6::Core
        Qt6::Concurrent
        Qt6::Gui
        Qt6::Network
        Qt6::Sql
        Qt6::Svg
        Qt6::Widgets
        Qt6::Test
        ShelliumDependencies::libssh
        ShelliumDependencies::libvterm
        ShelliumDependencies::libsodium
        ShelliumDependencies::minizip
        ShelliumDependencies::spdlog
    )
    if(UNIX AND NOT APPLE)
        target_link_libraries(ShelliumMainWindowShellTests PRIVATE util)
    endif()
    if(APPLE)
        target_sources(ShelliumMainWindowShellTests PRIVATE
            src/ui/MacWindowChrome.h
            src/ui/MacWindowChrome.mm
        )
        target_link_libraries(ShelliumMainWindowShellTests PRIVATE "-framework AppKit")
    endif()
    # 连接提示回归逐语言验证真实译文，因此测试资源必须与正式程序包含相同的语言集合。
    qt_add_translations(ShelliumMainWindowShellTests
        TS_FILES ${SHELLIUM_TS_FILES}
        RESOURCE_PREFIX "/i18n"
        NO_GENERATE_PLURALS_TS_FILE
        LUPDATE_TARGET ShelliumMainWindowShellTests_lupdate
        LRELEASE_TARGET ShelliumMainWindowShellTests_lrelease
        LUPDATE_OPTIONS -no-obsolete -locations none
    )
    add_test(NAME ShelliumMainWindowShellTests COMMAND ShelliumMainWindowShellTests)

    # SSH 异常断线后的手动恢复提示只涉及终端本地状态，因此单独成为一个轻量目标，用例
    # 全程不发起网络连接。
    qt_add_executable(ShelliumSessionRecoveryTests
        tests/SessionRecoveryTest.cpp
        src/core/ConnectionTarget.cpp
        src/core/ConnectionTarget.h
        src/core/HostRecord.h
        src/storage/ApplicationDatabase.cpp
        src/storage/ApplicationDatabase.h
        src/sftp/SftpService.cpp
        src/sftp/SftpService.h
        src/ssh/ShellShim.cpp
        src/ssh/ShellShim.h
        src/ssh/SshCommandParser.cpp
        src/ssh/SshCommandParser.h
        src/ssh/SshSessionNode.cpp
        src/ssh/SshSessionNode.h
        src/ssh/SshSessionStack.cpp
        src/ssh/SshSessionStack.h
        src/ssh/SshTunnelBroker.cpp
        src/ssh/SshTunnelBroker.h
        src/terminal/LocalShellWorker.cpp
        src/terminal/LocalShellWorker.h
        src/terminal/TerminalEchoFilter.cpp
        src/terminal/TerminalEchoFilter.h
        src/terminal/TerminalPane.cpp
        src/terminal/TerminalSearchBar.cpp
        src/terminal/TerminalSearchBar.h
        src/terminal/TerminalSearch.h
        src/terminal/TerminalPane.h
        src/terminal/TerminalView.cpp
        src/terminal/TerminalView.h
        src/terminal/VtermBridge.cpp
        src/terminal/VtermBridge.h
        src/settings/TerminalSettings.cpp
        src/settings/TerminalSettings.h
        src/ui/SettingsWidgets.cpp
        src/ui/SettingsWidgets.h
    )
    target_include_directories(ShelliumSessionRecoveryTests PRIVATE src)
    target_link_libraries(ShelliumSessionRecoveryTests PRIVATE
        Qt6::Core
        Qt6::Concurrent
        Qt6::Gui
        Qt6::Sql
        Qt6::Widgets
        Qt6::Test
        ShelliumDependencies::libssh
        ShelliumDependencies::libvterm
    )
    if(UNIX AND NOT APPLE)
        target_link_libraries(ShelliumSessionRecoveryTests PRIVATE util)
    endif()
    add_test(NAME ShelliumSessionRecoveryTests COMMAND ShelliumSessionRecoveryTests)

    qt_add_executable(ShelliumSessionTabBarTests
        tests/SessionTabBarTest.cpp
        src/ui/SessionTabBar.cpp
        src/ui/SessionTabBar.h
        src/ui/SessionTabStrip.cpp
        src/ui/SessionTabStrip.h
        src/ui/SessionTabDropOverlay.cpp
        src/ui/SessionTabDropOverlay.h
    )
    target_include_directories(ShelliumSessionTabBarTests PRIVATE src)
    target_link_libraries(ShelliumSessionTabBarTests PRIVATE
        Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
    if(APPLE)
        target_sources(ShelliumSessionTabBarTests PRIVATE
            src/ui/MacWindowChrome.h
            src/ui/MacWindowChrome.mm
        )
        target_link_libraries(ShelliumSessionTabBarTests PRIVATE "-framework AppKit")
    endif()
    add_test(NAME ShelliumSessionTabBarTests COMMAND ShelliumSessionTabBarTests)

    # 主机侧栏和配置对话框的界面回归。对话框内的“测试连接”复用正式 SSH 状态机，因此测试
    # 目标也必须链接相关实现；界面用例本身不会点击测试按钮，不会产生任何网络访问。
    qt_add_executable(ShelliumHostManagerTests
        tests/HostManagerWidgetTest.cpp
        src/core/ExcelHostCodec.cpp
        src/core/ExcelHostCodec.h
        src/core/ConnectionConfigCodec.cpp
        src/core/ConnectionConfigCodec.h
        src/core/ConnectionTarget.cpp
        src/core/ConnectionTarget.h
        src/core/HostRecord.h
        src/security/Vault.cpp
        src/security/Vault.h
        src/ssh/ShellShim.cpp
        src/ssh/ShellShim.h
        src/ssh/SshSessionNode.cpp
        src/ssh/SshSessionNode.h
        src/ssh/SshSessionStack.cpp
        src/ssh/SshSessionStack.h
        src/storage/ApplicationDatabase.cpp
        src/storage/ApplicationDatabase.h
        src/ui/HostManagerWidget.cpp
        src/ui/HostManagerWidget.h
        src/ui/HostDistributionIcon.h
        src/ui/SettingsWidgets.cpp
        src/ui/SettingsWidgets.h
        src/ui/UiIcons.h
        src/ui/VaultDialog.cpp
        src/ui/VaultDialog.h
    )
    qt_add_resources(ShelliumHostManagerTests "shellium_distribution_icons_host_manager_tests"
        PREFIX "/"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES
            resources/icons/distros/almalinux.svg
            resources/icons/distros/alpinelinux.svg
            resources/icons/distros/archlinux.svg
            resources/icons/distros/artixlinux.svg
            resources/icons/distros/centos.svg
            resources/icons/distros/debian.svg
            resources/icons/distros/deepin.svg
            resources/icons/distros/devuan.svg
            resources/icons/distros/elementary.svg
            resources/icons/distros/endeavouros.svg
            resources/icons/distros/fedora.svg
            resources/icons/distros/gentoo.svg
            resources/icons/distros/kalilinux.svg
            resources/icons/distros/linuxmint.svg
            resources/icons/distros/manjaro.svg
            resources/icons/distros/mxlinux.svg
            resources/icons/distros/nixos.svg
            resources/icons/distros/opensuse.svg
            resources/icons/distros/photon.svg
            resources/icons/distros/popos.svg
            resources/icons/distros/raspberrypi.svg
            resources/icons/distros/redhat.svg
            resources/icons/distros/rockylinux.svg
            resources/icons/distros/slackware.svg
            resources/icons/distros/solus.svg
            resources/icons/distros/suse.svg
            resources/icons/distros/ubuntu.svg
            resources/icons/distros/voidlinux.svg
            resources/icons/distros/zorin.svg
    )
    target_include_directories(ShelliumHostManagerTests PRIVATE src)
    target_link_libraries(ShelliumHostManagerTests PRIVATE
        Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Sql Qt6::Svg Qt6::Test
        ShelliumDependencies::libssh ShelliumDependencies::libsodium
        ShelliumDependencies::minizip)
    add_test(NAME ShelliumHostManagerTests COMMAND ShelliumHostManagerTests)

    # 设置界面改版后的控件接线回归：步进器、滑动开关、配色色板与分类导航都是自绘控件，
    # 只有真实构树才能验证“操作控件 → editedSettings() → SQLite”这条链路没有断。需要
    # Widgets 与 Sql，不涉及 libssh/libvterm 与网络。设置页现在还会在开启保险箱密码时调用
    # 正式保险库流程，因此目标需链接 Vault、VaultDialog 与 libsodium，避免测试使用假实现。
    qt_add_executable(ShelliumSettingsUiTests
        tests/SettingsUiTest.cpp
        src/ai/AiClient.cpp
        src/ai/AiClient.h
        src/ai/LocalAgentCli.cpp
        src/ai/LocalAgentCli.h
        src/ai/AiSettings.h
        src/ai/AiSettingsService.cpp
        src/ai/AiSettingsService.h
        src/core/ConnectionTarget.cpp
        src/core/ConnectionTarget.h
        src/core/HostRecord.h
        src/i18n/LocalizationService.cpp
        src/i18n/LocalizationService.h
        src/security/Vault.cpp
        src/security/Vault.h
        src/settings/BundledFonts.cpp
        src/settings/BundledFonts.h
        src/settings/TerminalSettings.cpp
        src/settings/TerminalSettings.h
        src/storage/ApplicationDatabase.cpp
        src/storage/ApplicationDatabase.h
        src/ui/ConnectionDialog.cpp
        src/ui/ConnectionDialog.h
        src/ui/OpenSourceLicensesDialog.cpp
        src/ui/OpenSourceLicensesDialog.h
        src/ui/SettingsDialog.cpp
        src/ui/SettingsDialog.h
        src/ui/SettingsWidgets.cpp
        src/ui/SettingsWidgets.h
        src/ui/VaultDialog.cpp
        src/ui/VaultDialog.h
        src/update/UpdateChecker.cpp
        src/update/UpdateChecker.h
        src/update/UpdatePrompt.cpp
        src/update/UpdatePrompt.h
    )
    shellium_add_bundled_fonts(ShelliumSettingsUiTests)
    shellium_add_license_notices(ShelliumSettingsUiTests)
    target_include_directories(ShelliumSettingsUiTests PRIVATE src)
    target_link_libraries(ShelliumSettingsUiTests PRIVATE
        Qt6::Core Qt6::Gui Qt6::Network Qt6::Widgets Qt6::Sql Qt6::Test Qt6::Concurrent
        ShelliumDependencies::libsodium)
    qt_add_translations(ShelliumSettingsUiTests
        TS_FILES i18n/shellium_zh_CN.ts
        RESOURCE_PREFIX "/i18n"
        NO_GENERATE_PLURALS_TS_FILE
        LUPDATE_TARGET ShelliumSettingsUiTests_lupdate
        LRELEASE_TARGET ShelliumSettingsUiTests_lrelease
        LUPDATE_OPTIONS -no-obsolete -locations none)
    add_test(NAME ShelliumSettingsUiTests COMMAND ShelliumSettingsUiTests)

    # 检查更新：版本比较、跳过规则与清单解析是纯逻辑，网络路径用 file:// 清单驱动真实的
    # QNetworkAccessManager，全程不访问外网，也不依赖本机端口（沙箱里连接被拒会长时间挂起）。
    qt_add_executable(ShelliumUpdateTests
        tests/UpdateCheckerTest.cpp
        src/update/UpdateChecker.cpp
        src/update/UpdateChecker.h
    )
    target_include_directories(ShelliumUpdateTests PRIVATE src)
    target_link_libraries(ShelliumUpdateTests PRIVATE Qt6::Core Qt6::Network Qt6::Test)
    add_test(NAME ShelliumUpdateTests COMMAND ShelliumUpdateTests)

    # AI 助手改版成单步交互回路后，"执行完命令自动读取真实输出并续问模型"这条链路完全靠
    # 轮询终端快照实现，没有任何一处是纯逻辑、可以脱离真实控件测试的。用手写的极简 HTTP
    # 服务器顶替真实网关，覆盖这条端到端路径。
    qt_add_executable(ShelliumAiAssistantTests
        tests/AiAssistantWidgetTest.cpp
        src/ai/AiAssistantWidget.cpp
        src/ai/AiAssistantWidget.h
        src/ai/AiClient.cpp
        src/ai/AiClient.h
        src/ai/AiChatTypes.h
        src/ai/AiSettings.h
        src/ai/AiSettingsService.cpp
        src/ai/AiSettingsService.h
        src/ai/CommandRiskClassifier.cpp
        src/ai/CommandRiskClassifier.h
        src/ai/LocalAgentCli.cpp
        src/ai/LocalAgentCli.h
        src/ai/LocalCliClient.cpp
        src/ai/LocalCliClient.h
        src/core/ConnectionTarget.cpp
        src/core/ConnectionTarget.h
        src/core/HostRecord.h
        src/i18n/LocalizationService.cpp
        src/i18n/LocalizationService.h
        src/i18n/Retranslate.h
        src/security/Vault.cpp
        src/security/Vault.h
        src/storage/ApplicationDatabase.cpp
        src/storage/ApplicationDatabase.h
    )
    target_include_directories(ShelliumAiAssistantTests PRIVATE src)
    target_link_libraries(ShelliumAiAssistantTests PRIVATE
        Qt6::Core Qt6::Gui Qt6::Network Qt6::Widgets Qt6::Sql Qt6::Test Qt6::Concurrent
        ShelliumDependencies::libsodium)
    qt_add_translations(ShelliumAiAssistantTests
        TS_FILES i18n/shellium_zh_CN.ts
        RESOURCE_PREFIX "/i18n"
        NO_GENERATE_PLURALS_TS_FILE
        LUPDATE_TARGET ShelliumAiAssistantTests_lupdate
        LRELEASE_TARGET ShelliumAiAssistantTests_lrelease
        LUPDATE_OPTIONS -no-obsolete -locations none)
    add_test(NAME ShelliumAiAssistantTests COMMAND ShelliumAiAssistantTests)

    # 运行日志是界面提示条移除后唯一的消息去路：写入一旦静默失败，用户既看不到提示也查
    # 不到记录。只依赖 Core/Test，单独成目标同样是为了避开重复 main。
    qt_add_executable(ShelliumActivityLogTests
        tests/ActivityLogTest.cpp
        src/core/ActivityLog.cpp
        src/core/ActivityLog.h
    )
    target_include_directories(ShelliumActivityLogTests PRIVATE src)
    target_link_libraries(ShelliumActivityLogTests PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME ShelliumActivityLogTests COMMAND ShelliumActivityLogTests)

    # 保活判定同样是内联纯逻辑，不需要 libssh 会话即可覆盖。独立成目标而不是并进
    # ShelliumCoreTests：一个可执行文件只能有一个 main，共用 main 还会让按名过滤测试时
    # 另一套报 "Unknown test function" 并返回非零，破坏单独复现某个用例的工作流。
    qt_add_executable(ShelliumKeepAliveTests tests/KeepAliveTest.cpp)
    target_include_directories(ShelliumKeepAliveTests PRIVATE src)
    target_link_libraries(ShelliumKeepAliveTests PRIVATE Qt6::Core Qt6::Test ShelliumDependencies::libssh)
    add_test(NAME ShelliumKeepAliveTests COMMAND ShelliumKeepAliveTests)

    # exec 中继打开 session 通道失败时的诊断文案同样是内联纯逻辑：MaxSessions 用尽时 OpenSSH
    # 只回 "error (2) open failed"，提示是否追加只取决于错误字符串。
    qt_add_executable(ShelliumRelayDiagnosticsTests tests/RelayDiagnosticsTest.cpp)
    target_include_directories(ShelliumRelayDiagnosticsTests PRIVATE src)
    target_link_libraries(ShelliumRelayDiagnosticsTests PRIVATE Qt6::Core Qt6::Test ShelliumDependencies::libssh)
    add_test(NAME ShelliumRelayDiagnosticsTests COMMAND ShelliumRelayDiagnosticsTests)

    # SFTP 递归传输的纯逻辑（路径越界防护、遍历顺序、跳过判定、失败聚合、进度计算）全部以
    # 内联形式放在 SftpService.h，因此这里不需要 libssh 会话、隧道或 SFTP 服务器就能覆盖。
    # 单独成一个目标是因为 ShelliumCoreTests 已经有一个 QTEST_MAIN，同一个可执行文件里不能
    # 再有第二个 main。
    qt_add_executable(ShelliumSftpRecursiveTests tests/SftpRecursiveTest.cpp)
    target_include_directories(ShelliumSftpRecursiveTests PRIVATE src)
    target_link_libraries(ShelliumSftpRecursiveTests PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME ShelliumSftpRecursiveTests COMMAND ShelliumSftpRecursiveTests)

    # "该不该传、能不能续、可不可以跳过"的判定层。判错的后果是最恶劣的一类：文件没更新却
    # 报告成功，或两个不同文件被拼成一个损坏文件，两者都不抛错。判定与续传账本都不依赖
    # libssh 与网络，因此这里能把每条边界都钉死。
    qt_add_executable(ShelliumSftpResumeTests tests/SftpResumeDecisionTest.cpp)
    target_include_directories(ShelliumSftpResumeTests PRIVATE src)
    target_link_libraries(ShelliumSftpResumeTests PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME ShelliumSftpResumeTests COMMAND ShelliumSftpResumeTests)

    # 远程文件管理改版不仅调整颜色，还改变默认单栏/全屏双栏、列结构、工具条顺序和窄宽度
    # 降级策略。这些契约必须通过真实 QWidget 构树验证。测试目标链接正式 SftpService 以确保
    # 控件与生产代码一致，但在测试文件中提供不会访问网络的 SshTunnelBroker 替身，因此运行
    # 测试不要求 SSH 服务器，也不会复用用户当前终端会话。
    qt_add_executable(ShelliumSftpUiTests
        tests/RemoteFileManagerWidgetTest.cpp
        src/core/ConnectionTarget.cpp
        src/core/ConnectionTarget.h
        src/core/HostRecord.h
        src/sftp/RemoteFileManagerWidget.cpp
        src/sftp/RemoteFileManagerWidget.h
        src/sftp/SftpService.cpp
        src/sftp/SftpService.h
        src/ssh/SshTunnelBroker.h
        src/ui/UiIcons.h
    )
    target_include_directories(ShelliumSftpUiTests PRIVATE src)
    target_link_libraries(ShelliumSftpUiTests PRIVATE
        Qt6::Core Qt6::Concurrent Qt6::Gui Qt6::Widgets Qt6::Test ShelliumDependencies::libssh)
    add_test(NAME ShelliumSftpUiTests COMMAND ShelliumSftpUiTests)

    # 备用屏（vim/top/less 等全屏程序）下的渲染与按键编码回归。只需要 libvterm 与
    # Widgets，不涉及 SSH 会话；单独成目标同样是为了避开重复 main。
    qt_add_executable(ShelliumTerminalAltScreenTests
        tests/TerminalAltScreenTest.cpp
        src/core/ConnectionTarget.cpp
        src/core/ConnectionTarget.h
        src/core/HostRecord.h
        src/settings/TerminalSettings.cpp
        src/settings/TerminalSettings.h
        src/storage/ApplicationDatabase.cpp
        src/storage/ApplicationDatabase.h
        src/terminal/TerminalView.cpp
        src/terminal/TerminalView.h
        src/terminal/VtermBridge.cpp
        src/terminal/VtermBridge.h
    )
    target_include_directories(ShelliumTerminalAltScreenTests PRIVATE src)
    target_link_libraries(ShelliumTerminalAltScreenTests PRIVATE
        Qt6::Core Qt6::Gui Qt6::Sql Qt6::Widgets Qt6::Test ShelliumDependencies::libvterm)
    add_test(NAME ShelliumTerminalAltScreenTests COMMAND ShelliumTerminalAltScreenTests)

    # `cat 二进制文件` 这类输出洪流的回归：libvterm 回流缩放的进程 abort、BEL 风暴限流、
    # 以及 PTY 读取端的背压。背压那条要 fork 一个真实的本地 Shell，因此不能并进只依赖
    # libvterm 的 AltScreen 目标；单独成目标同样是为了避开重复 main。
    qt_add_executable(ShelliumTerminalFloodTests
        tests/TerminalFloodTest.cpp
        src/terminal/LocalShellWorker.cpp
        src/terminal/LocalShellWorker.h
        src/terminal/VtermBridge.cpp
        src/terminal/VtermBridge.h
    )
    target_include_directories(ShelliumTerminalFloodTests PRIVATE src)
    target_link_libraries(ShelliumTerminalFloodTests PRIVATE
        Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test ShelliumDependencies::libvterm)
    add_test(NAME ShelliumTerminalFloodTests COMMAND ShelliumTerminalFloodTests)

    # 多标签内存占用的回归：紧凑滚动历史、排版缓存释放、后台标签暂停同步，以及这些优化
    # 不得改变渲染结果、选区与历史。需要真实 TerminalView，因此与 AltScreen 目标同一组源文件。
    qt_add_executable(ShelliumTerminalMemoryTests
        tests/TerminalMemoryTest.cpp
        src/core/ConnectionTarget.cpp
        src/core/ConnectionTarget.h
        src/core/HostRecord.h
        src/settings/TerminalSettings.cpp
        src/settings/TerminalSettings.h
        src/storage/ApplicationDatabase.cpp
        src/storage/ApplicationDatabase.h
        src/terminal/TerminalView.cpp
        src/terminal/TerminalView.h
        src/terminal/VtermBridge.cpp
        src/terminal/VtermBridge.h
    )
    target_include_directories(ShelliumTerminalMemoryTests PRIVATE src)
    target_link_libraries(ShelliumTerminalMemoryTests PRIVATE
        Qt6::Core Qt6::Gui Qt6::Sql Qt6::Widgets Qt6::Test ShelliumDependencies::libvterm)
    add_test(NAME ShelliumTerminalMemoryTests COMMAND ShelliumTerminalMemoryTests)

    # 针对真实服务器的连接路径回归。非阻塞握手状态机的价值就在于与真实 libssh 和真实
    # 服务器的时序交互，无法用桩替身覆盖；缺少 SHELLIUM_IT_* 环境变量时测试自行跳过。
    qt_add_executable(ShelliumSshIntegrationTests
        tests/SshSessionStackIntegrationTest.cpp
        src/core/ConnectionTarget.cpp
        src/core/ConnectionTarget.h
        src/core/HostRecord.h
        src/storage/ApplicationDatabase.cpp
        src/storage/ApplicationDatabase.h
        src/sftp/SftpService.cpp
        src/sftp/SftpService.h
        src/ssh/ShellShim.cpp
        src/ssh/ShellShim.h
        src/ssh/SshSessionNode.cpp
        src/ssh/SshSessionNode.h
        src/ssh/SshSessionStack.cpp
        src/ssh/SshSessionStack.h
        src/ssh/SshTunnelBroker.cpp
        src/ssh/SshTunnelBroker.h
        src/ssh/SshCommandParser.cpp
        src/ssh/SshCommandParser.h
        src/terminal/TerminalEchoFilter.cpp
        src/terminal/TerminalEchoFilter.h
        src/terminal/TerminalPane.cpp
        src/terminal/TerminalSearchBar.cpp
        src/terminal/TerminalSearchBar.h
        src/terminal/TerminalSearch.h
        src/terminal/TerminalPane.h
        src/terminal/TerminalView.cpp
        src/terminal/TerminalView.h
        src/terminal/VtermBridge.cpp
        src/terminal/VtermBridge.h
        src/terminal/LocalShellWorker.cpp
        src/terminal/LocalShellWorker.h
        src/settings/TerminalSettings.cpp
        src/settings/TerminalSettings.h
        src/ui/SettingsWidgets.cpp
        src/ui/SettingsWidgets.h
    )
    target_include_directories(ShelliumSshIntegrationTests PRIVATE src)
    target_link_libraries(ShelliumSshIntegrationTests PRIVATE
        Qt6::Core
        Qt6::Concurrent
        Qt6::Gui
        Qt6::Sql
        Qt6::Widgets
        Qt6::Test
        ShelliumDependencies::libssh
        ShelliumDependencies::libvterm
    )
    if(UNIX AND NOT APPLE)
        target_link_libraries(ShelliumSshIntegrationTests PRIVATE util)
    endif()

    add_test(NAME ShelliumSshIntegrationTests COMMAND ShelliumSshIntegrationTests)

    # 上传流水线（sftp_aio 多请求并发）的字节级回归：只有真实 SFTP 服务器才能验证服务端按
    # max_write_length 截断单次写请求时，新写的偏移续传逻辑是否正确拼接，桩替身测不出这类
    # 时序相关的正确性问题。缺少 SHELLIUM_IT_* 环境变量时测试自行跳过。
    qt_add_executable(ShelliumSftpUploadPipelineTests
        tests/SftpUploadPipelineTest.cpp
        src/core/ConnectionTarget.cpp
        src/core/ConnectionTarget.h
        src/core/HostRecord.h
        src/storage/ApplicationDatabase.cpp
        src/storage/ApplicationDatabase.h
        src/sftp/SftpService.cpp
        src/sftp/SftpService.h
        src/ssh/ShellShim.cpp
        src/ssh/ShellShim.h
        src/ssh/SshSessionNode.cpp
        src/ssh/SshSessionNode.h
        src/ssh/SshSessionStack.cpp
        src/ssh/SshSessionStack.h
        src/ssh/SshTunnelBroker.cpp
        src/ssh/SshTunnelBroker.h
        src/ssh/SshCommandParser.cpp
        src/ssh/SshCommandParser.h
        src/terminal/TerminalEchoFilter.cpp
        src/terminal/TerminalEchoFilter.h
        src/terminal/TerminalPane.cpp
        src/terminal/TerminalSearchBar.cpp
        src/terminal/TerminalSearchBar.h
        src/terminal/TerminalSearch.h
        src/terminal/TerminalPane.h
        src/terminal/TerminalView.cpp
        src/terminal/TerminalView.h
        src/terminal/VtermBridge.cpp
        src/terminal/VtermBridge.h
        src/terminal/LocalShellWorker.cpp
        src/terminal/LocalShellWorker.h
        src/settings/TerminalSettings.cpp
        src/settings/TerminalSettings.h
        src/ui/SettingsWidgets.cpp
        src/ui/SettingsWidgets.h
    )
    target_include_directories(ShelliumSftpUploadPipelineTests PRIVATE src)
    target_link_libraries(ShelliumSftpUploadPipelineTests PRIVATE
        Qt6::Core
        Qt6::Concurrent
        Qt6::Gui
        Qt6::Sql
        Qt6::Widgets
        Qt6::Test
        ShelliumDependencies::libssh
        ShelliumDependencies::libvterm
    )
    if(UNIX AND NOT APPLE)
        target_link_libraries(ShelliumSftpUploadPipelineTests PRIVATE util)
    endif()
    add_test(NAME ShelliumSftpUploadPipelineTests COMMAND ShelliumSftpUploadPipelineTests)

    # 续传与"是否需要更新"的真实服务器回归：最终结果由服务端 stat、能否保存修改时间、
    # rename 能否覆盖已存在文件、以及中断后残片实际留下多少字节共同决定，用桩替身模拟这些
    # 环节等于把被测逻辑一起模拟掉。缺少 SHELLIUM_IT_* 环境变量时测试自行跳过。
    qt_add_executable(ShelliumSftpResumeIntegrationTests
        tests/SftpResumeIntegrationTest.cpp
        src/core/ConnectionTarget.cpp
        src/core/ConnectionTarget.h
        src/core/HostRecord.h
        src/storage/ApplicationDatabase.cpp
        src/storage/ApplicationDatabase.h
        src/sftp/SftpService.cpp
        src/sftp/SftpService.h
        src/ssh/ShellShim.cpp
        src/ssh/ShellShim.h
        src/ssh/SshSessionNode.cpp
        src/ssh/SshSessionNode.h
        src/ssh/SshSessionStack.cpp
        src/ssh/SshSessionStack.h
        src/ssh/SshTunnelBroker.cpp
        src/ssh/SshTunnelBroker.h
        src/ssh/SshCommandParser.cpp
        src/ssh/SshCommandParser.h
        src/terminal/TerminalEchoFilter.cpp
        src/terminal/TerminalEchoFilter.h
        src/terminal/TerminalPane.cpp
        src/terminal/TerminalSearchBar.cpp
        src/terminal/TerminalSearchBar.h
        src/terminal/TerminalSearch.h
        src/terminal/TerminalPane.h
        src/terminal/TerminalView.cpp
        src/terminal/TerminalView.h
        src/terminal/VtermBridge.cpp
        src/terminal/VtermBridge.h
        src/terminal/LocalShellWorker.cpp
        src/terminal/LocalShellWorker.h
        src/settings/TerminalSettings.cpp
        src/settings/TerminalSettings.h
        src/ui/SettingsWidgets.cpp
        src/ui/SettingsWidgets.h
    )
    target_include_directories(ShelliumSftpResumeIntegrationTests PRIVATE src)
    target_link_libraries(ShelliumSftpResumeIntegrationTests PRIVATE
        Qt6::Core
        Qt6::Concurrent
        Qt6::Gui
        Qt6::Sql
        Qt6::Widgets
        Qt6::Test
        ShelliumDependencies::libssh
        ShelliumDependencies::libvterm
    )
    if(UNIX AND NOT APPLE)
        target_link_libraries(ShelliumSftpResumeIntegrationTests PRIVATE util)
    endif()
    add_test(NAME ShelliumSftpResumeIntegrationTests COMMAND ShelliumSftpResumeIntegrationTests)
endif()
