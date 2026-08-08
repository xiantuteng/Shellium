#include "ui/MacWindowChrome.h"

#include <QWidget>

#import <AppKit/AppKit.h>

bool enableMacWindowChrome(QWidget *window)
{
    if (window == nullptr || !window->isWindow()) {
        return false;
    }

    // 在 Qt 6 的 Cocoa 平台插件中，顶层 QWidget::winId() 对应承载内容的 NSView。窗口在
    // 第一次 showEvent 前可能尚未附着，因此所有桥接对象都必须逐层判空并允许调用方重试。
    NSView *nativeView = reinterpret_cast<NSView *>(window->winId());
    if (nativeView == nil) {
        return false;
    }
    NSWindow *nativeWindow = nativeView.window;
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
