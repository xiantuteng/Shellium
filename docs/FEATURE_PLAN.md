# Shellium — SSH 终端软件功能规划

> 基于 Qt 6.8 LTS 开发的跨平台 SSH 终端管理工具，对标 Termius

---

## 一、项目概述

### 1.1 项目定位

Shellium 是一款现代化的跨平台 SSH 终端软件，旨在提供媲美 Termius 的用户体验，同时保持本地优先、隐私安全的理念。

### 1.2 目标平台

| 平台 | 优先级 | 说明 |
|------|--------|------|
| macOS (arm64 / x86_64) | P0 | 主力开发平台 |
| Linux (x86_64) | P0 | CI/CD 构建验证 |
| Windows (x86_64) | P1 | 后续适配 |

### 1.3 技术栈选型

| 层面 | 选型 | 理由 |
|------|------|------|
| GUI 框架 | Qt 6.8 LTS | 长期支持，API 稳定 |
| 构建系统 | CMake 3.21+ | Qt 官方推荐，CLion 原生支持 |
| 语言标准 | C++20 | 协程、概念、模块支持 |
| SSH 协议库 | libssh 0.10+ | 功能完备：跳板机、Agent 转发、证书认证 |
| 终端模拟 | libvterm + 自绘 Widget | 轻量、可控、不依赖第三方 Qt 终端组件 |
| 本地存储 | SQLite + Qt SQL | 单文件数据库，零配置 |
| 加密存储 | libsodium / OpenSSL | 凭据保险库端到端加密 |
| JSON 处理 | Qt JSON (QJsonDocument) | 原生集成 |
| 日志框架 | spdlog | 轻量高效，异步日志 |

---

## 二、功能模块划分

### 2.1 核心发布门槛

以下两项为 Shellium 的强制 P0 能力，任何 MVP 或正式版本均不得裁剪：

1. 当前服务器必须提供 SFTP 文件管理页面，支持路径直达、上传、下载、重命名和删除。
2. 终端内继续 SSH 到其他服务器时，应用必须维护外层到内层的服务器连接链；文件管理页面既可自动跟随最内层服务器，也可由用户自由选择任意一层服务器。

> **分屏已于 2026-08-07 移除。** 原第 1 项要求同一标签页内可水平/垂直分屏。实际使用中，
> 每个标签页一个终端会话已经够用：需要同时操作多台服务器时新建标签页即可，而分屏树
> （嵌套 QSplitter + 折叠冗余分隔器 + 活动面板跟踪）带来的复杂度并没有换来相应价值。
> 文件管理器因此直接绑定本标签页的终端，不再需要"跟随当前激活分屏"这一层间接。

第二项采用“Shell Integration 事件优先、SSH 命令解析降级、用户手动校正兜底”的识别策略。针对内层服务器的文件管理不会复用交互式终端字节流，而是通过外层连接链建立独立、可验证主机指纹的 SFTP 会话。

远程文件管理器必须位于终端标签页内部，并由该标签页独占实例。不同标签页不得共享当前
路径、服务器选择、传输请求、错误重试状态或会话密码。

### 模块总览

```
Shellium
├── 核心终端模块 (Terminal Core)
├── 连接管理模块 (Connection Manager)
├── SSH 协议模块 (SSH Engine)
├── 文件传输模块 (SFTP)
├── 安全凭据模块 (Vault / Keychain)
├── 高级网络模块 (Port Forwarding / Proxy)
├── 效率工具模块 (Snippets / History / Autocomplete)
├── UI / 主题模块 (Theme / Appearance)
├── 数据持久化模块 (Storage / Sync)
└── 插件系统 (Plugin System) [Phase 3]
```

---

## 三、分阶段功能规划

### Phase 1 — MVP（最小可用版本）

> 目标：能够管理主机列表、建立 SSH 连接、在终端中交互操作

#### 3.1 终端核心

| 功能 | 描述 | 优先级 |
|------|------|--------|
| 终端模拟器 | 基于 libvterm 的 VT100/xterm 终端模拟，支持 256 色 + TrueColor | P0 |
| 键盘输入处理 | 普通键、功能键、修饰键、Alt/Meta 转义序列 | P0 |
| 鼠标输入处理 | 鼠标报告模式（SGR、utf-8 编码） | P1 |
| 选中文本复制 | 鼠标选中文本，复制到剪贴板 | P0 |
| 粘贴文本 | 从剪贴板粘贴到终端，支持安全粘贴（过滤控制字符） | P0 |
| 终端滚动 | 滚动缓冲区，滚轮 / 快捷键滚动 | P0 |
| 搜索终端输出 | 在终端输出中搜索文本 | P1 |
| 字体渲染 | 等宽字体，支持连字（Nerd Font / Powerline 字符） | P0 |
| 终端尺寸协商 | 窗口缩放时自动 SIGWINCH 通知远端 | P0 |

#### 3.2 标签页

| 功能 | 描述 | 优先级 |
|------|------|--------|
| 多标签页 | 每个标签一个终端会话，可新建 / 关闭 / 重命名 | P0 |
| 标签页拖拽 | 拖出为独立窗口，拖回合并 | P1 |
| 嵌套服务器链 | 自动识别终端内进入/退出的 SSH 服务器，维护外层到内层连接链 | P0 |

> 分屏（水平 / 垂直 / 比例调整 / 焦点切换）已于 2026-08-07 移除，见 2.1 节说明。

#### 3.3 连接管理

| 功能 | 描述 | 优先级 |
|------|------|--------|
| 主机配置 | 地址、端口、用户名、认证方式 | P0 |
| 主机分组 | 树形分组结构，支持嵌套 | P0 |
| 分组继承 | 子组 / 主机继承父组的配置（端口、跳板机等） | P0 |
| 主机搜索 | 按名称、地址、标签快速搜索 | P0 |
| 标签系统 | 给主机打标签，按标签筛选 | P1 |
| 主机图标 | 自动识别 OS 类型显示图标，支持自定义 | P2 |
| 导入 / 导出 | 支持导入 OpenSSH config、Termius 导出文件 | P1 |
| 最近连接 | 记录最近连接的主机，快速重连 | P0 |
| 连接历史 | 记录每次连接的时间、时长 | P1 |

#### 3.4 SSH 引擎

| 功能 | 描述 | 优先级 |
|------|------|--------|
| SSH2 协议 | 支持现代加密算法（aes256-gcm、chacha20-poly1305） | P0 |
| 密码认证 | 用户名 + 密码 | P0 |
| 公钥认证 | SSH 密钥对认证（RSA、Ed25519、ECDSA） | P0 |
| 密钥生成 | 应用内生成 SSH 密钥对 | P1 |
| 密钥导入 | 导入已有密钥文件（~/.ssh/） | P0 |
| 密钥口令 | 密钥 passphrase 管理，会话内缓存 | P0 |
| known_hosts | 自动记录和管理 known_hosts，指纹校验 | P0 |
| 连接保活 | KeepAlive 心跳，防止连接超时断开 | P0 |
| 重连机制 | 网络中断后自动重连 | P1 |
| 跳板机 (Jump Host) | 通过跳板机连接目标主机 (ProxyJump) | P1 |
| Agent 转发 | SSH Agent Forwarding | P1 |
| 连接复用 | ControlMaster 多路复用 | P2 |

#### 3.5 安全凭据

| 功能 | 描述 | 优先级 |
|------|------|--------|
| 本地保险库 | 主密码加密存储所有凭据（AES-256-GCM） | P0 |
| 主密码 | 启动时解锁，会话内缓存密钥 | P0 |
| 系统钥匙串集成 | macOS Keychain / Windows Credential Manager | P1 |
| 凭据引用 | 一个凭据可被多个主机引用，一处修改处处生效 | P0 |
| 自动锁定 | 闲置超时自动锁定保险库 | P1 |

#### 3.6 基础 UI

| 功能 | 描述 | 优先级 |
|------|------|--------|
| 主窗口布局 | 左侧主机树 + 右侧终端区域 | P0 |
| 工具栏 | 快速操作按钮（新建连接、设置等） | P0 |
| 状态栏 | 当前连接信息、编码、终端尺寸 | P0 |
| 深色 / 浅色主题 | 跟随系统或手动切换 | P0 |
| 快捷键体系 | 全局快捷键配置（可自定义） | P1 |
| 命令面板 | Ctrl+Shift+P 快速搜索执行命令 | P1 |

---

### Phase 2 — 完整功能版

> 目标：补齐 Termius 核心功能，达到日常可用水平

#### 3.7 SFTP 文件传输

> 路径浏览、上传、下载、重命名、删除和嵌套服务器选择属于 MVP 强制 P0 范围；断点续传、预览和队列增强在 Phase 2 完成。

| 功能 | 描述 | 优先级 |
|------|------|--------|
| SFTP 浏览器 | 双栏文件管理器（本地 / 远程） | P0 |
| 上传 / 下载 | 拖拽传输，进度条显示 | P0 |
| 路径直达 | 输入远程绝对路径直接进入目录 | P0 |
| 重命名 / 删除 | 修改远程文件名，删除文件或目录并进行风险确认 | P0 |
| 服务器链切换 | 在当前终端的外层和各级内层 SSH 服务器间自由切换 | P0 |
| 目录递归传输 | 递归上传 / 下载整个目录 | P0 |
| 断点续传 | 大文件传输中断后可续传 | P1 |
| 文件预览 | 文本 / 图片文件快速预览 | P1 |
| 权限管理 | 修改文件权限（chmod）、属主 | P1 |
| 队列管理 | 传输队列，暂停 / 恢复 / 取消 | P1 |
| 快捷传输 | 终端中右键 "上传到当前目录" | P2 |

#### 3.8 端口转发

| 功能 | 描述 | 优先级 |
|------|------|--------|
| 本地端口转发 | -L local:remote 转发 | P0 |
| 远程端口转发 | -R remote:local 反向转发 | P0 |
| 动态端口转发 | -D SOCKS5 代理 | P0 |
| 转发规则管理 | 保存转发规则，一键启停 | P0 |
| 转发状态监控 | 实时显示转发连接数、流量 | P1 |
| 随连接启动 | 连接主机时自动启动预设的转发规则 | P1 |

#### 3.9 代理与高级连接

| 功能 | 描述 | 优先级 |
|------|------|--------|
| SOCKS5 代理 | 通过 SOCKS5 代理建立 SSH 连接 | P0 |
| HTTP 代理 | 通过 HTTP CONNECT 代理 | P1 |
| Telnet 协议 | Telnet 连接支持 | P2 |
| Serial 串口连接 | 串口终端（波特率、数据位配置） | P2 |
| 本地终端 | 直接打开本地 Shell | P0 |
| SSH 断开回退 | 已建立的远程会话结束后，当前终端自动退回本地登录式 zsh | P0 |
| Mosh 协议 | Mosh 连接支持（需要远端安装 mosh-server） | P2 |

#### 3.10 效率工具

| 功能 | 描述 | 优先级 |
|------|------|--------|
| Snippets 代码片段 | 保存常用命令，侧栏快速执行 | P0 |
| Snippet 分组 | 按分组组织代码片段 | P0 |
| 多主机执行 | 同一 Snippet 在多个主机上同时执行 | P1 |
| 命令历史 | 按主机记录命令历史，跨会话持久化 | P0 |
| 自动补全 | 基于命令历史的智能补全 | P1 |
| 启动 Snippet | 连接主机后自动执行预设命令 | P1 |
| 广播模式 | 向所有打开的终端同时输入 | P1 |

#### 3.11 终端增强

| 功能 | 描述 | 优先级 |
|------|------|--------|
| 主题系统 | 内置多套配色方案 + 自定义 | P0 |
| 字体配置 | 字体族、字号、行高、字间距 | P0 |
| 背景设置 | 纯色 / 图片 / 渐变背景，透明度调节 | P1 |
| 光标样式 | 块状 / 下划线 / 竖线，闪烁开关 | P0 |
| 滚动缓冲区 | 可配置缓冲行数（默认 10000 行） | P0 |
| 终端铃声 | 视觉铃声 / 声音铃声开关 | P1 |
| 超链接识别 | 自动识别 URL，点击打开 | P1 |
| 导出终端输出 | 导出为文本文件 | P1 |
| 会话日志 | 自动记录终端输出到日志文件 | P1 |

#### 3.12 Workspace 工作区

| 功能 | 描述 | 优先级 |
|------|------|--------|
| 工作区概念 | 多个终端会话编组为一个工作区 | P1 |
| 布局保存 | 保存标签页组合，下次恢复 | P1 |
| 广播输入 | 工作区内所有终端同时输入 | P1 |
| 工作区模板 | 预设工作区模板，一键打开多个连接 | P2 |

---

### Phase 3 — 高级特性

> 目标：差异化竞争力，团队协作能力

#### 3.13 云同步与协作

| 功能 | 描述 | 优先级 |
|------|------|--------|
| 云同步 | 主机、密钥、Snippets 跨设备同步 | P1 |
| 端到端加密 | 同步数据端到端加密，服务端不可见 | P0 |
| 团队保险库 | 共享主机 / 凭据给团队成员 | P2 |
| 实时协作 | 多人同时查看 / 操作同一终端 | P2 |
| 会话录制 | 记录终端会话，回放查看 | P2 |
| SSO 登录 | SAML / OIDC 单点登录 | P2 |

#### 3.14 AI 增强

| 功能 | 描述 | 优先级 |
|------|------|--------|
| 自然语言转命令 | 描述意图，AI 生成 Shell 命令 | P1 |
| 命令解释 | 选中命令，AI 解释其作用 | P1 |
| 错误诊断 | 命令报错时，AI 分析原因并给出建议 | P1 |
| 智能补全 | AI 上下文感知的命令补全 | P2 |
| CLI Agent 集成 | 集成 Claude Code / Gemini CLI 等 | P2 |

#### 3.15 插件系统

| 功能 | 描述 | 优先级 |
|------|------|--------|
| 插件 API | 提供扩展接口（终端钩子、UI 扩展） | P2 |
| 脚本引擎 | 嵌入式脚本支持（Lua / JavaScript） | P2 |
| 插件市场 | 社区插件分发 | P3 |

#### 3.16 其他高级功能

| 功能 | 描述 | 优先级 |
|------|------|--------|
| SSH 证书认证 | SSH CA 签发证书认证 | P2 |
| FIDO2 硬件密钥 | YubiKey 等硬件安全密钥支持 | P2 |
| 指纹 / 生物识别 | Touch ID / Windows Hello 解锁保险库 | P2 |
| 导入兼容 | 导入 Termius / PuTTY / MobaXterm 配置 | P1 |
| CLI 模式 | 命令行模式快速连接（shellium user@host） | P1 |
| 全局快捷键 | 系统级快捷键唤起窗口 / 快速连接 | P2 |
| 托盘图标 | 系统托盘，后台保持连接 | P1 |

---

## 四、架构设计

### 4.1 分层架构

```
┌─────────────────────────────────────────────┐
│              Presentation Layer              │
│   (Qt Widgets / QML Views, Theme Engine)     │
├─────────────────────────────────────────────┤
│              Application Layer               │
│  (Connection Controller, Session Manager,    │
│   Command Palette, Shortcut Manager)         │
├─────────────────────────────────────────────┤
│               Domain Layer                   │
│  (Host Model, Credential Model, Snippet      │
│   Model, PortForward Model, Workspace)       │
├─────────────────────────────────────────────┤
│           Infrastructure Layer               │
│  (libssh Wrapper, libvterm Wrapper,          │
│   SQLite Storage, Crypto Engine,             │
│   Sync Engine, Plugin Loader)                │
└─────────────────────────────────────────────┘
```

### 4.2 核心类设计（草案）

```
// SSH 引擎
class SshSession          // 单个 SSH 连接会话
class SshChannel          // SSH 通道（Shell / SFTP / Forward）
class SshKeyManager       // 密钥管理
class SshConfigParser     // OpenSSH config 解析

// 终端
class TerminalWidget      // 终端渲染 Widget (QWidget)
class VtermBridge         // libvterm ↔ Qt 桥接
class TerminalSession     // 终端会话（Widget + SSH Channel）

// 连接管理
class HostModel           // 主机树模型 (QAbstractItemModel)
class HostNode            // 主机 / 分组节点
class ConnectionManager   // 连接生命周期管理
class ConnectionChain     // 维护终端内外层到内层的 SSH 服务器链

// 凭据
class Vault               // 加密保险库
class CredentialStore     // 凭据存储
class MasterPassword      // 主密码管理

// SFTP
class SftpModel           // SFTP 文件模型，可绑定连接链中的任意服务器
class TransferQueue       // 传输队列管理
class TransferTask        // 单个传输任务

// 效率
class SnippetManager      // 代码片段管理
class CommandHistory      // 命令历史
class AutocompleteEngine  // 自动补全引擎
```

### 4.3 数据流

```
用户输入 → TerminalWidget → VtermBridge → SshChannel → libssh → 网络
                                                              ↓
用户查看 ← TerminalWidget ← VtermBridge ← SshChannel ← libssh ← 网络
```

### 4.4 项目目录结构（建议）

```
Shellium/
├── CMakeLists.txt
├── src/
│   ├── main.cpp
│   ├── app/                    # 应用入口与全局管理
│   │   ├── Application.cpp
│   │   ├── MainWindow.cpp
│   │   └── Settings.cpp
│   ├── core/                   # 核心领域模型
│   │   ├── host/               # 主机模型与管理
│   │   ├── credential/         # 凭据与保险库
│   │   ├── snippet/            # 代码片段
│   │   └── workspace/          # 工作区
│   ├── ssh/                    # SSH 引擎封装
│   │   ├── SshSession.cpp
│   │   ├── SshChannel.cpp
│   │   ├── SshKeyManager.cpp
│   │   └── PortForwarder.cpp
│   ├── terminal/               # 终端模拟
│   │   ├── TerminalWidget.cpp
│   │   ├── VtermBridge.cpp
│   │   └── TerminalSession.cpp
│   ├── sftp/                   # SFTP 文件传输
│   ├── ui/                     # UI 组件
│   │   ├── widgets/
│   │   ├── dialogs/
│   │   ├── theme/
│   │   └── icons/
│   ├── storage/                # 持久化
│   │   ├── Database.cpp
│   │   └── VaultStorage.cpp
│   └── utils/                  # 工具类
├── resources/                  # 资源文件
│   ├── themes/
│   ├── fonts/
│   ├── icons/
│   └── shaders/
├── tests/                      # 单元测试
├── third_party/                # 第三方依赖
└── docs/                       # 文档
```

---

## 五、第三方依赖

### 5.1 核心依赖

| 库 | 版本 | 用途 | 许可证 |
|----|------|------|--------|
| Qt | 6.8 LTS | GUI 框架 | LGPL / Commercial |
| libssh | 0.10.x | SSH 协议 | LGPL-2.1 |
| libvterm | 0.3+ | 终端模拟 | MIT |
| SQLite | 3.40+ | 本地数据库 | Public Domain |
| libsodium | 1.0.18+ | 加密 | ISC |
| spdlog | 1.12+ | 日志 | MIT |

### 5.2 构建与开发工具

| 工具 | 用途 |
|------|------|
| CMake 3.21+ | 构建系统 |
| Ninja | 构建后端 |
| clang-format | 代码格式化 |
| clang-tidy | 静态分析 |
| Catch2 / GTest | 单元测试 |
| vcpkg / Homebrew | 依赖管理 |

---

## 六、关键设计决策（待讨论）

### 6.1 终端渲染方案

**方案 A：libvterm + 自绘 QWidget（推荐）**
- 优点：完全可控、性能好、无第三方 Qt 依赖
- 缺点：需要自行实现渲染逻辑

**方案 B：QTermWidget 移植**
- 优点：成熟代码，功能完备
- 缺点：基于 Qt5/KDE 生态，移植成本高，代码臃肿

**方案 C：QWebEngineView + xterm.js**
- 优点：现代渲染、即开即用
- 缺点：引入 Chromium 体积大、内存占用高、进程通信复杂

### 6.2 UI 技术路线

**方案 A：纯 QWidget（推荐 MVP 阶段）**
- 优点：成熟稳定、与终端自绘一致、性能好
- 缺点：动画效果不如 QML

**方案 B：QML / Qt Quick**
- 优点：现代 UI、动画流畅
- 缺点：终端 Widget 集成复杂

**方案 C：混合（QWidget 为主，QML 辅助）**
- 终端区域用 QWidget，设置面板等用 QML

### 6.3 配置存储格式

**方案 A：SQLite（推荐）**
- 优点：结构化查询、事务安全、支持大数据量
- 缺点：二进制文件，不可直接编辑

**方案 B：JSON / YAML 文件**
- 优点：人类可读、版本控制友好
- 缺点：大列表性能差、并发写入风险

> 建议：SQLite 为主存储，导出 / 导入时用 JSON 格式

---

## 七、开发里程碑

| 阶段 | 内容 | 交付物 |
|------|------|--------|
| M1 | 项目骨架 + 构建系统 + libssh/libvterm 集成 | 能编译运行的空壳 |
| M2 | 终端核心 + SSH 基础连接 | 能连上服务器并交互 |
| M3 | 连接管理 + 凭据保险库 + 主机分组 | 可管理主机列表 |
| M4 | 标签页 + 主题系统 | 多会话管理 |
| M5 | SFTP + 端口转发 | 文件传输与网络隧道 |
| M6 | Snippets + 命令历史 + 自动补全 | 效率工具 |
| M7 | Workspace + 跳板机 + 代理 | 高级连接 |
| M8 | 云同步 + AI 增强 | 差异化能力 |
| M9 | 插件系统 + 打包发布 | 正式发布 |

---

## 八、待确认事项

以下问题需要后续讨论确认：

1. **项目名称**：Shellium 是否作为正式名称？
2. **UI 技术路线**：纯 QWidget 还是混合 QML？
3. **终端渲染**：是否采用 libvterm 自绘方案？
4. **MVP 范围**：Phase 1 中哪些功能可以进一步裁剪？
5. **云同步策略**：是否需要自建后端？还是仅支持本地？
6. **许可证**：开源（GPL/MIT）还是闭源商业？
7. **国际化**：初期是否需要多语言支持？
8. **自动更新**：是否需要内置自动更新机制？

---

*文档版本: v0.2 | 更新日期: 2026-08-07*
