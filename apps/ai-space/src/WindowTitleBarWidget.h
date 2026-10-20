#pragma once
#include <QFrame>
class QToolButton;
class QLabel;
class QHBoxLayout;
namespace AiSpace {
namespace WindowMetrics {
constexpr int TitleBarHeight = 34;
constexpr int SearchHeaderHeight = 48;
constexpr int BottomStatusHeight = 36;
constexpr qreal CornerRadius = 10.0;
}
// Event-driven client decoration: native button accessibility and keyboard
// interaction, with vector glyphs that remain sharp at fractional scaling.
class WindowTitleBarWidget : public QFrame {
    Q_OBJECT
public:
    explicit WindowTitleBarWidget(QWidget *parent = nullptr);
    void attachHostUtilityButtons(QWidget *pin, QWidget *theme, QWidget *settings);
    void setAppearance(bool light, bool active, bool maximized);
    QToolButton *minimizeButton() const { return m_minimize; }
    QToolButton *maximizeButton() const { return m_maximize; }
    QToolButton *closeButton() const { return m_close; }
private:
    QLabel *m_title = nullptr;
    QHBoxLayout *m_tools = nullptr;
    QToolButton *m_minimize = nullptr;
    QToolButton *m_maximize = nullptr;
    QToolButton *m_close = nullptr;
};
}
