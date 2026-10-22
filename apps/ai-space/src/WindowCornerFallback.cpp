#include "WindowCornerFallback.h"
#include <QEvent>
#include <QGuiApplication>
#include <QLibrary>
#include <QObject>
#include <QPainterPath>
#include <QRegion>
#include <QWidget>

namespace
{
bool hasCompositor()
{
    if (QGuiApplication::platformName() != QStringLiteral("xcb"))
        return true;
    // Resolve Xlib here so native macros never leak into Qt headers and no
    // extra development SDK is needed by packagers.
    static QLibrary library(QStringLiteral("X11"));
    using Open = void *(*)(const char *);
    using Close = int (*)(void *);
    using Screen = int (*)(void *);
    using Atom = unsigned long (*)(void *, const char *, int);
    using Owner = unsigned long (*)(void *, unsigned long);
    const auto open = reinterpret_cast<Open>(library.resolve("XOpenDisplay"));
    const auto close = reinterpret_cast<Close>(library.resolve("XCloseDisplay"));
    const auto screen = reinterpret_cast<Screen>(library.resolve("XDefaultScreen"));
    const auto atom = reinterpret_cast<Atom>(library.resolve("XInternAtom"));
    const auto owner = reinterpret_cast<Owner>(library.resolve("XGetSelectionOwner"));
    if (!open || !close || !screen || !atom || !owner)
        return true;
    void *display = open(nullptr);
    if (!display)
        return true;
    const auto name = QStringLiteral("_NET_WM_CM_S%1").arg(screen(display)).toLatin1();
    const bool active = owner(display, atom(display, name.constData(), 0)) != 0;
    close(display);
    return active;
}

class CornerFallback final : public QObject
{
  public:
    CornerFallback(QWidget *window, int radius)
        : QObject(window), m_window(window), m_radius(radius)
    {
        window->installEventFilter(this);
    }

  protected:
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::Show)
            m_shaped = !hasCompositor();
        if (event->type() == QEvent::Show || event->type() == QEvent::Resize ||
            event->type() == QEvent::WindowStateChange) {
            if (m_shaped && !m_window->isMaximized()) {
                QPainterPath path;
                path.addRoundedRect(m_window->rect(), m_radius, m_radius);
                m_window->setMask(QRegion(path.toFillPolygon().toPolygon()));
            } else
                m_window->clearMask();
        }
        return false;
    }

  private:
    QWidget *m_window;
    int m_radius;
    bool m_shaped = false;
};
} // namespace
void installWindowCornerFallback(QWidget *window, int radius)
{
    new CornerFallback(window, radius);
}
