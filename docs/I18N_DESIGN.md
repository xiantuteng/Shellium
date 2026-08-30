# Shellium 多语言（i18n）技术方案

## 1. 现状

- GUI 基于 Qt 6.8 Widgets，`qt_add_executable(Shellium ...)`。
- 源码内已有 **1213 处 `tr()`**，源字符串是**简体中文字面量**（例如 `setWindowTitle(tr("设置"))`）。
- 尚无 `.ts` / `.qm` / `QTranslator` / `retranslate` 相关代码；`changeEvent` 只处理 `WindowStateChange`。
- 设置采用即时持久化：`TerminalSettings` 值对象 + `TerminalSettingsService::save()` 原子写 SQLite，通过 `settingsChanged` 广播；对话框只有“关闭”。
- 大量 `QStringLiteral` 用于 `objectName` / 资源路径 / 稳定 id（**不应翻译**），但也有个别用户可见文本用了 `QStringLiteral`（如 `SettingsDialog` 的终端预览文本），需要审计。
- 已有约定（见记忆）：新功能与修 BUG 一律补自动化用例，并临时还原缺陷验证用例真的会失败。

**目标**：源语言保持中文，先补齐 **English (`en`)**，架构上支持任意追加语言（如 `zh_TW`、`ja`）。用户在设置里切换语言，尽量做到**免重启**，无法免重启的部分明确提示。

---

## 2. 技术选型

采用 Qt 原生 **Linguist 工作流**（`.ts` 源 → `lrelease` 编译 `.qm` → `QTranslator` 运行时加载）。理由：与现有 `tr()` 零改造对接、Qt Creator/Linguist 有成熟译者工具、可增量 `lupdate`、支持复数与 `%n`。

不引入 gettext / 自研 JSON 方案（会丢掉 `tr()` 上下文机制与工具链）。

### 语言标识

以 BCP-47 / Qt `QLocale` 名称为准：`zh_CN`（源）、`en`、`zh_TW`、`ja` …。数据库只存这一个字符串。

---

## 3. 源字符串规范化（前置工作）

`lupdate` 只能提取 `tr()` / `QCoreApplication::translate()` / `QT_TR_NOOP` 中的**字符串字面量**。落地前需要一轮审计与整改：

1. **用户可见文本一律进 `tr()`**
   - 审计所有 `QStringLiteral(` / 裸 `QString` / `u"..."_s` 承载的可见文案（按文件：`MainWindow.cpp` 142、`SettingsDialog.cpp` 56、`HostManagerWidget.cpp` 97、`SettingsWidgets.cpp` 75 处 `QStringLiteral` 为重点排查对象）。
   - `objectName`、样式表选择器、资源路径、持久化 id（如 `colorSchemeId`、setting key）**保持 `QStringLiteral`，绝不翻译**——`TerminalThemePreset` 注释已明确 id 不受本地化影响，配色显示名 `name` 才是可见文本。
2. **禁止运行期拼接可见句子**
   - `tr("已连接 ") + host` → `tr("已连接 %1").arg(host)`。
   - 复数：`tr("%n 个文件", "", count)`，配合 `.ts` 的 `numerus` 复数形式。
3. **静态字符串用 `QT_TR_NOOP` 标记，使用处再 `tr()`**（表驱动的菜单项、枚举显示名等）。
4. **消歧义上下文**：同一中文在不同语境译法不同的，加第二参数 `tr("打开", "文件菜单项")`；`lupdate` 会按（class context + 消歧义）分条。
5. **非 `QObject` 处的翻译**：自由函数用 `QCoreApplication::translate("ScopeName", "文本")`。
6. **`.ui` 文件**：当前基本是纯代码构建 UI，若后续引入 `.ui`，`AUTOUIC` 生成的 `retranslateUi()` 天然可用。

产出：一份《可翻译文本审计清单》，整改 PR 独立于加载框架 PR，便于回归。

---

## 4. 运行时架构

### 4.1 目录与资源

```
i18n/
  shellium_zh_CN.ts   # 源语言（可选，主要作占位/校对基准）
  shellium_en.ts
  shellium_zh_TW.ts
  ...
```

编译出的 `shellium_<lang>.qm` 通过 `qt_add_resources` 打进 `:/i18n/`（与字体、品牌资源同样内嵌，避免随包丢失）。
Qt 自身文案（标准按钮 “OK/Cancel”、`QFileDialog`、`QLineEdit` 右键菜单等）用 Qt 官方 `qtbase_<lang>.qm`，从 `QLibraryInfo::path(QLibraryInfo::TranslationsPath)` 加载；macOS/Windows 打包时需把用到的 `qtbase_*.qm` 一并拷入 bundle（`macdeployqt` 默认带部分，需在 `RELEASE_CHECKLIST.md` 增加校验项）。

### 4.2 `LocalizationService`（新增，`src/i18n/`）

单一职责组件，`main()` 创建、生命周期覆盖 `MainWindow`：

```cpp
class LocalizationService : public QObject {
public:
    // 可选语言表：{ "zh_CN", "简体中文" }, { "en", "English" }, ...
    static QVector<LanguageOption> availableLanguages();

    // "system" -> 解析 QLocale::system()，落到最接近的受支持语言，否则回退 zh_CN
    QString resolve(const QString &stored) const;

    // 卸载旧 QTranslator，装载 app + qtbase 两个 QTranslator，
    // 成功后 QApplication 会自动向所有顶层控件投递 QEvent::LanguageChange
    bool apply(const QString &language);

    QString currentLanguage() const;
signals:
    void languageChanged(const QString &language);
};
```

- 持有 3 个 `QTranslator*`（app、qtbase、qtdeclarative 备用），`apply()` 里先 `removeTranslator` 再 `installTranslator`。
- 同时 `QLocale::setDefault(QLocale(language))`，让日期/数字格式跟随。
- RTL：`qApp->setLayoutDirection(QLocale(language).textDirection())`（当前目标语言都是 LTR，先预留）。

### 4.3 启动顺序（`main.cpp`）

```
QApplication app;
setApplicationName/Version/OrganizationName
// 1. 读设置（数据库已在下方初始化——需调整顺序：DB 初始化提前到翻译之前）
database.initialize()
QString lang = loc.resolve(database.setting("appearance/language").value_or("system"));
loc.apply(lang);                 // 必须在任何窗口/对话框构造前
BundledFonts::registerAll();
... Vault / MainWindow ...
```

注意：现有 `main()` 里数据库初始化失败的 `QMessageBox` 用了 `QObject::tr`；把 `loc.apply()` 提到它之前，这些早期错误框也能本地化。

### 4.4 语言切换（免重启）

Qt 在 `installTranslator` 后自动给每个 widget 发 `QEvent::LanguageChange`。每个自建窗口/控件实现：

```cpp
void changeEvent(QEvent *e) override {
    if (e->type() == QEvent::LanguageChange) retranslateUi();
    QWidget::changeEvent(e);   // MainWindow 里注意与既有 WindowStateChange 分支合并
}
void retranslateUi() {
    // 重新 setText/setWindowTitle/setPlaceholderText/setToolTip ...
    // 菜单、QActions、tab 标题、状态栏、表头 also
}
```

改造范围与约定：

- 把每个窗口构造函数里“设置文案”的代码抽成 `retranslateUi()`，构造时调用一次，`LanguageChange` 时再调一次。
- 动态生成的列表项（主机树节点名来自数据库、终端 tab 里的远端标题）**不是** UI 文案，不参与；只有像“未分组”“新建标签页”这类固定标签要重译。
- 已构造但不可见的对话框也会收到事件，实现需幂等。
- **无法热切换的部分**（明确告知用户，切换时弹一次性提示）：
  - 已渲染的终端内容 / 已滚动的历史；
  - 已写入 `ActivityLog` 的历史条目；
  - 少量第三方或系统级字符串。
  可接受策略：语言下拉旁注明“部分界面将在重新打开窗口后完全生效”，或切换后询问是否立即重启。

### 4.5 设置项集成

- `TerminalSettings` 增加 `QString language = QStringLiteral("system");`，`normalized()` 里把未知值回退 `"system"`。
- setting key：`appearance/language`。
- `SettingsDialog` 外观页加一个 `QComboBox`（`objectName` = `languageSelector`），选项来自 `LocalizationService::availableLanguages()`，`currentData` 存语言码。
- `TerminalSettingsService::save()` 落库后广播；`MainWindow` 订阅到 `language` 变化时调用 `LocalizationService::apply()`（服务层不直接依赖 UI 翻译器，解耦）。
- 即时持久化语义不变；切换失败（`.qm` 缺失）回退上一次成功值并提示。

---

## 5. 构建集成

Qt 6.8 提供 `qt_add_translations`（内部封装 `lupdate`/`lrelease`）：

```cmake
qt_add_translations(Shellium
    TS_FILES
        i18n/shellium_en.ts
        i18n/shellium_zh_TW.ts
    RESOURCE_PREFIX "/i18n"
    LUPDATE_OPTIONS -no-obsolete -locations none
)
```

- 生成 `Shellium_lupdate` / `update_translations` 目标：开发者改完源码跑 `cmake --build . --target update_translations` 刷新 `.ts`。
- 构建时自动 `lrelease` 成 `.qm` 并注入资源，无需手写 `.qrc`。
- CI 增加一步：`update_translations` 后 `git diff --exit-code i18n/` 失败即提示“有新文案未提取”（软门禁，或仅告警）。
- 源语言 `zh_CN`：可不建 `.ts`（`tr()` 直接回落字面量），或建一份用于统一校对/修正错别字。

打包校验（补进 `docs/RELEASE_CHECKLIST.md` 与 `MACOS_DMG_RELEASE.md`）：
- `:/i18n/shellium_en.qm` 在包内；
- 需要的 `qtbase_*.qm` 随 `macdeployqt` / windeployqt 落地；
- 启动时 `--lang en` 冒烟截图。

---

## 6. 本地化格式

- 日期/时间：统一走 `QLocale().toString(dt, QLocale::ShortFormat)`，禁止硬编码 `yyyy-MM-dd HH:mm`（审计 `ActivityLog`、SFTP 文件列表 mtime 显示、会话恢复时间戳）。
- 数字/字节大小：`QLocale().formattedDataSize(bytes)` 替代自研 `xx KB` 拼接。
- 文件名排序：`QCollator`（跟随 locale）。
- 终端内 / 远端返回的内容不做任何本地化转换。

---

## 7. 翻译工作流

1. 开发者只写中文 `tr()`。
2. 发版前跑 `update_translations` → `.ts` 更新。
3. 译者用 Qt Linguist 打开 `shellium_en.ts` 翻译，标记 “Finished”。
4. 审校：`lrelease -verbose` 报告未完成条目数；PR 模板要求未完成率为 0（或列出豁免）。
5. **伪本地化**（开发期自测）：生成 `shellium_xx.ts`，把每条译文包成 `⟦…—padding⟧`，跑一遍 UI 找截断/未提取字符串/拼接串。可用脚本 `scripts/pseudo_localize.py` 由 `en.ts` 生成。

---

## 8. 测试方案（遵循“改完必须补测试”）

新增 `tests/LocalizationTest.cpp`（`qt_add_executable(ShelliumLocalizationTests ...)`，注册进 `ctest`）：

| 用例 | 断言 | 还原缺陷验证 |
| --- | --- | --- |
| `resolvesSystemToSupported` | `resolve("system")` 在受支持列表内；未知码回退 `zh_CN` | 把回退分支改成返回原值 → 用例失败 |
| `applyLoadsQmAndEmitsSignal` | `apply("en")` 返回 true 且 `:/i18n/shellium_en.qm` 被装载；`QCoreApplication::translate` 对已知 key 返回英文 | 资源里移除 en.qm → 失败 |
| `missingLanguageRejected` | `apply("xx")` 返回 false，`currentLanguage()` 不变 | 去掉存在性检查 → 失败 |
| `languageChangeRetranslatesWindow` | 构造 `SettingsDialog`，`apply("en")` 后关键按钮 `text()` 变为英文（用一条已翻译样例字符串） | 注释掉该对话框 `retranslateUi()` 调用 → 失败 |
| `settingRoundTrip` | 写 `appearance/language=en` → `TerminalSettings::normalized()` 保留；写 `=xx` → 回落 `system` | normalized 不校验 → 失败 |
| `noBareConcatInVisibleStrings`（静态扫描，可选） | 脚本 grep `tr("[^"]*") *\+` 命中为 0 | 引入一处拼接 → 失败 |

补充：为 `SettingsUiTest` 增加“存在语言下拉且选项非空”断言。

CI：`ctest` 跑 `ShelliumLocalizationTests`；另跑一次 `lrelease` 确认 `.ts` 可编译。

---

## 9. 分期实施

| 阶段 | 内容 | 交付 |
| --- | --- | --- |
| P0 | 源字符串审计 + 整改（`QStringLiteral`→`tr()`、消拼接、加上下文） | 审计清单 + 整改 PR，无行为变化 |
| P1 | `LocalizationService` + `main.cpp` 接线 + CMake `qt_add_translations` + `en.ts` 空翻译 + `LocalizationTest` | 可用 `--lang en` 启动（界面仍中文，因译文空） |
| P2 | `retranslateUi()` 改造（`MainWindow` → 各对话框 → 各 Widget），`LanguageChange` 热切换 | 免重启切换 |
| P3 | 设置页语言下拉 + 持久化 + 服务广播接线 | 用户可切换 |
| P4 | English 全量翻译 + 伪本地化过一遍 + 打包/发版清单更新 | 首个真正双语版本 |
| P5（可选） | `zh_TW` / `ja`，格式本地化（`QLocale` 日期数字） | 追加语言 |

命令行 `--lang <code>` 覆盖参数在 P1 就加入，供测试与截图使用（优先级：命令行 > 数据库 > system）。

---

## 10. 风险与对策

- **热切换遗漏**：某些控件没实现 `retranslateUi()` → 伪本地化 + P2 逐窗口 checklist；对话框优先“重新打开生效”。
- **`.qm` 丢包**：内嵌进 Qt 资源（不是外部文件）从根本上规避；发版清单加校验。
- **Qt 自带译文缺失**：显式随包携带 `qtbase_*.qm`。
- **译文过长撑破布局**：`SettingsDialog` 用的是“说明+控件”弹性行，多数可自适应；伪本地化提前暴露固定宽度处。
- **持久化 id 被误翻**：审计阶段建立白名单（所有 setting key、`colorSchemeId`、`HostRecord.id` 等），code review 关注 `.ts` 里不该出现的 id。
- **`main()` 顺序调整**：数据库初始化需提前到 `loc.apply()` 之前，注意 UI 预览模式分支同步调整。

---

## 11. 实施进度

### P1 已完成（基础设施 + CLI + 测试）

- `TerminalSettings::language`（默认 `"system"`），`normalized()` 用新增的 `normalizeLanguageTag()` 收敛格式，`operator==` 纳入比较，服务 `load()/save()` 读写 `appearance/language`。
- `src/i18n/LocalizationService.{h,cpp}`：`availableLanguages()` / `supportedLanguages()`（当前 `zh_CN`、`en`）/ `resolve()` / `apply()` / `languageChanged` 信号。析构显式 `removeTranslator`，否则退出崩溃。
- `main.cpp`：`QCommandLineParser` 解析 `--lang`，优先级 命令行 > `appearance/language` > 系统；数据库打开前先按命令行/系统装载一次，打开后按存储偏好再装载一次。
- CMake：`find_package(... LinguistTools)` + `qt_add_translations(Shellium ...)`，`.qm` 注入资源前缀 `/i18n`；`i18n/shellium_en.ts` 已由 `lupdate` 提取（1127 条，多数 `unfinished`，含 4 条种子翻译供测试）。刷新命令：`cmake --build build --target Shellium_lupdate` 或聚合目标 `update_translations`。
- `tests/LocalizationTest.cpp` + `ShelliumLocalizationTests` 目标（自带 `qt_add_translations` 以便断言真实 `.qm` 装载）。8 个用例全绿；已用两处缺陷还原验证 `settingsNormalizesLanguageTag`、`applyLoadsCatalogAndEmitsSignal` 确实会失败。
- 构建环境：已 `brew install qttools`（6.11.1）。
- 已知无关问题：`SHELLIUM_UI_PREVIEW` 配合 `-platform offscreen` 在本机启动即崩（MacWindowChrome 向不存在的 NSWindow 发消息），与本改动无关，预览截图走真实 cocoa 平台。

### P2 已完成（全量免重启热切换）

改语言后**整个界面立即切换**，无需重启。

- **通用助手** `src/i18n/Retranslate.h`（纯头文件、无 moc）：`i18n::onRetranslate(owner, [&]{ ...重设 tr() 文案... })`。闭包立即执行一次（相当于初始化），之后每次装/卸翻译器（`QEvent::LanguageChange`，`installTranslator` 会向 `qApp` 投递）自动重跑；`owner` 销毁时自动注销。常驻控件用它替代构造函数里一次性 `setText(tr(...))`。
- **MainWindow**：菜单/动作用 `registerTranslatable(x, QT_TRANSLATE_NOOP("MainWindow", ...))` 登记，`changeEvent(LanguageChange)` → `retranslateUi()`（含侧栏标题、主机数、空态提示）。
- **常驻控件已接入 `onRetranslate`**：`HostManagerWidget`（标题行按钮/搜索框 + `refresh()` 重建树含"未分组"分区头）、`PortForwardManagerWidget`、`ProductivityWidget`（三个 Tab + 按钮 + 占位符）、`AiAssistantWidget`（新会话/输入占位/免责声明/上下文标签）、`TerminalPane`（编码/文件管理器提示 + `refreshHeaderLabel()`）、`RemoteFileManagerWidget`（列头 ×2 + 工具栏提示 + 续传勾选）、`SettingsDialog`（导航列表 + 页脚 + 窗口/侧栏标题 + 语言卡）。
- **按需对话框**（`ConnectionDialog`、`VaultDialog`、`SshKeyManagerDialog` 等）不接入：下次打开即以新语言重建。
- **`SettingsDialog` 正文也热切换**：`SettingsCard` / `SegmentedControl` / `ThemeSwatchButton` 新增「文案提供器」重载（`addRow([this]{ return tr("..."); }, field)`、`SettingsCard(provider, parent)`、`addSegment(provider, data)`、`setSubtitle()`），内部各自 `i18n::onRetranslate`；`buildXxxPage()` 的 `tr()` 实参由脚本统一包成 `[this]{ return tr(...); }`。设置窗口开着时切语言，卡片标题/行标签/说明/分段控件立即变。
- **无 `Q_OBJECT` 的嵌套对话框类**（`HostRecordDialog`、`HostTagDialog`、`HostPickerDialog`、`HostChainDialog`、`TagColorSwatch`、`PortForwardRuleDialog`、`PortForwardRuleRow`、`HostTreeComboBox`）加了 `Q_DECLARE_TR_FUNCTIONS(类名)`：否则它们的 `tr()` 运行时用基类上下文（`QDialog`…），而 lupdate 按类名提取，上下文不匹配 → 这些「新建主机」等对话框会退回英文源。这是翻转源语言后才暴露的隐患。
- **固有不切换**（内容而非界面文案，不再提示重启）：已经滚过的终端输出、历史 `ActivityLog` 条目、少数瞬时状态文案（连接中/失败等，随下一次状态变化转新语言）。
- `MainWindow::applyLanguageSetting` 的模态"需重启"弹框已改为一条 4 秒 transient 提示。

### P3 已完成（语言下拉 + 接线）

- `MainWindow` 构造函数新增 `LocalizationService *localization`（默认 `nullptr`，单测可省）；`main.cpp` 传入 `&localization`。
- `SettingsDialog` 外观页新增 `languageSelector` 下拉（`QComboBox`，`currentData` 存语言码），选项来自 `LocalizationService::availableLanguages()`；`editedSettings()` / `populateControls()` 读写 `settings.language`，沿用即时持久化。
- `MainWindow` 的 `settingsChanged` 处理器调用 `applyLanguageSetting()`：`resolve()` 后若与当前语言不同则 `apply()`，触发 `LanguageChange` 热重译，并首次弹一次 `QMessageBox::information` 提示部分界面需重开/重启。
- 命令行 `--lang` 仍然优先于设置。

### P2/P3 测试

- `ShelliumMainWindowShellTests::languageChangeRetranslatesMenuBar`：切到 `en` 后“连接”菜单标题与首个动作文案变英文，切回 `zh_CN` 恢复；该目标已加 `qt_add_translations` 以内嵌 `.qm`。`i18n/shellium_en.ts` 里为 `MainWindow` 上下文种入 `连接→Connection`、`连接服务器→Connect to Server` 供断言。
- `ShelliumSettingsUiTests::languageSelectorPersistsAndRoundTrips`：下拉存在、首项为 `system`、选 `en` 即时写入服务、重开设置页回显。
- 均已用缺陷还原验证会失败。

### 源语言已切换为英文（English-first）

原方案源语言是简体中文，现已翻转为 **English 为源语言**，简体中文改为翻译目录。

- 全部约 1130 处 `tr()` / `QObject::tr()` / `QCoreApplication::translate()` / `QT_TRANSLATE_NOOP()` 的中文字面量，经 `scripts/i18n/flip_source_language.py`（解析调用、按 `scripts/i18n/zh_to_en_map.json` 映射、重写文本参数、处理相邻字面量拼接）批量改写为英文；ID、setting key、`objectName`、样式选择器等非文案不受影响。
- `i18n/shellium_en.ts` 删除；新增 `i18n/shellium_zh_CN.ts`（`sourcelanguage="en"`），由 `lupdate` 提取英文源后按反向映射回填中文译文（1129 条 finished）。
- `LocalizationService`：`sourceLanguage()` → `"en"`；`supportedLanguages()` → `{"en","zh_CN"}`；`availableLanguages()` 顺序改为 system / English / 简体中文。`resolve("system")` 在非中文系统上落到 `en`，中文系统落到 `zh_CN` —— 即“英文优先，中文用户仍自动获得中文界面”。
- `CMakeLists.txt` 三处 `qt_add_translations` 的 `TS_FILES` 改为 `i18n/shellium_zh_CN.ts`。
- 测试：`ShelliumLocalizationTests` 改为用 `zh_CN` 验证 `.qm` 装载；`languageChangeRetranslatesMenuBar` 断言英文源 ↔ 中文译文往返。硬编码了 UI 文案的既有用例（`SshCommandParserTest`、`AiAssistantWidgetTest`、`SftpRecursiveTest`、`SessionRecoveryTest`、`HostManagerWidgetTest`、`ProductivityTest` 等）中，比对 widget 文本的断言改为英文，测试自造的 fixture 文案一并英化。
- 代码注释仍为中文（面向现有开发团队，非用户可见）——需要时另行处理。

刷新 `.ts`：`cmake --build build --target Shellium_lupdate`（现在提取英文源、回填中文译文由译者在 Linguist 完成）。

### 待办

- **P0/P4**：仍以 `QStringLiteral` 承载的用户可见文案审计整改（进 `tr()`）、消运行期拼接、中文译文人工校对、伪本地化。
- **发版清单**：macOS/Windows 打包需携带 **`qtbase_zh_CN.qm`**（英文是 Qt 源语言无需 qm；中文界面下 Qt 自带控件文案依赖它）。
- **P2 扩展（可选）**：更多常驻控件（`HostManagerWidget` 侧栏、`SessionTabBar`、`AiAssistantWidget`、终端空态页）热切换，按 `registerTranslatable` 模式逐个加。
- **P5**：`zh_TW` / `ja`，`QLocale` 日期数字格式本地化。
