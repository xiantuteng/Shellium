# Shellium 协作约定

## 多语言（i18n）

Shellium 源语言是英文，界面文案通过 Qt Linguist 管理，支持 14 种语言：
`en`(源) `zh_CN` `zh_TW` `fr` `de` `it` `es` `pt_BR` `ro` `ru` `uk` `ja` `ko` `ar`。
详见 `docs/I18N_DESIGN.md`。

**只要改动了界面中用户可见的文字（新增、修改或删除 `tr()` / `QT_TRANSLATE_NOOP` 文案），必须在同一次改动里同步更新其他所有语言的对应译文，不允许只改英文源。** 具体步骤：

1. 刷新 `.ts` 骨架：`cmake --build build --target Shellium_lupdate`
   （会把新的英文源灌进 `i18n/shellium_*.ts` 全部 13 个文件）。
2. 逐语言补/改译文：
   - `zh_TW` 可先用 `python3 scripts/i18n/make_zh_tw.py` 从 `zh_CN` 转换（需 `brew install opencc`）。
   - 其余语言直接翻译，写入 `scripts/i18n/drafts/<lang>.tsv`（`行号\t译文`，字面 `\n`/`\r`，
     保留 `%1..%9` 占位符），再 `python3 scripts/i18n/fill_drafts.py <lang…>` 回填。
   - **只新增/修改了少量文案时，不要跑 `fill_drafts.py`**：它按 `sources.json` 的旧索引回填，
     且会改写已有条目的转义，容易覆盖现成译文。直接在各 `.ts` 里把对应
     `<translation type="unfinished"></translation>` 替换成译文，再用 `git diff i18n` 确认
     只动了这几条。
   - **不要用 Google / DeepL / translate-shell**——直接翻译，注意术语一致和语境。
   - `ar` 用逻辑序 Unicode，不要做视觉反转。
3. 构建并跑 `ctest`，确认 `ShelliumLocalizationTests` 等通过；`lrelease` 会把
   `type="unfinished"` 但有文本的译文一并编入 `.qm`。
4. 新增常驻控件文案时按 `src/i18n/Retranslate.h` 的 `i18n::onRetranslate` 模式接入热切换；
   无 `Q_OBJECT` 的嵌套对话框类必须加 `Q_DECLARE_TR_FUNCTIONS(类名)`。

## 第三方依赖与开源许可

新增、替换或升级会进入安装包的第三方库（含被间接带入的动态库）或内置字体/图标时，必须同步
`resources/licenses/` 的许可证原文与 `OpenSourceNotices::components()` 的组件清单，否则
「关于 → 开源软件」里的声明会漏项。LGPL 组件（Qt、libssh 等）必须保持动态链接。流程见
`resources/licenses/README.md`。

## AI 助手（智能体回路）

改 `src/ai/` 前先读 `docs/ENGINEERING_NOTES.md` 第二十三节。几条容易踩的约束：

- 自动执行步数上限来自 `AiSettings::maxAutoExecutedSteps`（设置页可调，缺省 20，库值越界回落
  缺省），不要再写死常量。用户发新消息**或手动执行一条命令**都要清零计数。
- 命令完成判定靠"提示符回归"（`shellPromptHasReturned()`）。输出末尾没有换行时提示符会粘在
  输出后面（`done[admin@localhost ~]$`），还可能被软换行劈成两行；只做整行匹配会让 AI 永远
  等待。极短提示符（`$ `、`# `）不能做后缀匹配，否则任何以 `$` 结尾的输出都会被误判为结束。
- 风险分级只信 `CommandRiskClassifier` 的本地规则，不采信模型自述；全自动必须经确认框开启。

## 改完必须补测试

新功能与修 BUG 一律补自动化用例；修 BUG 时先临时还原缺陷，验证新用例确实会失败。
GUI 用例用 `QT_QPA_PLATFORM=offscreen` 跑，不要用 `ctest -j` 并行跑 GUI 测试。
