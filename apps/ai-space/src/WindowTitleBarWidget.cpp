#include "WindowTitleBarWidget.h"
#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>
#include <QStyle>

namespace AiSpace
{
namespace
{
class WindowButton final : public QToolButton
{
  public:
    enum Kind { Minimize, Maximize, Close };
    explicit WindowButton(Kind kind, QWidget *parent) : QToolButton(parent), m_kind(kind)
    {
        setFixedSize(kind == Close ? 44 : 42, WindowMetrics::TitleBarHeight);
        setAutoRaise(true);
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        const QString name = kind == Minimize   ? QStringLiteral("最小化")
                             : kind == Maximize ? QStringLiteral("最大化")
                                                : QStringLiteral("关闭AI空间");
        setToolTip(name);
        setAccessibleName(name);
    }
    bool light = false, active = true, maximized = false;

  protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const bool hover = underMouse() || hasFocus();
        if (hover || isDown()) {
            QColor fill = m_kind == Close ? QColor(isDown() ? "#e11d48" : "#f43f5e")
                                          : QColor(148, 163, 184, isDown() ? 42 : 26);
            p.setPen(Qt::NoPen);
            p.setBrush(fill);
            QPainterPath path;
            if (m_kind == Close && !maximized) {
                const qreal r = WindowMetrics::CornerRadius;
                path.moveTo(0, 0);
                path.lineTo(width() - r, 0);
                path.quadTo(width(), 0, width(), r);
                path.lineTo(width(), height());
                path.lineTo(0, height());
                path.closeSubpath();
            } else
                path.addRect(rect());
            p.drawPath(path);
        }
        QColor ink =
            (m_kind == Close && hover) ? QColor(Qt::white) : QColor(light ? "#334155" : "#e2e8f0");
        ink.setAlphaF(active || hover ? .96 : .55);
        p.setPen(QPen(ink, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        const QPointF c = QRectF(rect()).center();
        if (m_kind == Minimize)
            p.drawLine(c + QPointF(-5, 3.5), c + QPointF(5, 3.5));
        else if (m_kind == Close) {
            p.drawLine(c + QPointF(-4.5, -4.5), c + QPointF(4.5, 4.5));
            p.drawLine(c + QPointF(4.5, -4.5), c + QPointF(-4.5, 4.5));
        } else if (!maximized)
            p.drawRoundedRect(QRectF(c + QPointF(-4.8, -4.8), QSizeF(9.6, 9.6)), 1.4, 1.4);
        else {
            p.drawRoundedRect(QRectF(c + QPointF(-2.8, -5.2), QSizeF(7.8, 7.8)), 1.3, 1.3);
            QPainterPath back;
            back.moveTo(c + QPointF(-5, -2.2));
            back.lineTo(c + QPointF(-5, 3.7));
            back.quadTo(c + QPointF(-5, 5), c + QPointF(-3.7, 5));
            back.lineTo(c + QPointF(2.2, 5));
            p.drawPath(back);
        }
    }

  private:
    Kind m_kind;
};
} // namespace
WindowTitleBarWidget::WindowTitleBarWidget(QWidget *parent) : QFrame(parent)
{
    setObjectName(QStringLiteral("smartActionRail"));
    setFixedHeight(WindowMetrics::TitleBarHeight);
    setMouseTracking(true);
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(12, 0, 0, 0);
    row->setSpacing(6);
    auto *icon = new QLabel(this);
    icon->setPixmap(qApp->windowIcon().pixmap(20, 20));
    icon->setFixedSize(20, 20);
    icon->setAttribute(Qt::WA_TransparentForMouseEvents);
    row->addWidget(icon);
    m_title = new QLabel(QStringLiteral("AI空间"), this);
    m_title->setObjectName(QStringLiteral("smartBrandTitle"));
    m_title->setAttribute(Qt::WA_TransparentForMouseEvents);
    row->addWidget(m_title);
    row->addSpacing(8);
    m_tools = new QHBoxLayout;
    m_tools->setContentsMargins(0, 0, 0, 0);
    m_tools->setSpacing(3);
    row->addLayout(m_tools);
    row->addStretch(1);
    m_minimize = new WindowButton(WindowButton::Minimize, this);
    m_maximize = new WindowButton(WindowButton::Maximize, this);
    m_close = new WindowButton(WindowButton::Close, this);
    m_minimize->setObjectName("smartMinimizeButton");
    m_maximize->setObjectName("smartMaximizeButton");
    m_close->setObjectName("smartWindowCloseButton");
    row->addWidget(m_minimize);
    row->addWidget(m_maximize);
    row->addWidget(m_close);
}
void WindowTitleBarWidget::attachHostUtilityButtons(QWidget *pin, QWidget *theme, QWidget *settings)
{
    for (auto *button : {pin, theme, settings}) {
        if (!button)
            continue;
        button->setFixedHeight(26);
        m_tools->addWidget(button);
    }
}
void WindowTitleBarWidget::setAppearance(bool light, bool active, bool maximized)
{
    for (auto *button : {m_minimize, m_maximize, m_close}) {
        auto *b = static_cast<WindowButton *>(button);
        if (b->light == light && b->active == active && b->maximized == maximized)
            continue;
        b->light = light;
        b->active = active;
        b->maximized = maximized;
        b->update();
    }
    m_maximize->setToolTip(maximized ? QStringLiteral("还原") : QStringLiteral("最大化"));
    m_maximize->setAccessibleName(m_maximize->toolTip());
}
} // namespace AiSpace
