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

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Widgets Concurrent Sql)

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
    src/security/Vault.cpp
    src/security/Vault.h
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
)

target_include_directories(Shellium PRIVATE src)
target_link_libraries(Shellium PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Concurrent
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
        src/security/Vault.cpp
        src/security/Vault.h
        src/storage/ApplicationDatabase.cpp
        src/storage/ApplicationDatabase.h
        src/ssh/ShellShim.cpp
        src/ssh/ShellShim.h
        src/ssh/SshCommandParser.cpp
        src/ssh/SshCommandParser.h
        src/terminal/TerminalView.cpp
        src/terminal/TerminalView.h
        src/terminal/VtermBridge.cpp
        src/terminal/VtermBridge.h
        src/terminal/LocalShellWorker.cpp
        src/terminal/LocalShellWorker.h
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

    # 针对真实服务器的连接路径回归。非阻塞握手状态机的价值就在于与真实 libssh 和真实
    # 服务器的时序交互，无法用桩替身覆盖；缺少 SHELLIUM_IT_* 环境变量时测试自行跳过。
    qt_add_executable(ShelliumSshIntegrationTests
        tests/SshSessionStackIntegrationTest.cpp
        src/core/ConnectionTarget.cpp
        src/core/ConnectionTarget.h
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
        src/terminal/TerminalPane.cpp
        src/terminal/TerminalPane.h
        src/terminal/TerminalView.cpp
        src/terminal/TerminalView.h
        src/terminal/VtermBridge.cpp
        src/terminal/VtermBridge.h
        src/terminal/LocalShellWorker.cpp
        src/terminal/LocalShellWorker.h
    )
    target_include_directories(ShelliumSshIntegrationTests PRIVATE src)
    target_link_libraries(ShelliumSshIntegrationTests PRIVATE
        Qt6::Core
        Qt6::Concurrent
        Qt6::Gui
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
