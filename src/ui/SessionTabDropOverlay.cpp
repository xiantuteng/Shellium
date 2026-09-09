#include "ui/SessionTabDropOverlay.h"

#include <algorithm>
#include <cmath>

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

void SessionTabDropOverlay::resetEdge()
{
    if (m_edge == DropEdge::None) {
        return;
    }
    m_edge = DropEdge::None;
    update();
}

void SessionTabDropOverlay::updateForLocalPos(const QPoint &localPosition)
{
    DropEdge edge = DropEdge::None;
    if (rect().contains(localPosition)) {
        const qreal nx =
            static_cast<qreal>(localPosition.x()) / std::max(1, width()) - 0.5;
        const qreal ny =
            static_cast<qreal>(localPosition.y()) / std::max(1, height()) - 0.5;
        if (std::abs(nx) >= std::abs(ny)) {
            edge = nx < 0 ? DropEdge::Left : DropEdge::Right;
        } else {
            edge = ny < 0 ? DropEdge::Top : DropEdge::Bottom;
        }
    }
    if (m_edge == edge) {
        return;
    }
    m_edge = edge;
    update();
}

SessionTabDropOverlay::DropEdge SessionTabDropOverlay::consumeEdge()
{
    const DropEdge edge = m_edge;
    m_edge = DropEdge::None;
    return edge;
}

QRect SessionTabDropOverlay::zoneForEdge() const
{
    const QRect full = rect().adjusted(6, 6, -6, -6);
    switch (m_edge) {
    case DropEdge::None:
        return {};
    case DropEdge::Left:
        return {full.topLeft(), QSize(full.width() / 2, full.height())};
    case DropEdge::Right:
        return {QPoint(full.left() + full.width() / 2, full.top()),
                QSize(full.width() - full.width() / 2, full.height())};
    case DropEdge::Top:
        return {full.topLeft(), QSize(full.width(), full.height() / 2)};
    case DropEdge::Bottom:
        return {QPoint(full.left(), full.top() + full.height() / 2),
                QSize(full.width(), full.height() - full.height() / 2)};
    }
    return {};
}

void SessionTabDropOverlay::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QColor accent = palette().color(QPalette::Highlight);

    // 整块虚线边框始终存在，标明「松手即并入当前页工作空间」。
    QColor outline = accent;
    outline.setAlpha(220);
    QColor faintFill = accent;
    faintFill.setAlpha(26);
    painter.setBrush(faintFill);
    painter.setPen(QPen(outline, 2.0, Qt::DashLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawRoundedRect(QRectF(rect()).adjusted(6.0, 6.0, -6.0, -6.0), 12.0, 12.0);

    // 命中具体半区时，再叠一块实心高亮预告水平 / 垂直的落点方向。
    const QRect zone = zoneForEdge();
    if (!zone.isEmpty()) {
        QColor zoneFill = accent;
        zoneFill.setAlpha(70);
        painter.setBrush(zoneFill);
        painter.setPen(QPen(outline, 2.0));
        painter.drawRoundedRect(QRectF(zone).adjusted(1.5, 1.5, -1.5, -1.5), 8.0, 8.0);
    }
}
