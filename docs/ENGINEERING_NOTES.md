# Shellium — 工程约束与已知陷阱

> **接手本项目前请完整读一遍。**
>
> 下面每一条都是踩过坑之后才总结出来的。它们的共同特点是：**违反后不会立刻报错**，
> 而是表现为难以定位的偶发故障——静默吞掉用户输入、只在第二层以上复现的死锁、
> 看起来完全无关的功能突然失效。重犯的排查代价远高于遵守的代价。

---

## 一、最重要的架构决策：自持会话栈

嵌套 SSH（用户在终端里继续 `ssh` 到另一台服务器）**不是**通过解析终端输出来识别的，
而是由本程序亲自建立每一层会话：

```
Shellium ──libssh S1─────────────────────> 第 1 层
         ──S1 的通道──> libssh S2────────> 第 2 层
         ──S2 的通道──> libssh S3────────> 第 3 层

终端 PTY  ← 绑定栈顶层
SFTP      ← 绑定用户选中的那一层
```

**为什么这样做**：观察式识别（读键盘缓冲、匹配屏幕正则）永远存在观察不到的状态。
早期版本因此产生过大量"幽灵层"——失败的 `ssh` 留下一个用户从未进入过的层，而它常与
真实层同名，界面上分辨不出来，用户会在错误的服务器上删文件。自持之后，层的进入与
退出都是确定事件（通道 EOF），这类漂移从根上消失。

### 两类层必须严格隔离

| | 来源 | `confidence` | 出栈依据 |
|---|---|---|---|
| **自持层** `m_ownedTargets` | `SshSessionStack` 亲自建立 | `Confirmed` | Shell 通道 EOF，**确定事件** |
| **推测层** `m_inferredTargets` | 用户绕过接管、手打 `ssh` 被解析 | `Inferred` | 命令解析，仍是猜测 |

规则：
- `layersChanged` 到达时**整体替换**自持层并**清空**推测层——底座变了，叠在上面的推测就没了依据
- 解析器的 `LeaveServer` **只弹推测层**，绝不能碰自持层
- 密码嗅探只作用于推测层：自持层由程序自己认证，永远不需要偷听终端输入
- 任何展示与高危操作都要读 `confidence`：下拉框标 `⚠ 推测`，删除确认框必须写明服务器

### 受管主机链的逻辑根与物理栈底不同

主机管理中的可视化主机链会把保存主机解析为“跳板 1 → 跳板 2 → 最终目标”。这些跳板层
设置 `ConnectionTarget::transit`，只认证并承载转发，不申请 PTY 或启动 Shell；只有最终目标
是交互层。必须同时遵守以下边界：

- `shellConnected` 只在最终目标就绪后发出，中间跳板的提示符或横幅不得泄露到终端；
- 文件管理器目标列表隐藏 transit 层，但 `SshTunnelBroker` 仍为它们维护实际路由；
- SFTP 抵达最终目标时由其上一跳开通道，不能从本机直连私网地址；
- 持久化主机的 `-L/-R/-D` 绑定最终业务层，而不是物理栈底跳板，也不随临时嵌套层改变；
- 自动重连必须复用完整连接计划，不能只拿最终目标地址重新直连；
- 每一跳使用自己的凭据、主机指纹与算法策略，严禁把最终目标密码复制给整条链。

### `ConnectionTarget::confidence` 默认取 `Inferred`

新代码路径若忘记标注来源，应当落到需要用户确认的保守分支，而不是被当作已验证事实。

---

## 二、非阻塞约束（违反会死锁，且只在第 2 层以上复现）

第 N+1 层的传输**架在第 N 层的通道之上**，而该通道的数据搬运由同一个工作线程完成。
因此建立第 N+1 层时的任何阻塞等待都会饿死第 N 层 → 死锁。

这不是性能问题，是**正确性**问题。本地单层测试完全看不出来。

已改造的三处，新增代码必须遵守同样模式：

1. `SshSessionNode::advanceEstablish()` —— 握手/认证/开通道全是状态机，收到 `SSH_AGAIN` 立即返回
2. `SshSessionNode::beginForward()` / `pumpForwards()` —— 通道建立跨轮次推进，不在原地自旋
3. 主机指纹确认用 `HostKeyDecision` 枚举轮询，**不用 `QWaitCondition` 阻塞**

配套注意：
- `libssh` 在非阻塞模式下要求 `SSH_AUTH_AGAIN` 之后用**完全相同的参数**重试，
  所以密码与 PTY 模式字节串都缓存成成员（`m_secret` / `m_terminalModes`），不能每轮重新构造
- 非阻塞下 `SSH_OPTIONS_TIMEOUT` 不再保护握手，必须自带计时器
- `openForward` 的 `nonBlockingCaller` 参数两条路径需求**相反**：嵌套层的 fd 必须非阻塞，
  SFTP 的同步会话必须阻塞

---

## 三、libssh：只 `free`，绝不先 `close`

**非阻塞会话上 `ssh_channel_close()` 返回 `SSH_AGAIN` 而不完成**，留下一个半写的
CHANNEL_CLOSE 包，之后整条 SSH 流失步——**同一会话上其他完全无关的 channel
（比如交互式 Shell）会一起静默失效**。

对一个 `ssh_channel_open_forward` 失败、从未真正打开的 channel 调用 close 更糟：
远端根本没有对应的 channel id。

`ssh_channel_free()` 自身会在需要时关闭，并正确处理"未打开"与"非阻塞待关闭"两种情况。

当前 `SshSessionNode` 的交互 Shell、后台 exec、待建立转发和活动转发均已统一为 **free-only**
释放路径；`requestPopLayer()` 也会立即 free 并置空栈顶 Shell，不再发送 EOF 后调用 close。
后续新增通道时必须继续执行这一约束，不能因为“正常退出更礼貌”而重新引入显式 close。

> 症状极具迷惑性：转发失败之后终端"莫名其妙"不再响应，看起来像终端的问题而非转发的问题。

---

## 四、终端与远端交互

### 4.1 长内容绝不经交互式终端下发

Shell 集成脚本（约 700 字符）经**独立 exec 通道**写入远端临时文件，
交互式终端里只出现一条约 21 字符的 ` . /tmp/.shm-xxxxxxxx`，其回显由
`TerminalEchoFilter` 精确扣掉；脚本末尾自删。

**为什么**：凡是写给远端的字节都会被 PTY 回显回来。短行 readline 逐字回显，可按字节
精确扣除；**超宽行 readline 会按自己的方式重绘**，回显不再与发出的字节一一对应，
无论怎么匹配都扣不干净。

曾试过"抢在远端 Shell 接管之前发送"（那时 tty 还是规范模式，内核逐字回显）——
**这个竞争赢不了**：sshd 请求 shell 后几乎立刻打印 motd 并交给 bash，任何一次网络往返
都晚于它。第 1 层偶尔侥幸成功、第 2 层必然失败，正是这个竞争的表现。

新增任何需要下发到远端的内容，先问它是否必须经过终端。只有必须在交互式 Shell **进程内**
生效的东西（函数定义、变量）才需要经终端，并且要压到一行以内。

### 4.2 远端关回显的两条路都已否决

- `read -s` 并非 POSIX，不支持时返回非零，用 `||` 回退会**吃掉用户的下一行输入**
- `stty -echo` 多数实现走 `TCSAFLUSH`，会**丢弃已在缓冲区的输入**，回执若先于 stty
  抵达就被吞掉，shim 永久阻塞

宁可留一行噪音，也不能冒丢输入或死锁的风险。回显抑制一律在**本地**做。

### 4.3 OSC 载荷禁用 C0 控制字符

libvterm 的字符串解析器遇到 C0 控制字符会**中止**当前 OSC，把已累积的部分当作完整载荷
交出来。所以 OSC 6773 的 argv 用 `0x1E` 连接后**再 base64**。

> 阴险之处：单参数用例会**碰巧通过**（截断点恰好在唯一参数之后）。必须用多参数用例验证。

远端没有 `base64` 时 shim 直接执行原生 ssh，**绝不发出无人应答的 OSC**——那会让 shim
永远阻塞在 `read` 上，把用户终端挂死。

### 4.4 libvterm 双宽字符的右半格

CJK、全角标点、部分表情占两格，libvterm 把**右半格**标记为 `chars[0] == 0xFFFFFFFF`
而不是 0。只判 `!= 0` 会把这个哨兵当成有效码位，`QString::fromUcs4` 转成 U+FFFD，
于是每个汉字后面跟一个 `�`，列计数也整体错位。`cell.width` 对右半格仍是 1，靠 width 拦不住。

处理在 `VtermBridge::convertCell`：命中哨兵就把 `width` 置 0 并返回。
任何新增的终端渲染路径都要复用 `convertCell`，不要另写一份码位转换。

---

## 五、输入处理

### 5.1 拦截键盘的分支必须显式处理粘贴

macOS 上 Qt 把 Command 映射为 `Qt::ControlModifier`，所以 `Cmd+V` 携带 ControlModifier。
"追加普通字符"的分支通常写成 `!text.isEmpty() && !Control && !Meta`，粘贴因此不匹配
任何分支被**静默吞掉**。

> 症状极具误导性：**手打密码一切正常，粘贴则永远连不上**，日志里只看到"提交了空凭据"。
> 这个 bug 曾导致长时间的错误排查方向。

同理，`QTest` 也不产生**自动重复**事件（`event->isAutoRepeat()`）。真实键盘上按住回车
稍久，重复的回车会落在密码捕获刚开启的瞬间，提交空行。

### 5.2 空行提交与主动取消必须区分

在密码提示处直接回车，OpenSSH 的行为是**重新提示**而不是中止。把两者混为一谈会让
任何一次输入丢失都升级成不可恢复的连接失败。只有 `Ctrl+C` 才算放弃。

### 5.3 接管认证必须在终端内联索要密码

用户刚敲完 `ssh`，预期的是终端里的 `password:` 提示。弹对话框会彻底打断这个心智模型。
实现在 `TerminalView::beginLocalLineCapture()`：按键不发往远端、不回显——因为那一层的
SSH 会话尚未认证完成，远端还不存在。

拿到的密码要**写回目标**并随快照交给 SFTP，否则文件管理器会再弹一次密码框。

### 5.4 层未就绪前绝不发 `layersChanged`

曾经在隧道一通就 `push_back` 并立刻发信号，而此时该层只完成了选项配置。文件管理器因此
对未认证的层发起 SFTP，弹出本不该出现的密码框。层就绪的通知统一由
`advanceEstablishingLayers` 在 `step.ok` 时发出。

---

## 六、SFTP

- 连接**按目标池化**（`stableKey()` 已含跳板链，不同环境的同名内网主机不会串用）。
  同一连接上的操作由 mutex 串行化，不同目标并行
- **libssh 的 SFTP API 是同步的**，与非阻塞会话不兼容；且即便兼容，也不能跑在终端的
  泵送线程上——一次大文件传输会冻住终端。所以 SFTP 走独立线程 + 独立连接，
  **不复用会话栈里的 libssh 会话**
- 第 2 层起的 SFTP 必须经**外层隧道**抵达。缺少 `tunnelRouteId` 时会退化成从本机直连内网
  地址——而本机很可能就在同一私有网段上，于是连到局域网里另一台碰巧同 IP 的机器，
  **在错误的服务器上列目录甚至删文件**
- 要抵达第 N 层，转发必须由第 **N−1** 层发起（`SshSessionStack::originLayerFor`）

---

## 七、服务器策略降级

`AllowTcpForwarding no` 只禁用 `direct-tcpip` 通道类型，**不影响 session（shell/exec）通道**。
收到 `SSH_OPEN_ADMINISTRATIVELY_PROHIBITED`（错误文本含 `error (1)`）时自动改用
**exec 中继**：在远端跑一条中继命令，用它的标准输入输出当字节管道
（`bash /dev/tcp` → `nc` → `socat` 依次尝试）。

两个实现细节：
- 用 `{ ...; } 3<>/dev/tcp/...` 复合重定向而**不是** `exec 3<>...`——后者在非交互 shell 里
  重定向失败会直接终止整个 shell，回退链就没机会执行
- `trap "kill $p" EXIT` 保证通道关闭时后台的 `cat` 一并退出，不在服务器上留下游荡进程

接管失败时**必须把用户原本的 ssh 命令交还给远端执行**，否则用户敲下的命令凭空消失，
重试多少次都一样（每次都走同一条失败路径）。

---

## 八、测试规范（强制）

**新增功能或修复 BUG，修好之后必须补一条自动化测试用例，随代码一起提交。**

### 测试目标

每个测试类**一个独立可执行文件**（Qt 惯用做法）。共用 `main` 会让按名过滤测试时
另一套报 "Unknown test function" 并返回非零，破坏单独复现某个用例的工作流。

| 目标 | 内容 | 依赖 |
|------|------|------|
| `ShelliumCoreTests` | 命令解析、终端视图、设置、Vault | GUI + Sql + libssh + libvterm + libsodium |
| `ShelliumKeepAliveTests` | 保活判定纯逻辑 | Core + libssh |
| `ShelliumStorageTests` | 持久化读写 | Core + Sql |
| `ShelliumSftpRecursiveTests` | 递归传输纯逻辑 | Core |
| `ShelliumConnectionToolsTests` | 配置导入导出、SSH 密钥、SOCKS5 协议 | Core + Network + Sql + libssh |
| `ShelliumProductivityTests` | Snippet、历史、补全、多目标与广播接线 | GUI + Sql + SSH/终端依赖 |
| `ShelliumSettingsUiTests` | 设置界面自绘控件接线（步进器、开关、配色色板、分类导航） | GUI + Sql |
| `ShelliumSshIntegrationTests` | **真实服务器**连接路径 | 全部；缺 `SHELLIUM_IT_*` 时自动跳过 |

### 必须验证测试真的抓得住

把修复**临时改回有缺陷的版本**，确认用例失败，再恢复。不做这一步不能声称"已覆盖"。

> 曾有一条用例，去掉修复后照样通过——说明它根本没抓住目标，等于白写。
> 也曾有一条只测"payload 为空"，而 Qt 把空 `QByteArray` 绑成 NULL 被 `NOT NULL` 约束
> 顺手挡下，测试通过得毫无意义。

### 断言可观察行为，不要只断言字段

`tunnelRouteId` 非空不代表隧道通；必须真的走一次 SFTP 列目录、真的读到对端 SSH 横幅、
真的看到命令回显。

### 注意 QTest 覆盖不到的输入路径

`QTest::keyClicks` 只模拟逐字键入，**测不到粘贴**（用 `QTest::keySequence` + 剪贴板），
也**不产生自动重复事件**。这两条都真实漏过过 bug。

### 集成测试对真实服务器的影响

集成套件里有几条**故意制造认证失败**的用例。反复运行曾触发目标服务器的
`MaxStartups` 限流或 fail2ban 封禁，症状是 TCP 连得上但握手前即断开
（`kex_exchange_identification: Connection closed by remote host`）。
针对生产服务器运行时请注意频率。

---

## 九、凭据处理

- 测试凭据存放在项目根的 `AGENT.md`，**已被 `.gitignore` 排除**，不得提交
- 诊断日志只记录长度，**绝不记录字符内容**（见 `shellium.auth` 日志分类，
  用 `QT_LOGGING_RULES="shellium.auth.debug=true"` 打开）
- `Vault` 的空校验块**必须**判定为失败：那代表保险库尚未初始化，调用方应走创建流程。
  把两者混为一谈会让损坏数据被静默当成新保险库，**覆盖掉用户已有的凭据**
- 数据库里**只存密文**；`saveCredential` 的明文防护是启发式的（只能验证"有密文 + 有算法
  标识"），真正的保证要靠调用路径收口

---

## 十、Shell 集成对远端环境的影响

注入的 shim 会：

| 改动 | 范围 |
|---|---|
| `/tmp/.shm-xxxxxxxx` 约 700 字节临时文件 | 脚本末尾 `rm -f` 自删 |
| 定义 `ssh()` 函数 | 仅当前交互式 shell 进程，内存中 |
| 给 Bash 的 `HISTCONTROL` 追加 `ignorespace` | 保留原有 ignoredups/erasedups；已有 ignoreboth 时不改动 |

不写入任何 rc 文件，断开即消失。函数只在交互式 shell 生效（`case "$-" in *i*)` 护栏），
`command ssh` 可绕过。

安装命令会在独立 exec 通道检查登录 Shell，仅 bash/zsh 才写入权限为 0600 的临时脚本；
后台 exec 必须明确返回退出码 0 才会向交互终端发送 source。失败原因（通道错误、退出码与
受限长度的 stderr）通过 `shellIntegrationFailed` 上报，SSH 会话本身继续可用并降级到命令
解析路径。原先从未被读取的 `m_shellIntegrationEnabled` 已删除，避免出现“界面以为关闭、
工作线程仍继续注入”的伪开关。

exec 中继在 `ssh_channel_request_exec()` 成功后还会保留一个短暂确认窗口：远端缺少
`/dev/tcp`、`nc`、`socat` 或命令立即退出时，错误会作为转发失败返回，而不是先交付一个
随后立刻 EOF 的伪成功 socket；交付后的异步 stderr 则写入 `shellium.ssh` 日志分类。

---

## 十一、端口转发的 fd 所有权

本地转发（-L）、远程转发（-R）和动态转发（-D）均绑定一条已经认证的**根 SSH 会话**。
规则运行时不允许直接从自己的网络线程调用 libssh，所有通道操作都必须排队回
`SshSessionStack` 所属工作线程。

远程转发尤其要遵守“一个 fd 只有一个所有者”：

1. `SshSessionNode` 接受 `forwarded-tcpip` 后只向上交付一次 fd；
2. `PortForwardManagerWidget::dispatchRemoteConnection` 按准确的 sessionStack 与 ruleId
   唯一路由；
3. 命中运行时后由 `PortForwardRuntime::acceptRemoteConnection` 接管；
4. 规则已停止、标签已切换或找不到运行时时，管理器必须立即关闭孤儿 fd。

不能让每个规则分别订阅同一个“新连接”信号。那会形成多个接收者同时认为自己拥有 fd 的
双重关闭/重复读写竞态，通常只在快速停启规则或同一主机打开多个标签时出现。

终端断开时必须先 `requestStop()`，再发送会话即将关闭信号，让所有运行时停止后才释放
`SshSessionStack`。断开清理期间 `TerminalPane::forwardingSessionStack()` 仍应返回尚存活的
原对象，不能仅因 `m_shellReady` 已变为 false 就返回空指针，否则管理器无法精确注销。

---

## 十二、自动重连不是无限重试

自动重连只针对“曾经成功建立、随后没有远端正常退出码”的异常断线。以下情况不能自动重试：

- 用户主动关闭、切换或正常执行 exit；
- 首次主机指纹、认证或算法协商失败；
- 服务端给出正常退出状态；
- 用户把最大重试次数设为 0。

每轮异常断线有独立 token，默认最多 3 次；延迟按首轮秒数指数增长并封顶 60 秒。新连接成功、
用户手动发起连接或标签销毁都会使旧 token 失效，排队中的 QTimer 回调必须再次核对 token，
不能让上一台主机的延迟任务覆盖用户刚打开的新会话。

每次自动尝试都应写入保存主机的连接历史；临时连接没有持久 hostId，不得制造无法归属的
孤立历史行。重试耗尽后回到本地 Shell，使标签保持可操作。

---

## 十三、Qt 空字符串与 SQLite NOT NULL

Qt 的默认 `QString` 可处于“长度为 0 且 `isNull()` 为 true”的状态。把这种值直接交给
`QSqlQuery::bindValue` 会绑定成 SQL NULL，而不是空 TEXT。设置表的 value 是 NOT NULL，
而空字体族和用户主动取消的快捷键又确实需要保存为空文本。

因此 `ApplicationDatabase::setSetting` 必须在数据库边界把 null QString 规范化为非 null
空串。不要要求每个调用方分别修补，也不要放宽表约束；后者会让“缺失值”和“业务空值”
再次混在一起。

---

## 十四、工作区恢复必须先验证、后破坏

打开保存工作区会关闭当前全部连接，是不可轻易回滚的 UI 操作。恢复流程必须先完成：

1. JSON 语法、版本、标签数量和字段类型校验；
2. 所有非空 hostId 的存在性与 Host 类型校验；
3. 构造完整的待恢复快照；
4. 向用户确认替换当前标签。

只有全部成功后才能移除当前标签并新建工作区。逐个“关一个、解析一个”会让中途损坏的
工作区把用户现有会话破坏到一半。临时连接也不能被静默保存成本地 Shell：没有持久主机记录，
下次无法可靠恢复地址、跳板链与凭据引用，应在保存阶段整体拒绝并提示先保存主机。

---

## 十五、终端渲染（libvterm + QPlainTextEdit）

### 15.1 备用屏不是滚动历史

备用屏（vim/top/less 等全屏程序）与主屏是两套完全不同的显示契约。`VtermBridge::render`
在备用屏下必须跳过 scrollback、固定渲染全部 `m_rows` 行；`TerminalView::renderPreservingScroll`
必须把滚动条钉在 `minimum()` 而不是贴底。主屏是不断增长的历史、按底部对齐阅读；备用屏是
尺寸恰好等于 PTY 网格的画布，行 0 必须落在视口顶部。两者混在一起渲染，网格会整体沉到
历史之下，被裁掉的正是光标最常停留的前几行。

备用屏里的按键属于全屏程序自己的键位表：继续累积 `m_currentCommand` 会弹出历史补全浮层，
抢走 vim 的 Tab / 回车 / Esc；Ctrl-D、Ctrl+C 若照常发 commandSubmitted / nestedSshAborted
会凭空弹掉一层真实服务器。任何涉及渲染、滚动、按键分发的改动都先看
`VtermBridge::isAlternateScreen()`。方向键/Home/End 一律走 `VtermBridge::sendKey`，不要硬编码
CSI——vim 开 DECCKM 后正确的是 SS3（`ESC OA`）。回归见 `tests/TerminalAltScreenTest.cpp`。

### 15.2 上报行数按控件能完整显示的段数算

`TerminalView::recalculateTerminalGeometry` 报给 PTY 的行数必须是
`(viewportHeight - 2 * documentMargin - 1) / cellHeight`，即 QPlainTextEdit 真正能完整显示的
段数，不是 `viewportHeight / cellHeight`。直接除会多报一行——主屏看不出（只顶掉最旧一行
历史），备用屏下会把网格第 0 行连同光标推出视口上方，现象是“vim 里没有光标、文件从中间
开始显示”，极易误判成光标渲染坏了。改动字号、行高、padding、documentMargin 后跑
`ShelliumTerminalAltScreenTests::reportedRowsMatchDisplayableRows`。

### 15.3 主题必须自带 ANSI 16 色表

libvterm 内置 ANSI 调色板是 VGA 那套值（ANSI 蓝 = #0000e0），且**不随**
`vterm_screen_set_default_colors()` 变化。只配 background/foreground 时，`ls` 目录名、grep
高亮、vim 语法色仍是 1980 年代暗色，在深色主题上读不清。每套主题必须通过
`vterm_state_set_palette_color()` 自带 16 色表（`TerminalPalette::ansi`），缺失的由
`TerminalSettings::defaultAnsiPalette(dark)` 兜底。换调色板后必须 `invalidateDocument()`——
已渲染的段存的是解析后 RGB，不会自己更新。

### 15.4 改默认色要同步刷新 libvterm 画笔

`vterm_screen_set_default_colors()` 只改 state 的 default_fg/default_bg；当前画笔 `state->pen`
里是复位那一刻**已解析成 RGB** 的副本。平时无碍，但 DECSC/DECRC（vim 进出备用屏的
`CSI ?1049h/l`）会把 pen 存档再回灌，那份陈旧 RGB 被当成“显式指定的背景色”，退出 vim 后
每行都顶着一块与主题不符的底色。`setDefaultColors()` 之后调 `refreshPenDefaultColors()`：用
`vterm_state_get_penattr` 确认画笔当前确实是默认色，再注入 `ESC[39m`/`ESC[49m` 让 libvterm
重取副本；远端正显式着色时不要注入。写回归时主题色必须**不等于**兜底色 `#111318`，否则恒过。

### 15.5 双宽字符右半格的哨兵是 0xFFFFFFFF

libvterm 把双宽字符（中日韩、全角标点、部分 emoji）的**右半格**标记为
`cell.chars[0] == 0xFFFFFFFF`，不是 0，且 `cell.width` 对右半格仍是 1。判空只写
`chars[0] != 0` 会把哨兵当有效码位，`QString::fromUcs4` 转成 U+FFFD——`正在连接` 渲染成
`正�在�连�接�`，列计数也被打乱。`VtermBridge::convertCell` 里先查哨兵、命中就置 `width = 0`
并返回；渲染循环与 `cursorLineText()` 都按 `width > 0` 过滤。任何新增终端渲染路径都复用
`convertCell`，不要另写码位转换。回归 `terminalViewRendersWideCharacters`。

### 15.6 行高锚点只能按字符格式剔除

复制与导出剔除行高锚点走 `TerminalView::textWithoutLineAnchors()`，**按
`VtermBridge::isLineAnchorFormat()` 的字符格式逐 fragment 判定**。`lineAnchorSuffix()` 只是
锚点的上限集合，仅能用于断言，不能用于剔除——实际锚点个数由本机字体探测决定
（`buildLineAnchors()`），emoji 后备字体与 CJK 相同或探测不到时只有一个 U+4E2D，与写死的
`中`+U+200B 后缀对不上，那个本该不可见的“中”就原样进了剪贴板。凡是把文档内容交给外部
（剪贴板、导出、日志、搜索比对）的路径都用格式判定，绝不无差别删除 U+4E2D。回归
`TerminalAltScreenTest::partialSelectionDropsLineHeightAnchors`。

### 15.7 输出洪流：`cat` 一个二进制文件会同时引爆五个问题

`cat 某个.jar` 很容易被当成"用户瞎折腾"，但它会一次性暴露终端管线上五个互相独立的缺陷，
其中两个会**直接 abort 掉整个进程**（用户机器上已留下崩溃报告）。全部回归在
`ShelliumTerminalFloodTests`。

#### （1）回流必须保持关闭

`VtermBridge` 构造时显式 `vterm_screen_enable_reflow(m_screen, false)`——这也是 libvterm 的
默认值。libvterm 0.3.3 的 `resize_buffer()` 自底向上重排逻辑行，某条行放不进新网格时只用

```c
if(old_row_start <= old_cursor.row && old_cursor.row < old_row_end)
```

补救光标（末尾的 `<` 是 off-by-one），补不到就 `fprintf` + `abort()`。

**别再试"识别危险状态再临时关掉回流"。** 这条路已经走过并失败：判据只能预测上游那段有
缺陷的代码会怎么走，而连续多轮缩放（真实的窗口拖拽就是这样）会积累出判据覆盖不到的状态。
随机内容 + 连续多轮缩放的 fuzz：4000 例里 646 次 abort，判据漏掉 436 次；同一套 fuzz 在关闭
回流下 20000 例零 abort。代价是缩放不再重排长行（变窄时尾部被截断），这正是不做回流的终端
的一贯行为。上游修掉 off-by-one 之后才可以重新打开，届时请连同 fuzz 一起重跑。

#### （2）每次 `vterm_set_size` 之前必须把光标夹回网格

`VtermBridge::clampCursorIntoGrid()`。libvterm 0.3.3 的 `savecursor(state, 0)`（DECRC，
`ESC 8`）直接 `state->pos = state->saved.pos`，**不做任何夹取**；而缩小网格的 `on_resize`
只夹 `state->pos`，从不更新 `state->saved.pos`。于是

> 大网格里 `ESC 7` → 网格缩小 → `ESC 8`

就留下一个越界的光标行，下一次 `vterm_set_size` 在 `resize_buffer()` 里永远找不到它，
`new_cursor` 保持 `{-1,-1}`，随即 abort。二进制文件里 `ESC 7`/`ESC 8` 不过是两个字节的巧合
组合，而缩放随时会发生——改窗口、缩放字号，甚至 `noteRenderedCellHeight()` 的首帧行高纠正，
**用户什么都不用做**。

**这条路径与回流无关**（关掉回流照样复现），当初正是因为先入为主地以为"关掉回流就完了"
而多花了一轮排查。libvterm 没有公开的光标写接口，因此用它自己的协议搬：CUP 在 `on_csi`
收尾处一定会被夹进网格（origin 模式夹进滚动区，非 origin 夹进 `[0, rows-1]`）。

#### （3）BEL 必须限流

BEL 只是一个普通字节（0x07），二进制里满地都是——那个 34MB 的 jar 含 **122873 个**。一个
字节一次 `QApplication::beep()` 意味着在 GUI 线程的解析循环里同步响十几万次，macOS 会把每
一声都压进音频队列：命令早就跑完了，机器还在"嘟嘟"响。限流放在 `VtermBridge::bellCallback`
（200ms 抑制窗口，与 xterm 的 `bellSuppressTime` 一致），**不是**放在某个消费方——可听响铃、
将来可能的视觉响铃都该看到同一串已经合并好的事件。修复后同一个文件只放出 36~48 声。

#### （4）PTY 读取端必须有背压

GUI 线程那一侧要把字节喂进 libvterm、重排 QTextDocument，实测吞吐只有约 8 MB/s（Release，
Debug 更低），比 `cat` 往 PTY 里灌的速度慢一到两个数量级。读到 EAGAIN 为止的读循环会把差额
全部堆进 GUI 线程的事件队列：34MB 的文件实测让常驻内存冲到 234MB，而排在后面的重绘和按键
一次都轮不到——**这就是用户看到的"界面卡住"**。

`LocalShellWorker` 用 `m_outputInFlight` 做有界在途字节数（1 MiB），超限就不再 poll
`POLLIN`，让内核缓冲区顶住、`cat` 自己阻塞在 `write()` 上——这正是真实终端的行为。实测
128MiB 的输出在 GUI 线程完全不回执时只发出 1048779 字节就停住了。

**消费方必须无条件调用 `notifyOutputConsumed()`**，包括因会话代次不符而丢弃的那批；漏调会
让计数只增不减，读取端永久停摆。回执时**不能用 `m_localWorker`**：会话已切换时它指向的是
新 worker，回执会记到错误的那一个头上。

#### （5）关闭路径必须继续排空 PTY，否则 SIGKILL 也杀不掉子进程

主端不再读时，写终端的进程停在内核的 tty 输出等待里，信号要等它回到用户态才生效——而它回
不去。实测现象是 `kill(pid, SIGKILL)` 返回 0、`kill(pid, 0)` 仍返回 0，`waitpid` 永远等不到
它退出，工作线程就此卡死，析构时报 `QThread: Destroyed while thread is still running`。
**用户关掉一个正在 `cat` 大文件的标签页就会撞上。**

`LocalShellWorker` 的 SIGHUP 宽限期与 SIGKILL 之后都边排空边等：排空到位 76ms，缺排空
7066ms。

> 写用例时注意：SIGKILL 之后那段轮询有上限，超时后 `close(masterFd)` 也会顺带把子进程放
> 出来，所以**只断言"线程能结束"的用例抓不住这个缺陷**（已实测：去掉排空照样通过）。
> 必须断言关闭耗时。

SSH 通道（`SshSessionStack`）用同一套在途字节背压，同一条"无条件回执"规则。

### 15.8 多标签内存：两处大头，省内存不许砍体验

用户报告 7 个标签 1.7 GB。实测单标签（10000 行、170 列日志）161 MB，拆开是两处：

**（1）滚动历史逐格保存。** 每格一个 `Cell`（QString + 两个 QColor + 标志 ≈ 72 字节）再加
一次 QString 堆分配，一行约 20 KB，10000 行约 190 MB。现在历史存成 `PackedLine`（整行文本 +
样式段，非"每格一个 UTF-16 字符"的行才附带每格一字节布局），同样内容约 6 MB。

- 解包结果必须与 `convertCell()` 的输出**逐字段相等**：增量渲染靠"滚进历史的这一行和上次
  写的屏幕行一样"来原地保留该段，一旦不等就删掉重写，用户的选区随之塌缩。空格子（制表符
  跳过的格）不能补成空格，无效色不能补成黑色，双宽右半格要还原成默认 Cell。
- 整体重建走 `writePackedLine()` 直接写文档，不逐格解包——逐格解包让 10000 行重建从 159 ms
  拉长到 249 ms。

**（2）QPlainTextEdit 每排版一段，就给该段的 QTextEngine 留一块 HarfBuzz 缓冲**（`hb_buffer`，
按行长约 3–6 KB），直到这一段被删除才释放。`tail -f` 时每一行都经过视口，10000 行就是约
60 MB。`QTextLayout::clearLayout()`、`markContentsDirty()`、`setCacheEnabled(false)` 等公开接口
**都不释放它**（已逐一实测）。唯一的办法是让段本身消失：`VtermBridge::releaseBlockLayouts()`
把视口外的历史段删掉再按原样写回（多段变更时 QPlainTextDocumentLayout 只清空、不重排）。
约束：

- 有选区或正按着鼠标时不做，否则选区塌缩。
- 先把 QPlainTextEdit 自己的文本光标停到文档末尾（保持滚动位置）：否则删除会把它挤进被
  重写的区间，Qt 求光标矩形时从视口首段逐段排版到光标所在段，一轮释放反而在别处留下一串
  新缓存。
- 需要释放的段只能靠登记：绘制过的段、滚动到过的位置（窗口被遮挡时滚动不会重绘，但 Qt
  照样排版，只在 paintEvent 里登记会漏）、尾部那一屏原地改写过的段、停光标时 Qt 顺手排版的
  末尾几屏。登记与保留范围都是视口上方两屏、下方三屏，覆盖
  `QPlainTextEditControl::blockBoundingRect()` 自己会去排版的范围。
- 首段（前面没有段分隔符）的块数据换不掉，末尾那一屏每批输出都会重排，两者按设计常驻。

**不许用"砍历史"换内存。** 曾经的做法是默认历史降到 2000 行、后台标签再裁到 1000 行并清空
文档：用户切回后台跑 `tail` 的标签，前面的日志已经找不回来，导出也拿到空内容。现在默认仍是
10000 行，另有 32 MB 的字节上限只拦病态情况（几十万行、数千列的超长行）；隐藏的标签只暂停
文档同步、文档原样保留，切回时做一次增量同步，`flushPendingRender()` 会补上隐藏期间欠的帧，
导出与查找读到的是最新内容。回归用例在 `tests/TerminalMemoryTest.cpp`。

### 15.9 换行与提示符之间不画光标

zsh 回车是两次写入：先 `\r\r\n` 与 PROMPT_SP 的 `\r \r`，约 20ms 后（precmd 钩子越重越久）
才发提示符。空闲后首帧立即渲染，于是会单独画出一帧"光标在新行第 0 列"，连续回车时行首
一直闪。`TerminalView` 在 feed 后按 libvterm 状态判断：光标在第 0 列且该行为空，就暂停绘制
光标，后续输出让光标离开空行行首时立即恢复，最多等 `kCursorSettleMilliseconds`（120ms）——
`cat`、`read` 这类真正停在空行行首等输入的程序随后照样能看到光标。别改成"有输出就藏光标"：
打字回显也是输出，那样光标会随每次按键闪。回归用例
`TerminalAltScreenTest::cursorHiddenAtBlankLineStartUntilPromptArrives`。

---

## 十六、Qt 拖拽：非 item-view 的 QAbstractScrollArea 要注册 viewport

给 `QPlainTextEdit` / `QTextEdit` / `QScrollArea` 等直接继承 `QAbstractScrollArea` 的控件手写
拖拽支持时，两行缺一不可：

```cpp
setAcceptDrops(true);
viewport()->setAcceptDrops(true);
```

真正接收操作系统原生拖拽事件的是 viewport 子控件。viewport 没注册时 macOS 原生视图从不
接受该拖拽类型，Qt 层完全收不到事件，且**没有任何日志线索**。`QAbstractItemView`
（QTreeWidget/QListWidget）不受此坑影响——`setDragDropMode()` 内部已替你注册。

第二个坑：viewport 注册后仍可能“能拖进来但松手无反应、一直显示禁止图标”。`QPlainTextEdit`
/ `QTextEdit` 自带一份 `dragMoveEvent`，按 `canInsertFromMimeData` 判定；控件只读、拖的是
文件 URL 时每次鼠标移动都 `ignore()`，覆盖掉 dragEnter 阶段的 accept，`dropEvent` 永不触发。
**`dragEnterEvent`、`dragMoveEvent`、`dropEvent` 三个都要显式重写并对同一 MIME 条件 accept。**

单元测试只把合成事件发给 `widget.viewport()` 绕过了操作系统注册环节，测试全绿也不能证明
真机可用——这一步必须真机手动验证一次。

---

## 十七、多语言（i18n）

改动界面用户可见文字时的强制流程见 [`AGENTS.md`](../AGENTS.md) 的「多语言」一节，设计与
现状见 [`docs/I18N_DESIGN.md`](I18N_DESIGN.md)。要点：

- 源语言是英文，`tr()` 字面量即英文；14 种语言的 `.ts` 都登记在 CMake 的
  `SHELLIUM_TS_FILES`，`.qm` 编进 Qt 资源前缀 `/i18n`。
- **只要增删改了 `tr()` / `QT_TRANSLATE_NOOP` 文案，必须在同一次改动里同步更新其余 13 种
  语言的对应译文**，不允许只改英文源。流程：`Shellium_lupdate` 刷 `.ts` →
  `scripts/i18n/drafts/<lang>.tsv` 填译文 → `scripts/i18n/fill_drafts.py` 回填 → 跑 `ctest`。
- 新增常驻控件文案按 `src/i18n/Retranslate.h` 的 `i18n::onRetranslate` 接入免重启热切换；
  无 `Q_OBJECT` 的嵌套对话框类必须加 `Q_DECLARE_TR_FUNCTIONS(类名)`，否则运行时 tr() 上下文
  与 lupdate 提取的类名不匹配，切换语言后退回英文。
- **挂在常驻 `QMenu` 上、只在构造期设过一次文案的 `QAction` 也必须进 `onRetranslate` 闭包**——
  命令面板 `openCommandPalette()` 通过 `findChildren<QAction*>` 收录全部动作，且每个终端标签页
  各有一份文件管理器实例，漏掉的话切换语言后面板里会以上一次语言重复列出该项。
- **由某个 `reloadXxx()` / 数据驱动重建出来的列表行、卡片内控件（`QListWidget::setItemWidget`
  的行按钮等），文案在重建函数里一次性写死，`onRetranslate` 闭包必须重跑该重建函数**，
  否则切换语言后已有行会残留上一次（甚至更早）语言——`ProductivityWidget` 片段行的
  “执行/复制”按钮即因此在闭包里调 `reloadSnippets()`。右键菜单是按需构造的，不受影响。
- **其它容易漏掉的一次性写文案，全部要进 `onRetranslate` 闭包**：
  - `QSpinBox::setSuffix()`（`SettingsDialog` 的“行 / px / 次 / 秒”后缀）——它不是
    lambda 文案提供器，构造期设一次就不会再变。
  - 不由 `SettingsCard` 托管的裸 `QPushButton`（AI 设置页的“测试连接 / 保存”）、裸
    `QToolButton` 的 `setToolTip/setAccessibleName`。
  - `MainWindow` 侧栏标题/导航栏 Tooltip：**绝不能把 `{tr("Hosts"), …}` 存进
    `static QStringList`**，static 只在首次调用时按当时语言求值一次；用每次实时求值的
    `sidebarPageTitle(int)`，并在 `retranslateUi()` 里按 `m_activeSidebarPage` 重设。
- `SettingsCard::addWideRow(...)` 的字符串版返回的是 **field 本身**而非行容器；lambda 版
  要挂重译得从 `field->parentWidget()` 去 `findChildren<QLabel*>`，直接对返回值找会什么
  都搜不到。
- **`static` + `tr()` 是隐蔽的语言冻结源**：`static const QHash/QList{ …, tr("x"), … }` 只在
  首次调用时按当时语言求值一次，之后永远返回旧译文。已知踩过的：
  `TerminalSettings::shortcutDisplayName()`（快捷键设置页动作名，已去掉 static）、
  `TerminalSettings::themePresets()` 里的 `"Default"`（色板标题，SettingsDialog 闭包按
  `QCoreApplication::translate("QObject","Default")` 实时重译，其余是品牌名保持原样）。
  新写查找表时，含 `tr()` 就不要 `static`；无法去 static 的，就在调用点/`onRetranslate`
  闭包里对那几个字符串单独重译。
- 构建依赖 `brew install qttools`（lupdate/lrelease）+ `brew install opencc`（生成 `zh_TW`）。
- `LocalizationService` 析构必须 `removeTranslator`；用到它的测试 main 必须建 `QApplication`
  而非 `QCoreApplication`。
