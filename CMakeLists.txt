cmake_minimum_required(VERSION 3.21)

project(Shellium VERSION 0.1.0 LANGUAGES CXX)

# Qt 的元对象编译器负责生成信号、槽以及属性系统所需代码。开启这些自动化选项后，
# 新增 QObject 子类时无需手工维护 moc 文件列表，可减少跨平台构建差异。
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Widgets Concurrent Network Sql)

# Homebrew、Linux 发行版和 vcpkg 提供的 libssh 均会安装 pkg-config 元数据。
# 通过 IMPORTED_TARGET 引入可以同时携带头文件路径、库路径和间接链接参数。
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBSSH REQUIRED IMPORTED_TARGET libssh>=0.10)
pkg_check_modules(LIBVTERM REQUIRED IMPORTED_TARGET vterm>=0.3)
pkg_check_modules(LIBSODIUM REQUIRED IMPORTED_TARGET libsodium>=1.0.18)
pkg_check_modules(SPDLOG REQUIRED IMPORTED_TARGET spdlog>=1.12)

qt_add_executable(Shellium
    src/main.cpp
    src/app/MainWindow.cpp
    src/app/MainWindow.h
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
    src/settings/TerminalSettings.cpp
    src/settings/TerminalSettings.h
    src/storage/ApplicationDatabase.cpp
    src/storage/ApplicationDatabase.h
    src/sftp/RemoteFileManagerWidget.cpp
    src/sftp/RemoteFileManagerWidget.h
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
    src/terminal/TerminalWorkspace.cpp
    src/terminal/TerminalWorkspace.h
    src/ui/ConnectionDialog.cpp
    src/ui/ConnectionDialog.h
    src/ui/HostManagerWidget.cpp
    src/ui/HostManagerWidget.h
    src/ui/SettingsDialog.cpp
    src/ui/SettingsDialog.h
    src/ui/SessionTabBar.cpp
    src/ui/SessionTabBar.h
    src/ui/SshKeyManagerDialog.cpp
    src/ui/SshKeyManagerDialog.h
    src/ui/UiIcons.h
    src/ui/VaultDialog.cpp
    src/ui/VaultDialog.h
)

target_include_directories(Shellium PRIVATE src)
target_link_libraries(Shellium PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Concurrent
    Qt6::Network
    Qt6::Sql
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

    # 生成标准 macOS App Bundle，后续可以直接补充图标、签名和公证配置。
    set_target_properties(Shellium PROPERTIES MACOSX_BUNDLE TRUE)
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

    # 标题栏标签控件测试需要真实的 QTabBar 布局与窗口曝光，因此依赖 Qt6::Widgets 和
    # 平台 GUI；macOS 上还需要 Objective-C++ 的原生标题栏桥接。
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

    # 主机侧栏的两行渲染与标题动作条：副标题必须补齐分组继承的账号/端口，动作条只保留三个
    # 主动作。这些是用户直接看到的行为，靠 Widgets 真实构树验证，不需要网络或 GUI 交互。
    qt_add_executable(ShelliumHostManagerTests
        tests/HostManagerWidgetTest.cpp
        src/core/ConnectionConfigCodec.cpp
        src/core/ConnectionConfigCodec.h
        src/core/ConnectionTarget.cpp
        src/core/ConnectionTarget.h
        src/core/HostRecord.h
        src/security/Vault.cpp
        src/security/Vault.h
        src/storage/ApplicationDatabase.cpp
        src/storage/ApplicationDatabase.h
        src/ui/HostManagerWidget.cpp
        src/ui/HostManagerWidget.h
        src/ui/UiIcons.h
        src/ui/VaultDialog.cpp
        src/ui/VaultDialog.h
    )
    target_include_directories(ShelliumHostManagerTests PRIVATE src)
    target_link_libraries(ShelliumHostManagerTests PRIVATE
        Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Sql Qt6::Test PkgConfig::LIBSODIUM)
    add_test(NAME ShelliumHostManagerTests COMMAND ShelliumHostManagerTests)

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
endif()
