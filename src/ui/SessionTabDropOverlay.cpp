#include "ui/SessionTabDropOverlay.h"

#include <QPainter>
#include <QPaintEvent>

SessionTabDropOverlay::SessionTabDropOverlay(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("SessionTabDropOverlay"));
    // 纯装饰：显示期间也不拦截鼠标，用户的拖放释放仍落到下方的终端视图/侧栏。
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    hide();
}

void SessionTabDropOverlay::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QColor accent = palette().color(QPalette::Highlight);
    QColor fill = accent;
    fill.setAlpha(26);
    painter.setBrush(fill);
    accent.setAlpha(220);
    painter.setPen(QPen(accent, 2.0, Qt::DashLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawRoundedRect(QRectF(rect()).adjusted(6.0, 6.0, -6.0, -6.0), 12.0, 12.0);
}
