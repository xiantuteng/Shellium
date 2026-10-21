见 [AGENTS.md](AGENTS.md)。

重点：**任何改动界面用户可见文字（`tr()` / `QT_TRANSLATE_NOOP`）的提交，必须同时更新其余 13 种语言的对应译文**，不允许只改英文源。流程见 AGENTS.md 的「多语言」一节。

其他必须遵守的约定（同见 AGENTS.md）：

- 新功能与修 BUG 一律补自动化用例，修 BUG 时先临时还原缺陷，确认新用例会失败。
- 只改少量文案时直接精确编辑 `.ts`，不要跑 `fill_drafts.py`。
- 改 AI 助手（`src/ai/`）前先读 AGENTS.md 的「AI 助手」一节和 `docs/ENGINEERING_NOTES.md` 第二十三节：
  自动执行步数可配置、手动执行会重置计数；提示符可能粘在无换行的输出后面。
