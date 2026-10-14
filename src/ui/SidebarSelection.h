#pragma once

/**
 * @file SidebarSelection.h
 * @brief 侧边栏列表“当前项”卡片的品牌绿透明度，供各处自绘委托与 QSS 共用。
 *
 * 主机树、端口转发主机树（委托自绘）和效率工具两个列表（QSS）画的是同一种卡片，过去各自
 * 写死 13% / 42%，在浅色侧栏上选中项几乎看不出来，调一处又容易漏掉另外两处。统一放在这里，
 * 改深浅只动这两个数。
 */
namespace ShelliumUi {

/** @brief 选中卡片底色的品牌绿不透明度。 */
inline constexpr float kSidebarSelectionFillAlpha = 0.24F;

/** @brief 选中卡片 1px 描边的品牌绿不透明度。 */
inline constexpr float kSidebarSelectionBorderAlpha = 0.62F;

} // namespace ShelliumUi
