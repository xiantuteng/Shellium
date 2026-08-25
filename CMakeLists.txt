cmake_minimum_required(VERSION 3.21)

project(Shellium VERSION 0.1.12 LANGUAGES CXX)

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

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Widgets Concurrent Network Sql Svg)

# Homebrew、Linux 发行版和 vcpkg 提供的 libssh 均会安装 pkg-config 元数据。
# 通过 IMPORTED_TARGET 引入可以同时携带头文件路径、库路径和间接链接参数。
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBSSH REQUIRED IMPORTED_TARGET libssh>=0.10)
pkg_check_modules(LIBVTERM REQUIRED IMPORTED_TARGET vterm>=0.3)
pkg_check_modules(LIBSODIUM REQUIRED IMPORTED_TARGET libsodium>=1.0.18)
pkg_check_modules(SPDLOG REQUIRED IMPORTED_TARGET spdlog>=1.12)

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
    src/core/ActivityLog.cpp
    src/core/ActivityLog.h
    src/core/ConnectionTarget.cpp
    src/core/ConnectionTarget.h
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
    src/terminal/TerminalPane.h
    src/terminal/TerminalView.cpp
    src/terminal/TerminalView.h
    src/terminal/VtermBridge.cpp
    src/terminal/VtermBridge.h
    src/terminal/TerminalWorkspace.cpp
    src/terminal/TerminalWorkspace.h
    src/ui/ConnectionDialog.cpp
    src/ui/ConnectionDialog.h
    src/ui/HostManagerWidget.cpp
    src/ui/HostManagerWidget.h
    src/ui/HostDistributionIcon.h
    src/ui/SettingsDialog.cpp
    src/ui/SettingsDialog.h
    src/ui/SettingsWidgets.cpp
    src/ui/SettingsWidgets.h
    src/ui/SessionTabBar.cpp
    src/ui/SessionTabBar.h
    src/ui/SshKeyManagerDialog.cpp
    src/ui/SshKeyManagerDialog.h
    src/ui/UiIcons.h
    src/ui/VaultDialog.cpp
    src/ui/VaultDialog.h
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

target_include_directories(Shellium PRIVATE src)
target_link_libraries(Shellium PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Concurrent
    Qt6::Network
    Qt6::Sql
    Qt6::Svg
    PkgConfig::LIBSSH
    PkgConfig::LIBVTERM
    PkgConfig::LIBSODIUM
    PkgConfig::SPDLOG
)

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

install(TARGETS Shellium
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Test Sql)

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
        PkgConfig::LIBSSH
        PkgConfig::LIBVTERM
        PkgConfig::LIBSODIUM
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
        Qt6::Core Qt6::Network Qt6::Sql Qt6::Test PkgConfig::LIBSSH)
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
        PkgConfig::LIBSSH
        PkgConfig::LIBVTERM
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
        PkgConfig::LIBSSH)
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
        PkgConfig::LIBSSH
        PkgConfig::LIBVTERM
        PkgConfig::LIBSODIUM
        PkgConfig::SPDLOG
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
        PkgConfig::LIBSSH
        PkgConfig::LIBVTERM
    )
    if(UNIX AND NOT APPLE)
        target_link_libraries(ShelliumSessionRecoveryTests PRIVATE util)
    endif()
    add_test(NAME ShelliumSessionRecoveryTests COMMAND ShelliumSessionRecoveryTests)

    qt_add_executable(ShelliumSessionTabBarTests
        tests/SessionTabBarTest.cpp
        src/ui/SessionTabBar.cpp
        src/ui/SessionTabBar.h
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
        PkgConfig::LIBSSH PkgConfig::LIBSODIUM)
    add_test(NAME ShelliumHostManagerTests COMMAND ShelliumHostManagerTests)

    # 设置界面改版后的控件接线回归：步进器、滑动开关、配色色板与分类导航都是自绘控件，
    # 只有真实构树才能验证“操作控件 → editedSettings() → SQLite”这条链路没有断。需要
    # Widgets 与 Sql，不涉及 libssh/libvterm 与网络。设置页现在还会在开启保险箱密码时调用
    # 正式保险库流程，因此目标需链接 Vault、VaultDialog 与 libsodium，避免测试使用假实现。
    qt_add_executable(ShelliumSettingsUiTests
        tests/SettingsUiTest.cpp
        src/ai/AiClient.cpp
        src/ai/AiClient.h
        src/ai/AiSettings.h
        src/ai/AiSettingsService.cpp
        src/ai/AiSettingsService.h
        src/core/ConnectionTarget.cpp
        src/core/ConnectionTarget.h
        src/core/HostRecord.h
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
        src/ui/SettingsDialog.cpp
        src/ui/SettingsDialog.h
        src/ui/SettingsWidgets.cpp
        src/ui/SettingsWidgets.h
        src/ui/VaultDialog.cpp
        src/ui/VaultDialog.h
    )
    shellium_add_bundled_fonts(ShelliumSettingsUiTests)
    target_include_directories(ShelliumSettingsUiTests PRIVATE src)
    target_link_libraries(ShelliumSettingsUiTests PRIVATE
        Qt6::Core Qt6::Gui Qt6::Network Qt6::Widgets Qt6::Sql Qt6::Test PkgConfig::LIBSODIUM)
    add_test(NAME ShelliumSettingsUiTests COMMAND ShelliumSettingsUiTests)

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
        src/core/ConnectionTarget.cpp
        src/core/ConnectionTarget.h
        src/core/HostRecord.h
        src/security/Vault.cpp
        src/security/Vault.h
        src/storage/ApplicationDatabase.cpp
        src/storage/ApplicationDatabase.h
    )
    target_include_directories(ShelliumAiAssistantTests PRIVATE src)
    target_link_libraries(ShelliumAiAssistantTests PRIVATE
        Qt6::Core Qt6::Gui Qt6::Network Qt6::Widgets Qt6::Sql Qt6::Test PkgConfig::LIBSODIUM)
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
    target_link_libraries(ShelliumKeepAliveTests PRIVATE Qt6::Core Qt6::Test PkgConfig::LIBSSH)
    add_test(NAME ShelliumKeepAliveTests COMMAND ShelliumKeepAliveTests)

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
        Qt6::Core Qt6::Concurrent Qt6::Gui Qt6::Widgets Qt6::Test PkgConfig::LIBSSH)
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
        Qt6::Core Qt6::Gui Qt6::Sql Qt6::Widgets Qt6::Test PkgConfig::LIBVTERM)
    add_test(NAME ShelliumTerminalAltScreenTests COMMAND ShelliumTerminalAltScreenTests)

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
        PkgConfig::LIBSSH
        PkgConfig::LIBVTERM
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
        PkgConfig::LIBSSH
        PkgConfig::LIBVTERM
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
        PkgConfig::LIBSSH
        PkgConfig::LIBVTERM
    )
    if(UNIX AND NOT APPLE)
        target_link_libraries(ShelliumSftpResumeIntegrationTests PRIVATE util)
    endif()
    add_test(NAME ShelliumSftpResumeIntegrationTests COMMAND ShelliumSftpResumeIntegrationTests)
endif()
