#pragma once

/**
 * @brief 终端内查找的一步操作。
 *
 * 终端输出越靠下越新，查找因此以"从最新输出往回找"为主方向：Refine 在输入框内容变化时
 * 使用，停留在当前命中（若仍匹配）或落到最靠下的命中；Previous 向上找更早的输出；Next
 * 向下找更新的输出。两端都会环回。
 */
enum class TerminalSearchStep {
    Refine,
    Previous,
    Next
};

/**
 * @brief 一次终端查找的结果，供查找栏显示"第几个 / 共几个"。
 *
 * current 从 1 开始计数；没有命中时 current 与 total 都为 0。计数超过上限时停止统计并
 * 置 totalCapped，避免在超长滚动历史里搜单个字符时每敲一键都遍历全部文本。
 */
struct TerminalSearchResult {
    int current = 0;
    int total = 0;
    bool totalCapped = false;
};
