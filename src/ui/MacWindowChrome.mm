#include "ui/MacWindowChrome.h"

#include <QWidget>

#import <AppKit/AppKit.h>

namespace {

/**
 * @brief 从已经创建平台句柄的 Qt 顶层控件解析 Cocoa 窗口。
 * @param window 候选 Qt 顶层窗口。
 * @return 成功桥接时返回 NSWindow；参数无效、句柄未创建或尚未附着窗口时返回 nil。
 *
 * QWidget::winId() 在 Qt Cocoa 平台插件中对应 NSView，而不是 NSWindow。所有原生标题栏
 * 操作都走同一处严格判空，可避免窗口首次显示的短暂阶段访问尚未装配完成的 Cocoa 对象。
 */
NSWindow *nativeWindowFor(QWidget *window)
{
    if (window == nullptr || !window->isWindow()) {
        return nil;
    }

    NSView *nativeView = reinterpret_cast<NSView *>(window->winId());
    if (nativeView == nil) {
        return nil;
    }
    return nativeView.window;
}

} // namespace

bool enableMacWindowChrome(QWidget *window)
{
    NSWindow *nativeWindow = nativeWindowFor(window);
    if (nativeWindow == nil) {
        return false;
    }

    nativeWindow.titleVisibility = NSWindowTitleHidden;
    nativeWindow.titlebarAppearsTransparent = YES;
    nativeWindow.styleMask |= NSWindowStyleMaskFullSizeContentView;

    if (@available(macOS 11.0, *)) {
        // Big Sur 以后标题区默认可能绘制一条分隔线；自定义标题区已经拥有自己的层级颜色，
        // 再保留系统分隔线会产生“双边框”，因此只关闭装饰线，不影响窗口按钮功能。
        nativeWindow.titlebarSeparatorStyle = NSTitlebarSeparatorStyleNone;
    }
    return true;
}

void activateMacApplication()
{
    [NSApp activateIgnoringOtherApps:YES];
}

bool beginMacWindowDrag(QWidget *window)
{
    NSWindow *nativeWindow = nativeWindowFor(window);
    NSEvent *currentEvent = NSApp.currentEvent;
    if (nativeWindow == nil || currentEvent == nil) {
        return false;
    }

    const NSEventType eventType = currentEvent.type;
    if ((eventType != NSEventTypeLeftMouseDown && eventType != NSEventTypeLeftMouseDragged)
        || currentEvent.window != nativeWindow) {
        // performWindowDragWithEvent: 只能消费属于目标窗口的左键按下/拖动事件。严格校验可
        // 防止通过快捷键或合成事件误用上一个全局 NSEvent，导致窗口无预期跳动。
        return false;
    }

    [nativeWindow performWindowDragWithEvent:currentEvent];
    return true;
}

bool toggleMacWindowZoom(QWidget *window)
{
    NSWindow *nativeWindow = nativeWindowFor(window);
    if (nativeWindow == nil) {
        return false;
    }

    // zoom: 会在标准缩放尺寸与用户先前的窗口尺寸之间切换；直接复用 AppKit 语义可以
    // 正确避让 Dock、菜单栏和刘海安全区，也不会破坏下一次恢复窗口时所需的旧 frame。
    [nativeWindow zoom:nil];
    return true;
}

void alignMacWindowButtons(QWidget *window, const qreal titleBarHeight)
{
    NSWindow *nativeWindow = nativeWindowFor(window);
    NSView *contentView = nativeWindow.contentView;
    if (nativeWindow == nil || contentView == nil || titleBarHeight <= 0.0) {
        return;
    }

    // Qt 逻辑像素与 AppKit point 在高分屏上代表同一布局单位。先将全尺寸 contentView 的
    // 顶边换算到窗口坐标，再向下偏移半个自定义标题栏高度，得到期望的按钮中心位置。
    [contentView layoutSubtreeIfNeeded];
    const NSRect contentBoundsInWindow = [contentView convertRect:contentView.bounds toView:nil];
    const CGFloat desiredCenterY = NSMaxY(contentBoundsInWindow)
        - static_cast<CGFloat>(titleBarHeight) / 2.0;

    const NSWindowButton buttonKinds[] = {
        NSWindowCloseButton,
        NSWindowMiniaturizeButton,
        NSWindowZoomButton
    };
    for (const NSWindowButton buttonKind : buttonKinds) {
        NSButton *button = [nativeWindow standardWindowButton:buttonKind];
        NSView *buttonContainer = button.superview;
        if (button == nil || buttonContainer == nil) {
            // 无标题按钮的工具窗口或正在切换全屏空间时可能暂时拿不到容器；跳过该按钮即可，
            // 调用方会在下一次 showEvent/WindowStateChange 后再次进行幂等对齐。
            continue;
        }

        NSRect buttonFrameInWindow = [buttonContainer convertRect:button.frame toView:nil];
        buttonFrameInWindow.origin.y = desiredCenterY - NSHeight(buttonFrameInWindow) / 2.0;
        const NSRect adjustedFrameInContainer = [buttonContainer convertRect:buttonFrameInWindow
                                                                     fromView:nil];

        // 仅覆盖纵坐标，系统决定的按钮大小、横向间距和从窗口左边缘的安全距离必须保留。
        NSRect localFrame = button.frame;
        localFrame.origin.y = adjustedFrameInContainer.origin.y;
        button.frame = localFrame;
    }
}
