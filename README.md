# Shellium

Shellium 是基于 Qt 6、libssh 和 libvterm 的桌面 SSH 终端与远程文件管理工具。它的核心能力
不是“从终端标题猜主机”，而是维护外层到内层的 SSH 会话链，并让当前标签页的 SFTP
文件管理器自动管理用户实际进入的那一层服务器。

## 核心能力

- Shell Integration 事件优先识别内层 SSH；由 Shellium 接管的层使用
  SshSessionStack / SshSessionNode 自持会话，入栈、认证成功和退出均为确定事件；
- Shell Integration 不可用时降级为 SSH 命令解析，推测层会在界面和高风险操作中明确标记；
- 每个标签页独占终端、服务器链、SFTP 路径、连接池请求、传输队列和会话凭据；
- 文件管理器默认跟随最内层服务器，也可手动选择任意外层；内层 SFTP 经正确外层隧道路由；
- 本地/远程双栏、路径直达、双向拖拽、递归传输、断点续传、队列暂停/恢复/取消、
  文本/图片预览、chmod、重命名和删除；
- 终端右键可直接“上传到当前远程目录”，请求固定路由到同一个标签页和当前服务器层。

当前还包括：

- libvterm VT/xterm 状态机、256 色/TrueColor、备用屏幕、双宽字符与可配置滚动历史；
- macOS/Linux 使用 PTY、Windows 使用 ConPTY 的本地交互式 Shell，多标签页，连接异常有限
  自动重连和断开回退；
- 主机树、嵌套分组、配置继承、标签筛选、最近连接、连接历史，以及可复用已有主机凭据的
  可视化主机链；同时保留高级手动 ProxyJump 兼容入口；
- OpenSSH config 导入，以及不含凭据的 Shellium JSON 导入/导出；
- Argon2id + XChaCha20-Poly1305 本地凭据保险库、启动解锁、凭据引用和闲置自动锁定；
- Ed25519 / RSA / 可用时 ECDSA 密钥生成、已有私钥检查和公钥复制；
- 本地、远程和动态端口转发（-L / -R / -D），规则持久化、自动启动与流量统计；
- Snippet 分组、命令历史、历史补全、多主机执行与广播输入；
- 跟随系统/亮/暗主题、字体、光标、铃声、滚动、自动重连和全局快捷键设置；
- 融入系统标题栏的会话标签、侧栏一键收起、会话顶部状态与远程文件开关，以及覆盖主机、
  会话、文件、Snippet 和端口转发的上下文菜单；
- 命令面板，以及工作区保存、校验、恢复和删除。

## 构建

需要：

- CMake 3.21+；
- Qt 6.8+：Core、Widgets、Concurrent、Network、Sql、Test；
- libssh 0.10+；
- libvterm 0.3+；
- libsodium 1.0.18+；
- spdlog 1.12+；
- Ninja 和支持 C++20 的编译器；macOS/Linux 可用 pkg-config，Windows 可用 CMake Config
  Package/vcpkg 或 pkg-config。

~~~bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
~~~

在没有显示器的 macOS/Linux 环境执行 GUI 测试时使用：

~~~bash
env QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure
~~~

macOS 可执行文件位于：

~~~text
build/Shellium.app/Contents/MacOS/Shellium
~~~

Apple Silicon 站外发布使用 Developer ID 签名、公证与 DMG 一键脚本。证书准备、最低系统
兼容性和完整命令见 [macOS DMG 发布指南](docs/MACOS_DMG_RELEASE.md)。

Windows 10 1809+ / Windows 11 x64 的原生构建、交叉工具链、NSIS 与发布验收说明见
[Windows 构建与发布指南](packaging/windows/README.md)。

## 首次使用

1. 首次启动可直接使用；如需主密码保护和查看已保存密码，可在“设置 → 连接与安全 →
   凭据保险箱”中启用“使用主密码保护凭据”。启用时需要设置并确认新密码；关闭时需要
   重新输入当前密码。主密码不会写入数据库，关闭成功后会清除盐值、校验块和内存会话密钥。
   如果忘记主密码，可在解锁或关闭保护窗口点击“忘记密码？”，输入“重置保险箱”确认后
   清除全部已保存的 SSH 密码和私钥口令。主机地址、账号、端口、私钥路径、分组和主机链
   会保留，但被删除的凭据无法恢复，需要重新录入。启动解锁也可以取消并进入应用管理主机。
2. 在左侧“主机管理”新建分组或主机；密码和私钥文件配置会随主机自动保存。开启保险箱
   密码后，密码使用主密码加密；内网主机可在“主机链”中选择已有跳板机，也可现场新建。
3. 双击保存主机连接，或从“连接”菜单发起临时连接。
4. 右侧远程文件管理器默认连接当前终端最内层服务器；使用会话顶部右侧文件夹按钮可收起
   或展开当前标签专属的文件区域。
5. 在终端执行 ssh user@host 时，Shellium 会优先接管并建立新层；exit 或 Ctrl+D 返回外层。
6. 服务器选择器可关闭“跟随最内层”，然后手动管理连接链中的任意服务器。
7. 首次连接未知服务器时必须人工核对 SHA-256 主机指纹。

旧服务器如果只支持 hmac-sha1，Shellium 会在协商失败后显示安全提示。只有用户明确确认，
才会为当前目标开启兼容算法；该设置不会从父分组继承，也不会降低其他服务器的安全基线。

## 真实服务器集成测试

ShelliumSshIntegrationTests 通过 SHELLIUM_IT_* 环境变量连接受控测试服务器。缺少这些变量时，
对应测试会安全跳过，不会连接任意外部主机。集成套件包含故意制造认证失败的场景，不应高频
指向生产服务器，以免触发 MaxStartups 或 fail2ban。

## 当前边界

- macOS arm64 已完成本机构建与测试；Linux 继续使用原有 POSIX 路径，尚需 CI 实机验证；
- Windows x64 已具备 ConPTY、原生 socket、私钥 ACL、CMake/NSIS 构建基线；正式发布仍需
  在干净 Windows 10 1809+/11 VM 完成 GUI、安装器、句柄泄漏和 Authenticode 验收；
- 终端鼠标报告、终端感知块选择、URL 点击与自动会话日志尚未实现；
- SFTP 删除支持文件和空目录，递归目录删除仍需单独的高风险交互设计；
- 外部 SOCKS5/HTTP 代理“用于建立 SSH 连接”尚未实现；这与已完成的 -D 动态转发不同；
- Termius/PuTTY/MobaXterm 导入、系统钥匙串、生物识别、云同步、团队协作、AI 与插件系统
  尚未实现或需要先确定产品和安全方案。

详细状态、剩余优先级与架构约束见：

- [功能规划与实现审计](docs/FEATURE_PLAN.md)
- [工程约束与已知陷阱](docs/ENGINEERING_NOTES.md)
- [发布验证清单](docs/RELEASE_CHECKLIST.md)

### 终端显示恢复

误用 `cat` / `tail` 输出 JAR、ZIP 等二进制文件时，控制字节可能改变文字颜色或隐藏属性，
表现为黑块、选中才看得见文字，也可能出现 `�`。在终端内按 **Cmd+Shift+R（macOS）**
或 **Ctrl+Shift+R（Windows/Linux）** 可本地重置终端模式和当前屏幕，不发送远程命令。
已有主屏内容保留到滚动历史中，其原始颜色保持不变；恢复后可按回车重新显示 Shell 提示符。
会话重连、切换本地 Shell 和进入断线恢复提示时也会自动重置，避免污染延续到下一会话。
这不会修改远端 `stty` 设置；查看 JAR 内容请使用 `jar tf` 或 `unzip -l`。
