#include "UiMessages.h"
#include "SpaceFormats.h"
#include "WindowCornerFallback.h"
#include "SpacePresentation.h"
using namespace AiSpaceUi;
#include <QPaintEvent>
#include "BackgroundTask.h"
#include <QDBusPendingCall>
#include "SmartSpaceWidget.h"
#include "AppConfig.h"
#include "WindowTitleBarWidget.h"
#include "KnowledgeManagerDialog.h"
#include "KnowledgeFailuresDialog.h"
#include "KnowledgeJobController.h"
#include <QProcessEnvironment>

#include "DesktopItem.h"
#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QCursor>
#include <QDateTime>
#include <QDebug>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDesktopServices>
#include <QDrag>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFrame>
#include <QElapsedTimer>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHash>
#include <QHelpEvent>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLinearGradient>
#include <QListView>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QPointer>
#include <QRegularExpression>
#include <QRegion>
#include <QSaveFile>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QSettings>
#include <QSet>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QFocusEvent>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QTabWidget>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>
#include <QtEndian>
#include <QtDBus/QDBusInterface>
#include <QtDBus/QDBusConnection>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <signal.h>
#include <unistd.h>
#include <zlib.h>
#if defined(__GLIBC__)
#include <malloc.h>
#endif

class CompactKnowledgeStatus : public QLabel
{
  public:
    explicit CompactKnowledgeStatus(QWidget *parent) : QLabel(parent) {}

  protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setPen(palette().color(QPalette::WindowText));
        const QString brief = text().simplified();
        painter.drawText(contentsRect(), Qt::AlignVCenter | Qt::AlignLeft,
                         fontMetrics().elidedText(brief + QStringLiteral(" · 详情 ›"),
                                                  Qt::ElideRight, contentsRect().width()));
    }
};

namespace
{

// UKUI's native message helper ignores our surface styling but inherits its
// light foreground. Keep confirmations inside a regular, explicitly styled
// QDialog so the window and its text always use the same color scheme.
constexpr int kDefaultWidth = 980;
constexpr int kDefaultHeight = 620;
// Rich result cards use a few lightweight child widgets each. Build them in
// explicit pages so the complete index remains searchable without creating
// thousands of QWidget objects at once.
constexpr int kResultsPageSize = 100;
constexpr int kInitialResultsPageSize = 32;
constexpr int kMaxWatchedDirectories = 512;
constexpr int kMinResultDensity = -5;
constexpr int kMaxResultDensity = 2;
constexpr int kResultDensityLevels = 8;
constexpr int kExactNameScore = 1200;
constexpr int kExactContentScore = 700;
constexpr int kExactPathScore = 350;
constexpr int kTermNameScore = 180;
constexpr int kTermContentScore = 70;
constexpr int kTermPathScore = 35;
constexpr int kYearTermBonus = 120;

class AspectRatioSizeGrip final : public QWidget
{
  public:
    AspectRatioSizeGrip(QWidget *target, QWidget *parent) : QWidget(parent), m_target(target)
    {
        setObjectName(QStringLiteral("smartSpaceResizeGrip"));
        setFixedSize(22, 22);
        setCursor(Qt::SizeFDiagCursor);
        setMouseTracking(true);
    }

  protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || !m_target)
            return QWidget::mousePressEvent(event);
        m_dragging = true;
        m_startGlobal = event->globalPos();
        m_startSize = m_target->size();
        m_startPosition = m_target->pos();
        m_ratio =
            qMax(0.1, m_startSize.width() / static_cast<qreal>(qMax(1, m_startSize.height())));
        grabMouse();
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_dragging || !m_target)
            return QWidget::mouseMoveEvent(event);
        const QPoint delta = event->globalPos() - m_startGlobal;
        const qreal horizontal = qAbs(delta.x()) / static_cast<qreal>(qMax(1, m_startSize.width()));
        const qreal vertical = qAbs(delta.y()) / static_cast<qreal>(qMax(1, m_startSize.height()));
        int width = horizontal >= vertical ? m_startSize.width() + delta.x()
                                           : qRound((m_startSize.height() + delta.y()) * m_ratio);

        const QSize minimum = m_target->minimumSize();
        int minimumWidth = qMax(minimum.width(), qCeil(minimum.height() * m_ratio));
        int maximumWidth = QWIDGETSIZE_MAX;
        if (QWidget *container = m_target->parentWidget()) {
            const QPoint origin =
                m_target->isWindow() ? container->mapFromGlobal(m_startPosition) : m_startPosition;
            const int availableWidth = container->width() - origin.x();
            const int availableHeight = container->height() - origin.y();
            maximumWidth = qMin(availableWidth, qFloor(availableHeight * m_ratio));
        }
        maximumWidth = qMax(minimumWidth, maximumWidth);
        width = qBound(minimumWidth, width, maximumWidth);
        m_target->resize(width, qRound(width / m_ratio));
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && m_dragging) {
            m_dragging = false;
            releaseMouse();
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QColor line = palette().color(QPalette::Text);
        line.setAlpha(120);
        QPen pen(line, 1.5, Qt::SolidLine, Qt::RoundCap);
        painter.setPen(pen);
        painter.drawLine(width() - 5, height() - 12, width() - 12, height() - 5);
        painter.drawLine(width() - 5, height() - 7, width() - 7, height() - 5);
    }

  private:
    QPoint m_startPosition;
    QWidget *m_target = nullptr;
    QPoint m_startGlobal;
    QSize m_startSize;
    qreal m_ratio = 1.0;
    bool m_dragging = false;
};

QIcon smartMenuIcon(const QString &name)
{
    return QIcon(QStringLiteral(":/smart-space/menu-%1.png").arg(name));
}

// Observe delivery to child controls as well as the frameless surface. An
// edge cursor belongs to the surface; it must not remain inherited when a
// child consumes mouse movement. This observer never consumes input or polls.
class WindowPointerObserver final : public QObject
{
  public:
    WindowPointerObserver(QWidget *window, std::function<void(QPoint)> moved)
        : QObject(window), m_window(window), m_moved(std::move(moved))
    {
        qApp->installEventFilter(this);
    }

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        const auto type = event->type();
        if (type != QEvent::Enter && type != QEvent::MouseMove && type != QEvent::Leave)
            return false;
        auto *widget = qobject_cast<QWidget *>(watched);
        if (!widget || widget->window() != m_window)
            return false;
        if (type == QEvent::Leave && widget != m_window)
            return false;
        const QPoint global = type == QEvent::MouseMove
                                  ? static_cast<QMouseEvent *>(event)->globalPos()
                                  : QCursor::pos();
        m_moved(m_window->mapFromGlobal(global));
        return false;
    }

  private:
    QWidget *m_window;
    std::function<void(QPoint)> m_moved;
};

void execMenuAt(QMenu &menu, QWidget *anchor)
{
    if (!anchor) {
        menu.exec(QCursor::pos());
        return;
    }
    menu.ensurePolished();
    menu.adjustSize();
    const QRect button(anchor->mapToGlobal(QPoint()), anchor->size());
    // Footer actions open upward, aligned to the button's right edge.
    QPoint pos(button.right() + 1 - menu.width(), button.top() - menu.height() - 6);
    QScreen *screen = QGuiApplication::screenAt(button.center());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (screen) {
        const QRect avail = screen->availableGeometry().adjusted(4, 4, -4, -4);
        if (pos.y() < avail.top() && button.bottom() + 6 + menu.height() <= avail.bottom())
            pos.setY(button.bottom() + 7);
        pos.setX(
            qBound(avail.left(), pos.x(), qMax(avail.left(), avail.right() + 1 - menu.width())));
        pos.setY(
            qBound(avail.top(), pos.y(), qMax(avail.top(), avail.bottom() + 1 - menu.height())));
    }
    menu.exec(pos);
}

QPixmap brandBadgePixmap(int size)
{
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    const int px = qMax(16, qRound(size * dpr));
    QPixmap pixmap(px, px);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF rect(1.0, 1.0, size - 2.0, size - 2.0);
    QLinearGradient grad(rect.topLeft(), rect.bottomRight());
    grad.setColorAt(0.0, QColor(QStringLiteral("#2563eb")));
    grad.setColorAt(0.55, QColor(QStringLiteral("#4f46e5")));
    grad.setColorAt(1.0, QColor(QStringLiteral("#6366f1")));
    painter.setPen(Qt::NoPen);
    painter.setBrush(grad);
    painter.drawRoundedRect(rect, size * 0.28, size * 0.28);

    const QPointF c = rect.center();
    const qreal r = size * 0.30;
    QPainterPath star;
    star.moveTo(c.x(), c.y() - r);
    star.cubicTo(c.x() + r * 0.14, c.y() - r * 0.14, c.x() + r * 0.14, c.y() - r * 0.14, c.x() + r,
                 c.y());
    star.cubicTo(c.x() + r * 0.14, c.y() + r * 0.14, c.x() + r * 0.14, c.y() + r * 0.14, c.x(),
                 c.y() + r);
    star.cubicTo(c.x() - r * 0.14, c.y() + r * 0.14, c.x() - r * 0.14, c.y() + r * 0.14, c.x() - r,
                 c.y());
    star.cubicTo(c.x() - r * 0.14, c.y() - r * 0.14, c.x() - r * 0.14, c.y() - r * 0.14, c.x(),
                 c.y() - r);
    star.closeSubpath();
    painter.fillPath(star, Qt::white);
    painter.setBrush(QColor(255, 255, 255, 220));
    painter.drawEllipse(QPointF(c.x() + r * 0.58, c.y() - r * 0.58), size * 0.055, size * 0.055);
    return pixmap;
}

QPixmap modernRailGlyph(const QString &key, const QColor &color)
{
    QPixmap pixmap(48, 48);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(color, 3.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    if (key == QLatin1String("pin")) {
        QPainterPath pin;
        pin.moveTo(17, 10);
        pin.lineTo(31, 10);
        pin.lineTo(29, 18);
        pin.lineTo(34, 24);
        pin.lineTo(14, 24);
        pin.lineTo(19, 18);
        pin.closeSubpath();
        painter.fillPath(pin, color);
        painter.drawLine(QPointF(24, 24), QPointF(24, 39));
        painter.drawLine(QPointF(20, 39), QPointF(24, 34));
        painter.drawLine(QPointF(28, 39), QPointF(24, 34));
    } else if (key == QLatin1String("hide")) {
        painter.drawLine(QPointF(12, 12), QPointF(12, 36));
        QPainterPath arrow;
        arrow.moveTo(31, 13);
        arrow.lineTo(20, 24);
        arrow.lineTo(31, 35);
        painter.drawPath(arrow);
    } else if (key == QLatin1String("database")) {
        painter.drawEllipse(QRectF(12, 9, 24, 9));
        painter.drawArc(QRectF(12, 17, 24, 9), 180 * 16, 180 * 16);
        painter.drawArc(QRectF(12, 25, 24, 9), 180 * 16, 180 * 16);
        painter.drawArc(QRectF(12, 31, 24, 8), 180 * 16, 180 * 16);
        painter.drawLine(QPointF(12, 13.5), QPointF(12, 35));
        painter.drawLine(QPointF(36, 13.5), QPointF(36, 35));
    } else if (key == QLatin1String("refresh")) {
        painter.drawArc(QRectF(10, 10, 28, 28), 35 * 16, 275 * 16);
        QPainterPath arrow;
        arrow.moveTo(34, 8);
        arrow.lineTo(39, 17);
        arrow.lineTo(29, 16);
        arrow.closeSubpath();
        painter.fillPath(arrow, color);
    } else if (key == QLatin1String("ocr")) {
        QPainterPath document;
        document.moveTo(15, 8);
        document.lineTo(29, 8);
        document.lineTo(36, 15);
        document.lineTo(36, 39);
        document.lineTo(15, 39);
        document.closeSubpath();
        painter.drawPath(document);
        painter.drawLine(QPointF(29, 8), QPointF(29, 16));
        painter.drawLine(QPointF(29, 16), QPointF(36, 16));
        painter.drawLine(QPointF(20, 23), QPointF(31, 23));
        painter.drawLine(QPointF(20, 29), QPointF(31, 29));
        painter.drawLine(QPointF(20, 35), QPointF(28, 35));
    } else if (key == QLatin1String("pause")) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawRoundedRect(QRectF(15, 11, 7, 27), 2.5, 2.5);
        painter.drawRoundedRect(QRectF(27, 11, 7, 27), 2.5, 2.5);
    } else if (key == QLatin1String("folder")) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawRoundedRect(QRectF(7, 12, 34, 25), 4, 4);
        painter.drawRoundedRect(QRectF(10, 8, 15, 9), 3, 3);
    } else if (key == QLatin1String("list")) {
        painter.setPen(QPen(color, 3.0, Qt::SolidLine, Qt::RoundCap));
        painter.drawPoint(QPointF(10, 13));
        painter.drawPoint(QPointF(10, 24));
        painter.drawPoint(QPointF(10, 35));
        painter.drawLine(QPointF(18, 13), QPointF(38, 13));
        painter.drawLine(QPointF(18, 24), QPointF(38, 24));
        painter.drawLine(QPointF(18, 35), QPointF(38, 35));
    } else if (key == QLatin1String("sparkle")) {
        QPainterPath sparkle;
        sparkle.moveTo(24, 7);
        sparkle.cubicTo(25.5, 17, 31, 22.5, 41, 24);
        sparkle.cubicTo(31, 25.5, 25.5, 31, 24, 41);
        sparkle.cubicTo(22.5, 31, 17, 25.5, 7, 24);
        sparkle.cubicTo(17, 22.5, 22.5, 17, 24, 7);
        sparkle.closeSubpath();
        painter.fillPath(sparkle, color);
    } else if (key == QLatin1String("settings")) {
        painter.drawEllipse(QRectF(17, 17, 14, 14));
        painter.drawEllipse(QRectF(21, 21, 6, 6));
        for (int angle = 0; angle < 360; angle += 45) {
            const qreal radians = qDegreesToRadians(static_cast<qreal>(angle));
            painter.drawLine(QPointF(24 + std::cos(radians) * 10, 24 + std::sin(radians) * 10),
                             QPointF(24 + std::cos(radians) * 16, 24 + std::sin(radians) * 16));
        }
    } else if (key == QLatin1String("search")) {
        painter.drawEllipse(QRectF(10, 10, 21, 21));
        painter.drawLine(QPointF(27, 27), QPointF(38, 38));
    } else if (key == QLatin1String("knowledge")) {
        painter.drawRoundedRect(QRectF(10, 10, 28, 28), 5, 5);
        painter.drawLine(QPointF(18, 10), QPointF(18, 38));
        painter.drawLine(QPointF(24, 19), QPointF(32, 19));
        painter.drawLine(QPointF(24, 26), QPointF(32, 26));
    } else if (key == QLatin1String("sun")) {
        painter.drawEllipse(QRectF(16, 16, 16, 16));
        for (int angle = 0; angle < 360; angle += 45) {
            const qreal rad = qDegreesToRadians(static_cast<qreal>(angle));
            painter.drawLine(QPointF(24 + std::cos(rad) * 12, 24 + std::sin(rad) * 12),
                             QPointF(24 + std::cos(rad) * 17, 24 + std::sin(rad) * 17));
        }
    } else if (key == QLatin1String("moon")) {
        QPainterPath moon;
        moon.addEllipse(QRectF(11, 11, 26, 26));
        QPainterPath cut;
        cut.addEllipse(QRectF(18, 7, 24, 24));
        painter.fillPath(moon.subtracted(cut), color);
    } else if (key == QLatin1String("minimize")) {
        painter.setPen(QPen(color, 3.4, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(13, 24), QPointF(35, 24));
    } else if (key == QLatin1String("maximize")) {
        painter.setPen(QPen(color, 3.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawRoundedRect(QRectF(13, 13, 22, 22), 4.0, 4.0);
    } else if (key == QLatin1String("restore")) {
        painter.setPen(QPen(color, 2.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawRoundedRect(QRectF(12, 17, 18, 18), 3.5, 3.5);
        QPainterPath back;
        back.moveTo(18, 17);
        back.lineTo(18, 13);
        back.lineTo(35, 13);
        back.lineTo(35, 30);
        back.lineTo(30, 30);
        painter.drawPath(back);
    } else if (key == QLatin1String("close")) {
        painter.setPen(QPen(color, 3.4, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(14, 14), QPointF(34, 34));
        painter.drawLine(QPointF(34, 14), QPointF(14, 34));
    }
    return pixmap;
}

QIcon modernRailIcon(const QString &key, const QColor &normal, const QColor &active,
                     const QColor &selected)
{
    QIcon icon;
    icon.addPixmap(modernRailGlyph(key, normal), QIcon::Normal, QIcon::Off);
    icon.addPixmap(modernRailGlyph(key, active), QIcon::Active, QIcon::Off);
    QColor disabled = normal;
    disabled.setAlpha(85);
    icon.addPixmap(modernRailGlyph(key, disabled), QIcon::Disabled, QIcon::Off);
    icon.addPixmap(modernRailGlyph(key, selected), QIcon::Normal, QIcon::On);
    icon.addPixmap(modernRailGlyph(key, selected), QIcon::Active, QIcon::On);
    return icon;
}

void repairBackingStoreAfterDrag(QWidget *source)
{
    if (!source)
        return;
    auto repair = [source] {
        QWidget *top = source->window();
        QWidget *embedded = source;
        while (embedded->parentWidget() && embedded->parentWidget() != top)
            embedded = embedded->parentWidget();
        if (top)
            top->repaint();
        if (embedded && embedded != top)
            embedded->repaint();
    };
    QTimer::singleShot(0, source, repair);
    QTimer::singleShot(80, source, repair);
}

class SmartEntryList final : public QListWidget
{
  public:
    SmartEntryList()
    {
        setDragEnabled(true);
        setAcceptDrops(false);
        setDragDropMode(QAbstractItemView::DragOnly);
        setDefaultDropAction(Qt::CopyAction);
        setSelectionMode(QAbstractItemView::ExtendedSelection);
    }

    void setBlankClickAction(std::function<void()> action)
    {
        m_blankClickAction = std::move(action);
    }

  protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && !itemAt(event->pos())) {
            clearSelection();
            if (m_blankClickAction)
                m_blankClickAction();
            event->accept();
            return;
        }
        QListWidget::mousePressEvent(event);
    }

    void startDrag(Qt::DropActions) override
    {
        QList<QUrl> urls;
        for (QListWidgetItem *item : selectedItems()) {
            const QString path = item->data(Qt::UserRole + 1).toString();
            if (!path.isEmpty() && QFileInfo::exists(path))
                urls << QUrl::fromLocalFile(path);
        }
        if (urls.isEmpty())
            return;
        auto *mime = new QMimeData;
        mime->setUrls(urls);
        auto *drag = new QDrag(this);
        drag->setMimeData(mime);
        drag->exec(Qt::CopyAction);
        repairBackingStoreAfterDrag(this);
    }

  private:
    std::function<void()> m_blankClickAction;
};

constexpr int kFolderCountRole = Qt::UserRole + 40;
class FolderBadgeDelegate final : public QStyledItemDelegate
{
  public:
    explicit FolderBadgeDelegate(QObject *parent) : QStyledItemDelegate(parent) {}
    void paint(QPainter *p, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        if (!index.data(kFolderCountRole).isValid()) {
            QStyledItemDelegate::paint(p, option, index);
            return;
        }
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        const QString title = opt.text;
        const QIcon icon = opt.icon;
        opt.text.clear();
        opt.icon = QIcon();
        const QWidget *widget = opt.widget;
        (widget ? widget->style() : qApp->style())
            ->drawControl(QStyle::CE_ItemViewItem, &opt, p, widget);
        const bool light = widget && widget->window()->property("smartLightTheme").toBool();
        const bool selected = opt.state & QStyle::State_Selected;
        const QColor ink =
            selected ? QColor(light ? "#4f46e5" : "#a5b4fc") : opt.palette.color(QPalette::Text);
        const QColor muted = light ? QColor("#64748b") : QColor("#94a3b8");
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const int extent = qMin(opt.decorationSize.width(), qMax(12, opt.rect.height() - 10));
        const QRect iconRect(opt.rect.left() + 10, opt.rect.center().y() - extent / 2, extent,
                             extent);
        icon.paint(p, iconRect, Qt::AlignCenter);
        QFont badgeFont = opt.font;
        badgeFont.setPixelSize(10);
        p->setFont(badgeFont);
        const QString count = QLocale().toString(index.data(kFolderCountRole).toInt());
        const int badgeWidth = qMin(64, p->fontMetrics().horizontalAdvance(count) + 14);
        const int badgeHeight = qMin(20, opt.rect.height() - 6);
        const QRect badge(opt.rect.right() - badgeWidth - 9,
                          opt.rect.center().y() - badgeHeight / 2, badgeWidth, badgeHeight);
        p->setPen(Qt::NoPen);
        p->setBrush(selected ? QColor(light ? "#eef2ff" : "#30395e")
                             : QColor(light ? "#e8edf3" : "#1e293b"));
        p->drawRoundedRect(badge, 5, 5);
        p->setPen(selected ? ink : muted);
        p->drawText(badge, Qt::AlignCenter, count);
        QFont titleFont = opt.font;
        titleFont.setBold(selected);
        p->setFont(titleFont);
        p->setPen(ink);
        const QRect textRect(iconRect.right() + 9, opt.rect.top(),
                             qMax(0, badge.left() - iconRect.right() - 16), opt.rect.height());
        p->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                    p->fontMetrics().elidedText(title, Qt::ElideRight, textRect.width()));
        p->restore();
    }
};

class SmartFileCard final : public QWidget
{
  public:
    explicit SmartFileCard(std::function<void()> activate, QWidget *parent = nullptr)
        : QWidget(parent), m_activate(std::move(activate))
    {
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_StyledBackground, true);
    }
    void setPreviewButton(QToolButton *button)
    {
        m_preview = button;
        button->installEventFilter(this);
        updatePreviewVisibility();
    }
    void setSelectedState(bool selected)
    {
        if (m_selected == selected)
            return;
        m_selected = selected;
        setProperty("selected", selected);
        style()->unpolish(this);
        style()->polish(this);
        update();
        updatePreviewVisibility();
    }

  protected:
    void enterEvent(QEvent *event) override
    {
        m_hovered = true;
        updatePreviewVisibility();
        QWidget::enterEvent(event);
    }
    void leaveEvent(QEvent *event) override
    {
        m_hovered = false;
        updatePreviewVisibility();
        QWidget::leaveEvent(event);
    }
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == m_preview &&
            (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut))
            updatePreviewVisibility(event->type() == QEvent::FocusIn);
        return QWidget::eventFilter(watched, event);
    }
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && rect().contains(event->pos()) && m_activate) {
            m_activate();
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

  private:
    void updatePreviewVisibility(bool focused = false)
    {
        if (m_preview)
            m_preview->setVisible((isVisible() && rect().contains(mapFromGlobal(QCursor::pos()))) ||
                                  m_hovered || m_selected || focused || m_preview->hasFocus());
    }
    QToolButton *m_preview = nullptr;
    bool m_hovered = false, m_selected = false;
    std::function<void()> m_activate;
};

QIcon entryIcon(const SmartSpaceEntry &entry)
{
    if (entry.isDir)
        return QIcon::fromTheme(QStringLiteral("folder"));
    if (entry.category == QLatin1String("image"))
        return QIcon::fromTheme(QStringLiteral("image-x-generic"));
    if (entry.category == QLatin1String("pdf"))
        return QIcon::fromTheme(QStringLiteral("application-pdf"));
    if (entry.category == QLatin1String("presentation"))
        return QIcon::fromTheme(QStringLiteral("x-office-presentation"));
    if (entry.category == QLatin1String("spreadsheet"))
        return QIcon::fromTheme(QStringLiteral("x-office-spreadsheet"));
    return QIcon::fromTheme(QStringLiteral("text-x-generic"));
}

QStringList localQueryTerms(const QString &query)
{
    QStringList result;
    QSet<QString> seen;
    auto add = [&result, &seen](QString term) {
        term = term.trimmed().toCaseFolded();
        if (term.size() < 2 || seen.contains(term) || result.size() >= 28)
            return;
        seen.insert(term);
        result << term;
    };
    add(query);
    const QRegularExpression parts(QStringLiteral("[\\p{Han}]+|[A-Za-z]+|[0-9]+"));
    QRegularExpressionMatchIterator iterator = parts.globalMatch(query);
    while (iterator.hasNext()) {
        const QString part = iterator.next().captured();
        add(part);
        const bool han = QRegularExpression(QStringLiteral("^[\\p{Han}]+$")).match(part).hasMatch();
        if (!han || part.size() <= 2)
            continue;
        // Chinese searches do not always contain spaces. Bounded 2-4
        // character n-grams keep local matching useful for phrases such as
        // “宁波银行2025年报告”.
        for (int length = qMin(4, part.size()); length >= 2; --length) {
            for (int offset = 0; offset + length <= part.size(); ++offset)
                add(part.mid(offset, length));
        }
    }
    std::sort(result.begin(), result.end(), [](const QString &left, const QString &right) {
        return left.size() == right.size() ? left < right : left.size() > right.size();
    });
    return result;
}

int localEntryScore(const SmartSpaceEntry &entry, const QString &query,
                    QStringList *matchedTerms = nullptr)
{
    const QString foldedQuery = query.trimmed().toCaseFolded();
    const QString name = entry.name.toCaseFolded();
    const QString path = entry.path.toCaseFolded();
    const QString content = entry.content.toCaseFolded();
    int score = 0;
    if (!foldedQuery.isEmpty()) {
        if (name.contains(foldedQuery))
            score += kExactNameScore;
        if (content.contains(foldedQuery))
            score += kExactContentScore;
        if (path.contains(foldedQuery))
            score += kExactPathScore;
    }
    for (const QString &term : localQueryTerms(query)) {
        const bool inName = name.contains(term);
        const bool inContent = content.contains(term);
        const bool inPath = path.contains(term);
        if (!inName && !inContent && !inPath)
            continue;
        if (matchedTerms && !matchedTerms->contains(term))
            matchedTerms->append(term);
        const int lengthBonus = qMin(10, term.size()) * 8;
        if (inName)
            score += kTermNameScore + lengthBonus;
        if (inContent)
            score += kTermContentScore + lengthBonus;
        if (inPath)
            score += kTermPathScore + lengthBonus;
        if (term.size() == 4 && term.at(0).isDigit())
            score += kYearTermBonus;
    }
    return score;
}

void clearButtonLayout(QHBoxLayout *layout)
{
    if (!layout)
        return;
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *widget = item->widget())
            widget->deleteLater();
        delete item;
    }
}

} // namespace

SmartSpaceWidget::SmartSpaceWidget(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    setAutoFillBackground(false);
    setMinimumSize(620, 360);
    setMouseTracking(true);
    setWindowTitle(QStringLiteral("AI空间"));
    setObjectName(QStringLiteral("smartSpaceSurface"));

    buildUi();
    new WindowPointerObserver(this, [this](QPoint pos) {
        if (!m_resizing && !m_dragging && !(QApplication::mouseButtons() & Qt::LeftButton))
            updateResizeCursor(pos);
    });
    restoreSettings();
    m_geometrySaveTimer.setSingleShot(true);
    m_geometrySaveTimer.setInterval(250);
    connect(&m_geometrySaveTimer, &QTimer::timeout, this, [this] { saveSettings(); });
    applyTheme();
    rebuildFileScopePill();
    rebuildCategoryPills();
    updateActionState();
    updateWindowStateButtons();
    updateRoundedMask();

    m_jobController = new KnowledgeJobController(this);
    installWindowCornerFallback(this, 12);

    m_watcher = new QFileSystemWatcher(this);
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { scheduleReindex(); });
    connect(m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { scheduleReindex(); });

    m_reindexDebounce.setSingleShot(true);
    m_reindexDebounce.setInterval(4000);
    connect(&m_reindexDebounce, &QTimer::timeout, this, &SmartSpaceWidget::startIndexing);

    connect(&m_periodicRefresh, &QTimer::timeout, this, &SmartSpaceWidget::startIndexing);
    configureIndexScheduling();
    // Login brings up the desktop, file manager and session services in
    // several waves. QFileSystemWatcher can report those startup changes as
    // ordinary directory changes, so keep automatic policies disarmed while
    // the desktop session settles.
    QTimer::singleShot(30000, this, [this] { m_automaticIndexingReady = true; });

    if (QFileInfo::exists(indexPath())) {
        loadIndexAsync();
    } else {
        rebuildWatches();
        m_statusLabel->setText(QStringLiteral("尚未建立索引 · 从知识库管理选择存储盘并分阶段建库"));
    }
    // Startup auto-index is a test-fixture capability only. Requiring the
    // second guard prevents a leaked variable in a real desktop session from
    // silently starting the user's index.
    if (qEnvironmentVariableIntValue("AI_SPACE_AUTO_INDEX") == 1 &&
        qEnvironmentVariableIntValue("AI_SPACE_TEST_AUTO_INDEX") == 1)
        QTimer::singleShot(250, this, &SmartSpaceWidget::startIndexing);
}

SmartSpaceWidget::~SmartSpaceWidget()
{
    m_geometrySaveTimer.stop();
    if (m_providerAuditProcess) {
        m_providerAuditProcess->kill();
        m_providerAuditProcess->waitForFinished(200);
    }
    if (m_knowledgeSearchProcess) {
        m_knowledgeSearchProcess->kill();
        m_knowledgeSearchProcess->waitForFinished(200);
    }
    if (m_libraryPreviewProcess) {
        m_libraryPreviewProcess->kill();
        m_libraryPreviewProcess->waitForFinished(200);
    }
    saveSettings();
    delete m_jobController;
    m_jobController = nullptr;
    m_knowledgeProcess = nullptr;
    if (m_previewProcess && m_previewProcess->state() != QProcess::NotRunning) {
        m_previewProcess->kill();
        m_previewProcess->waitForFinished(200);
    }
}

void SmartSpaceWidget::buildUi()
{
    auto *shell = new QVBoxLayout(this);
    shell->setContentsMargins(1, 1, 1, 1);
    shell->setSpacing(0);

    auto *titleBar = new AiSpace::WindowTitleBarWidget(this);
    m_actionRail = titleBar;
    auto makeUtility = [this](const QString &key, const QString &tip) {
        auto *button = new QToolButton(m_actionRail);
        button->setProperty("titleControl", true);
        button->setProperty("railIconKey", key);
        button->setToolTip(tip);
        button->setAccessibleName(tip);
        button->setFixedSize(28, 26);
        button->setCursor(Qt::PointingHandCursor);
        return button;
    };
    m_pinButton = makeUtility("pin", QStringLiteral("置顶窗口"));
    m_pinButton->setCheckable(true);
    m_themeToggleButton = makeUtility("moon", QStringLiteral("切换明暗主题"));
    m_moreButton = makeUtility("settings", QStringLiteral("偏好设置"));
    m_moreButton->setCheckable(true);
    titleBar->attachHostUtilityButtons(m_pinButton, m_themeToggleButton, m_moreButton);
    m_searchNavButton = new QToolButton(m_actionRail);
    m_searchNavButton->setText(QStringLiteral("‹ 返回检索"));
    m_searchNavButton->setProperty("titleControl", true);
    m_searchNavButton->hide();
    static_cast<QHBoxLayout *>(titleBar->layout())->insertWidget(4, m_searchNavButton);
    connect(m_searchNavButton, &QToolButton::clicked, this,
            [this] { setSettingsViewVisible(false); });
    connect(m_moreButton, &QToolButton::clicked, this,
            [this] { setSettingsViewVisible(!m_settingsViewVisible); });
    m_minimizeButton = titleBar->minimizeButton();
    m_maximizeButton = titleBar->maximizeButton();
    m_windowCloseButton = titleBar->closeButton();
    shell->addWidget(m_actionRail);

    // Main Stack: Workspace Page (Index search & browser) + Settings Host Page
    m_mainStack = new QStackedWidget(this);
    m_mainStack->setObjectName(QStringLiteral("smartMainStack"));

    m_workspacePage = new QWidget(m_mainStack);
    m_workspacePage->setObjectName(QStringLiteral("smartWorkspacePage"));
    m_contentContainer = m_workspacePage;
    auto *outer = new QVBoxLayout(m_workspacePage);
    outer->setContentsMargins(10, 0, 10, 0);
    outer->setSpacing(0);

    m_settingsHost = new QWidget(m_mainStack);
    m_settingsHost->setObjectName(QStringLiteral("smartSettingsHost"));
    auto *hostLayout = new QVBoxLayout(m_settingsHost);
    hostLayout->setContentsMargins(0, 0, 0, 0);

    m_mainStack->addWidget(m_workspacePage);
    m_mainStack->addWidget(m_settingsHost);
    shell->addWidget(m_mainStack, 1);

    m_header = new QWidget(m_workspacePage);
    m_header->setObjectName(QStringLiteral("smartSpaceHeader"));
    auto *headerLayout = new QVBoxLayout(m_header);
    headerLayout->setContentsMargins(12, 6, 12, 6);
    m_header->setFixedHeight(AiSpace::WindowMetrics::SearchHeaderHeight);
    headerLayout->setSpacing(6);

    auto makeToolButton = [this](QHBoxLayout *row, const QString &icon, const QString &text,
                                 const QString &tip, const QString &role) {
        auto *button = new QToolButton(m_header);
        if (!icon.isEmpty())
            button->setIcon(QIcon::fromTheme(icon));
        button->setText(text);
        button->setToolTip(tip);
        button->setProperty("buttonRole", role);
        button->setAutoRaise(false);
        button->setCursor(Qt::PointingHandCursor);
        if (!text.isEmpty())
            button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        row->addWidget(button);
        return button;
    };

    auto *commandRow = new QHBoxLayout;
    commandRow->setContentsMargins(0, 0, 0, 0);
    commandRow->setSpacing(8);
    m_searchEdit = new QLineEdit(m_header);
    m_searchEdit->setObjectName(QStringLiteral("smartSpaceSearch"));
    m_searchEdit->setPlaceholderText(QStringLiteral("搜索文件与内容 · Enter 检索"));
    m_searchEdit->setToolTip(QStringLiteral("输入后按 Enter 执行搜索\n"
                                            "空格分隔多个必须同时命中的条件，例如：宁波 pdf\n"
                                            "正则表达式以 re: 开头，例如：re:^宁波.*\\.pdf$"));
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setMinimumHeight(36);
    m_searchEdit->setMinimumWidth(120);
    m_searchEdit->addAction(QIcon::fromTheme(QStringLiteral("edit-find")),
                            QLineEdit::LeadingPosition);
    commandRow->addWidget(m_searchEdit, 1);
    m_searchButton = makeToolButton(commandRow, QString(), QStringLiteral("搜索"),
                                    QStringLiteral("执行搜索（Enter）；空格表示“且”，re:表示正则"),
                                    QStringLiteral("accent"));
    m_searchButton->setObjectName(QStringLiteral("smartSearchButton"));
    m_searchButton->hide();

    m_indexProgress = new QProgressBar(m_header);
    m_indexProgress->setObjectName(QStringLiteral("smartIndexProgress"));
    m_indexProgress->setRange(0, 0);
    m_indexProgress->setTextVisible(true);
    m_indexProgress->setFormat(QStringLiteral("正在统计待索引文件…"));
    m_indexProgress->setAlignment(Qt::AlignCenter);
    m_indexProgress->setFixedHeight(20);
    m_indexProgress->setMouseTracking(true);
    m_indexProgress->setAttribute(Qt::WA_Hover, true);
    m_indexProgress->installEventFilter(this);
    m_indexProgress->setToolTip(QStringLiteral("索引进度"));
    m_indexProgress->hide();

    m_categoryCombo = new QComboBox(m_header);
    m_categoryCombo->setObjectName(QStringLiteral("smartCategoryFilter"));
    m_categoryCombo->addItem(QStringLiteral("全部类型"), QString());
    m_categoryCombo->addItem(QStringLiteral("图片"), QStringLiteral("image"));
    m_categoryCombo->addItem(QStringLiteral("PDF"), QStringLiteral("pdf"));
    m_categoryCombo->addItem(QStringLiteral("文档"), QStringLiteral("document"));
    m_categoryCombo->addItem(QStringLiteral("演示"), QStringLiteral("presentation"));
    m_categoryCombo->addItem(QStringLiteral("表格"), QStringLiteral("spreadsheet"));
    m_categoryCombo->addItem(QStringLiteral("其他"), QStringLiteral("other"));
    m_categoryCombo->setMinimumHeight(36);
    m_categoryCombo->setFixedWidth(132);
    commandRow->addWidget(m_categoryCombo);
    connect(m_categoryCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int index) {
                m_selectedCategories.clear();
                if (m_categoryCombo && index > 0)
                    m_selectedCategories << m_categoryCombo->itemData(index).toString();
                updateResults(true);
            });

    m_sortCombo = new QComboBox(m_header);
    m_sortCombo->setObjectName("smartSortFilter");
    m_sortCombo->addItem(QStringLiteral("按相关度"), 0);
    m_sortCombo->addItem(QStringLiteral("按修改时间"), 1);
    m_sortCombo->addItem(QStringLiteral("按文件大小"), 2);
    m_sortCombo->setFixedWidth(146);
    m_sortCombo->setMinimumHeight(36);
    commandRow->addWidget(m_sortCombo);
    connect(m_sortCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int index) {
                m_resultSortMode = index;
                saveSettings();
                updateResults(false);
            });
    headerLayout->addLayout(commandRow);

    m_settingsButton = m_moreButton;
    outer->addWidget(m_header);

    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setObjectName(QStringLiteral("smartMainSplitter"));
    m_splitter->setChildrenCollapsible(false);
    m_splitter->setHandleWidth(10);

    m_folderLevelsContainer = new QWidget;
    m_folderLevelsContainer->setObjectName(QStringLiteral("smartFolderLevels"));
    m_folderLevelsContainer->installEventFilter(this);
    m_folderLevelsLayout = new QVBoxLayout(m_folderLevelsContainer);
    m_folderLevelsLayout->setContentsMargins(0, 0, 0, 0);
    m_folderLevelsLayout->setSpacing(8);
    m_folderLevelsLayout->addStretch(0);
    m_folderScroll = new QScrollArea;
    m_folderScroll->setObjectName(QStringLiteral("smartFolderScroll"));
    m_folderScroll->setWidgetResizable(true);
    m_folderScroll->setFrameShape(QFrame::NoFrame);
    m_folderScroll->setWidget(m_folderLevelsContainer);
    m_folderScroll->viewport()->installEventFilter(this);
    m_folderPanel = createPanel(QStringLiteral("文件夹拾取"), m_folderScroll,
                                QStringLiteral("smartFolderPanel"));

    m_fileList = createEntryList();
    m_fileList->setObjectName("smartFileList");
    connect(m_fileList, &QListWidget::itemSelectionChanged, this, [this] {
        for (int row = 0; row < m_fileList->count(); ++row) {
            auto *item = m_fileList->item(row);
            if (auto *card = dynamic_cast<SmartFileCard *>(m_fileList->itemWidget(item)))
                card->setSelectedState(item->isSelected());
        }
    });
    m_filePanel =
        createPanel(QStringLiteral("匹配结果"), m_fileList, QStringLiteral("smartFilePanel"));

    m_splitter->addWidget(m_folderPanel);
    m_splitter->addWidget(m_filePanel);
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_folderPanel->setMinimumWidth(260);
    m_folderPanel->setMaximumWidth(360);
    m_splitter->setSizes({280, 600});
    outer->addWidget(m_splitter, 1);

    m_previewPanel = new QFrame(m_resultStack);
    m_previewPanel->setObjectName(QStringLiteral("smartPreviewPanel"));
    auto *previewLayout = new QVBoxLayout(m_previewPanel);
    previewLayout->setContentsMargins(12, 11, 12, 11);
    previewLayout->setSpacing(8);
    auto *previewHeader = new QHBoxLayout;
    m_previewTitle = new QLabel(QStringLiteral("相关内容"), m_previewPanel);
    m_previewTitle->setObjectName(QStringLiteral("smartPreviewTitle"));
    m_previewTitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    previewHeader->addWidget(m_previewTitle, 1);
    m_previewCloseButton = new QToolButton(m_previewPanel);
    m_previewCloseButton->setText(QStringLiteral("×"));
    m_previewCloseButton->setFixedSize(28, 28);
    m_previewCloseButton->setAccessibleName(QStringLiteral("关闭预览"));
    m_previewCloseButton->setToolTip(QStringLiteral("关闭预览"));
    m_previewCloseButton->setProperty("buttonRole", QStringLiteral("quiet"));
    previewHeader->addWidget(m_previewCloseButton);
    previewLayout->addLayout(previewHeader);
    m_previewMeta = new QLabel(m_previewPanel);
    m_previewMeta->setObjectName(QStringLiteral("smartPreviewMeta"));
    m_previewMeta->setWordWrap(true);
    previewLayout->addWidget(m_previewMeta);
    m_previewStack = new QStackedWidget(m_previewPanel);
    m_previewText = new QTextBrowser(m_previewStack);
    m_previewText->setObjectName(QStringLiteral("smartPreviewText"));
    m_previewText->setFrameShape(QFrame::NoFrame);
    m_previewText->setOpenExternalLinks(false);
    m_previewText->setOpenLinks(false);
    connect(m_previewText, &QTextBrowser::anchorClicked, this, [this](const QUrl &url) {
        if (url.scheme() == QLatin1String("aispace-next"))
            if (const auto *entry = entryForPath(m_previewPath))
                readLibraryPreview(*entry, url.path().toInt());
    });
    m_previewStack->addWidget(m_previewText);
    auto *imageScroll = new QScrollArea(m_previewStack);
    imageScroll->setObjectName(QStringLiteral("smartPreviewImageScroll"));
    imageScroll->setWidgetResizable(true);
    imageScroll->setFrameShape(QFrame::NoFrame);
    m_previewImage = new QLabel(imageScroll);
    m_previewImage->setAlignment(Qt::AlignCenter);
    m_previewImage->setObjectName(QStringLiteral("smartPreviewImage"));
    imageScroll->setWidget(m_previewImage);
    m_previewStack->addWidget(imageScroll);
    previewLayout->addWidget(m_previewStack, 1);
    m_previewOpenButton = new QPushButton(QStringLiteral("打开文件"), m_previewPanel);
    m_previewOpenButton->setObjectName(QStringLiteral("smartPreviewOpen"));
    previewLayout->addWidget(m_previewOpenButton);
    m_previewPanel->setMinimumWidth(220);
    m_splitter->addWidget(m_previewPanel);
    m_splitter->setStretchFactor(2, 1);
    m_previewPanel->hide();

    m_statusContainer = new QWidget(this);
    m_statusContainer->setObjectName("smartStatusBar");
    m_statusContainer->setFixedHeight(AiSpace::WindowMetrics::BottomStatusHeight);
    auto *statusRow = new QHBoxLayout(m_statusContainer);
    statusRow->setContentsMargins(10, 0, 4, 0);
    statusRow->setSpacing(4);
    m_statusLabel = new CompactKnowledgeStatus(m_statusContainer);
    m_statusLabel->setObjectName(QStringLiteral("smartSpaceStatus"));
    m_statusLabel->setText(QStringLiteral("尚未建立索引"));
    m_statusLabel->setCursor(Qt::PointingHandCursor);
    m_statusLabel->setToolTip(QStringLiteral("点击查看完整状态、OCR 待处理量与资源限制"));
    m_statusLabel->installEventFilter(this);
    m_statusLabel->setFocusPolicy(Qt::StrongFocus);
    m_statusLabel->setMinimumWidth(0);
    m_statusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    statusRow->addWidget(m_statusLabel, 1);
    m_indexProgress->setParent(m_statusContainer);
    m_indexProgress->setMinimumWidth(70);
    m_indexProgress->setMaximumWidth(180);
    statusRow->addWidget(m_indexProgress, 1);
    auto createTask = [&](const QString &key, const QString &text, const QString &tip) {
        auto *button = new QToolButton(m_statusContainer);
        button->setProperty("railAction", true);
        button->setProperty("railIconKey", key);
        button->setText(text);
        button->setToolTip(tip);
        button->setAccessibleName(tip);
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setIconSize(QSize(14, 14));
        button->setFixedHeight(28);
        statusRow->addWidget(button);
        return button;
    };
    m_idleIndexButton = createTask("database", QStringLiteral("① 全量扫描"),
                                   QStringLiteral("第1步：建立文件清单，不提取正文"));
    m_textIndexButton = createTask("refresh", QStringLiteral("② 文本建库"),
                                   QStringLiteral("第2步：按已有清单提取正文，不运行OCR"));
    m_ocrIndexButton = createTask("ocr", QStringLiteral("③ OCR补全"),
                                  QStringLiteral("第3步：按已扫描清单识别图片与扫描PDF"));
    m_refreshButton = createTask(
        "refresh", QStringLiteral("增量"),
        QStringLiteral("在当前知识库基础上重新扫描；未变化的正文会复用，更新或删除有变化的记录"));
    m_pauseIndexButton =
        createTask("pause", QStringLiteral("暂停"), QStringLiteral("安全暂停当前索引"));
    m_pauseIndexButton->setEnabled(false);
    m_knowledgeMenuButton =
        createTask("knowledge", QStringLiteral("知识库 ▾"), QStringLiteral("知识库管理"));
    m_knowledgeMenuButton->setObjectName("smartKnowledgeMenu");
    connect(m_knowledgeMenuButton, &QToolButton::clicked, this, &SmartSpaceWidget::showHeaderMenu);
    m_sizeGrip = new AspectRatioSizeGrip(this, m_statusContainer);
    m_sizeGrip->setToolTip(QStringLiteral("拖动缩放AI空间"));
    statusRow->addWidget(m_sizeGrip);
    outer->addWidget(m_statusContainer);
    auto *searchShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+K")), this);
    connect(searchShortcut, &QShortcut::activated, this, [this] {
        setSettingsViewVisible(false);
        m_searchEdit->setFocus();
        m_searchEdit->selectAll();
    });

    connect(m_textIndexButton, &QToolButton::clicked, this,
            [this] { startGuidedBuild(false, false, QStringLiteral("text")); });
    connect(m_refreshButton, &QToolButton::clicked, this, &SmartSpaceWidget::startIndexing);
    connect(m_themeToggleButton, &QToolButton::clicked, this, [this] {
        const int nextMode = (m_themeMode == 2 ? 1 : 2);
        setSimpleTheme(nextMode);
    });
    connect(m_idleIndexButton, &QToolButton::clicked, this,
            [this] { startGuidedBuild(false, false, QStringLiteral("manifest")); });
    connect(m_ocrIndexButton, &QToolButton::clicked, this, &SmartSpaceWidget::startOcrIndex);
    connect(m_pauseIndexButton, &QToolButton::clicked, this, [this] {
        if (m_libraryJobBusy) {
            m_statusLabel->setText(QStringLiteral("正在暂停知识库任务 · 已提交内容会保留"));
            stopIdleFullIndex();
        }
    });
    connect(m_searchButton, &QToolButton::clicked, this, &SmartSpaceWidget::executeSearch);
    connect(m_searchEdit, &QLineEdit::returnPressed, this, &SmartSpaceWidget::executeSearch);
    connect(m_pinButton, &QToolButton::clicked, this, [this] { setAlwaysOnTop(!m_alwaysOnTop); });
    connect(m_minimizeButton, &QToolButton::clicked, this, &SmartSpaceWidget::minimizeWindow);
    connect(m_maximizeButton, &QToolButton::clicked, this, &SmartSpaceWidget::toggleMaximized);
    connect(m_windowCloseButton, &QToolButton::clicked, this, &SmartSpaceWidget::closeRequested);
    connect(m_previewCloseButton, &QToolButton::clicked, this, &SmartSpaceWidget::hidePreview);
    connect(m_previewOpenButton, &QPushButton::clicked, this, [this] { openPath(m_previewPath); });
    connect(m_searchEdit, &QLineEdit::textChanged, this,
            [this] { m_statusLabel->setText(QStringLiteral("搜索条件已修改 · 按 Enter 后生效")); });
    connect(m_fileList, &QListWidget::itemClicked, this, &SmartSpaceWidget::openFileItem);
    connect(m_fileList, &QListWidget::customContextMenuRequested, this,
            [this](const QPoint &pos) { showEntryMenu(m_fileList, pos); });
    auto *previewShortcut = new QShortcut(QKeySequence(Qt::Key_Space), m_fileList);
    previewShortcut->setContext(Qt::WidgetShortcut);
    connect(previewShortcut, &QShortcut::activated, this, [this] {
        if (QListWidgetItem *item = m_fileList->currentItem())
            showPreviewForPath(item->data(PathRole).toString());
    });
    auto *closePreviewShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    closePreviewShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(closePreviewShortcut, &QShortcut::activated, this, &SmartSpaceWidget::hidePreview);

    rebuildCategoryPills();
}

void SmartSpaceWidget::applyTheme()
{
    const bool lightMode = (m_themeMode == 2);
    setProperty("smartLightTheme", lightMode);
    m_customOpacity = 100;
    m_customColorsEnabled = false;

    if (lightMode) {
        m_surfaceColor = QColor(QStringLiteral("#f1f5f9"));
        m_cardColor = QColor(QStringLiteral("#ffffff"));
        m_borderColor = QColor(QStringLiteral("#e2e8f0"));
        m_textColor = QColor(QStringLiteral("#0f172a"));
        m_mutedColor = QColor(QStringLiteral("#64748b"));
        m_accentColor = QColor(QStringLiteral("#4f46e5"));
    } else {
        m_surfaceColor = QColor(QStringLiteral("#0b1120"));
        m_cardColor = QColor(QStringLiteral("#111827"));
        m_borderColor = QColor(QStringLiteral("#1e293b"));
        m_textColor = QColor(QStringLiteral("#f1f5f9"));
        m_mutedColor = QColor(QStringLiteral("#94a3b8"));
        m_accentColor = QColor(QStringLiteral("#a5b4fc"));
    }
    setFont(QFont());
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    setAttribute(Qt::WA_NoSystemBackground, isWindow());
    setAttribute(Qt::WA_StyledBackground, false);
    setAutoFillBackground(false);

    if (layout()) {
        const int m = isWindowMaximized() ? 0 : 1;
        layout()->setContentsMargins(m, m, m, m);
    }

    if (m_themeToggleButton) {
        const QSignalBlocker blocker(m_themeToggleButton);
        m_themeToggleButton->setText(lightMode ? QStringLiteral("☀") : QStringLiteral("☾"));
        m_themeToggleButton->setToolTip(lightMode
                                            ? QStringLiteral("当前为白天模式，点击切换为黑夜模式")
                                            : QStringLiteral("当前为黑夜模式，点击切换为白天模式"));
    }

    if (m_actionRail) {
        const QList<QToolButton *> railButtons = findChildren<QToolButton *>();
        for (QToolButton *button : railButtons) {
            const QString key = button->property("railIconKey").toString();
            if (key.isEmpty())
                continue;
            if (button->property("titleClose").toBool()) {
                button->setIcon(modernRailIcon(key, m_mutedColor, Qt::white, Qt::white));
            } else if (button->property("navSegment").toBool()) {
                button->setIcon(modernRailIcon(key, m_mutedColor, m_accentColor, m_accentColor));
            } else if (button->property("titleControl").toBool()) {
                button->setIcon(modernRailIcon(key, m_mutedColor, m_textColor, m_accentColor));
            } else {
                button->setIcon(modernRailIcon(key, m_mutedColor, m_accentColor, Qt::white));
            }
        }
    }
    updateWindowStateButtons();

    QPalette widgetPalette = palette();
    widgetPalette.setColor(QPalette::Window, m_surfaceColor);
    widgetPalette.setColor(QPalette::WindowText, m_textColor);
    widgetPalette.setColor(QPalette::Base, m_cardColor);
    widgetPalette.setColor(QPalette::Text, m_textColor);
    widgetPalette.setColor(QPalette::Button, m_cardColor);
    widgetPalette.setColor(QPalette::ButtonText, m_textColor);
    widgetPalette.setColor(QPalette::Highlight, m_accentColor);
    widgetPalette.setColor(QPalette::HighlightedText, Qt::white);
#if QT_VERSION >= QT_VERSION_CHECK(5, 12, 0)
    widgetPalette.setColor(QPalette::PlaceholderText, m_mutedColor);
#endif
    setPalette(widgetPalette);
    if (m_searchEdit) {
        QPalette searchPalette = m_searchEdit->palette();
#if QT_VERSION >= QT_VERSION_CHECK(5, 12, 0)
        searchPalette.setColor(QPalette::PlaceholderText, m_mutedColor);
#endif
        m_searchEdit->setPalette(searchPalette);
    }

    QColor hover = m_accentColor;
    hover.setAlpha(lightMode ? 14 : 32);
    QColor fieldBg = lightMode ? QColor("#f8fafc") : QColor("#1e293b");
    QColor cardBorder = m_borderColor;
    QColor accentHover = m_accentColor.darker(110);

    const QString style = QStringLiteral(R"(
        QWidget { color: %1; font-size: 13px; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif; }
        QFrame#smartActionRail {
            background: transparent; border: none;
        }
        QLabel#smartBrandTitle {
            font-size: 13px; font-weight: 700; color: %1;
        }
        QFrame#smartNavSegment {
            background: %4; border: 1px solid %3; border-radius: 16px;
        }
        QToolButton[navSegment="true"] {
            color: %5; background: transparent; border: none; border-radius: 14px;
            padding: 4px 12px; font-size: 12px; font-weight: 600;
        }
        QToolButton[navSegment="true"]:hover { color: %1; }
        QToolButton[navSegment="true"]:checked {
            color: %6; background: %2; font-weight: 700;
        }
        QToolButton[railAction="true"] {
            color: %1; background: %2; border: 1px solid %3;
            border-radius: 7px; padding: 2px 9px; font-size: 12px; font-weight: 500;
        }
        QToolButton[railAction="true"]:hover {
            background: %7; border-color: %6; color: %6;
        }
        QToolButton[railAction="true"]:disabled {
            color: %5; border-color: transparent; background: transparent;
        }
        QFrame#smartRailSeparator {
            background: %3;
        }
        QToolButton[titleControl="true"] {
            color: %5; background: transparent; border: none;
            border-radius: 6px; padding: 0; font-size: 16px; font-weight: 600;
        }
        QToolButton[titleControl="true"]:hover {
            color: %1; background: %7;
        }
        QToolButton[titleControl="true"]:checked {
            color: %6; background: %7;
        }
        QToolButton[titleClose="true"] {
            color: %5; background: transparent; border: none;
            border-radius: 6px; padding: 0;
        }
        QToolButton[titleClose="true"]:hover {
            color: white; background: #ef4444;
        }
        QStackedWidget#smartMainStack { background: transparent; border: none; }
        QWidget#smartWorkspacePage { background: transparent; }
        QWidget#smartSpaceHeader { background: transparent; border: none; }
        #smartSpaceSearch {
            color: %1; background: %2; border: 1px solid %3;
            border-radius: 10px; padding-left: 12px; padding-right: 12px;
            font-size: 14px;
        }
        #smartSpaceSearch:focus { border-color: #6366f1; }
        QToolButton#smartSearchButton {
            color: white; background: %6; border: 1px solid %6;
            border-radius: 10px; font-weight: 600; font-size: 13px;
        }
        QToolButton#smartSearchButton:hover { background: %8; border-color: %8; }
        QComboBox#smartCategoryFilter, QComboBox#smartSortFilter {
            color: %1; background: %2; border: 1px solid %3;
            border-radius: 10px; padding-left: 12px; padding-right: 10px;
        }
        QComboBox#smartCategoryFilter:focus, QComboBox#smartSortFilter:focus { border-color: #6366f1; }
        QComboBox::drop-down { width: 24px; border: none; }
        QComboBox QAbstractItemView {
            color: %1; background: %2; border: 1px solid %3; border-radius: 8px;
            selection-background-color: %6; selection-color: white;
            padding: 4px; outline: none;
        }
        #smartSpaceStatus { color: %5; background: transparent; border: none; padding: 3px 6px; font-size: 12px; }
        QFrame#smartFolderPanel {background: transparent; border: none;}
        QFrame#smartFilePanel, QFrame#smartPreviewPanel {
            background: %2; border: 1px solid %3; border-radius: 12px;
        }
        QStackedWidget#smartResultStack { background: transparent; border: none; }
        #smartPreviewTitle { color: %1; font-size: 15px; font-weight: 600; }
        #smartPreviewMeta { color: %5; font-size: 12px; }
        QTextBrowser#smartPreviewText { color: %1; background: transparent; border: none; padding: 6px; }
        QLabel#smartPreviewImage { background: transparent; }
        QToolButton[buttonRole="quiet"] {
            color: %5; background: transparent; border: none;
            border-radius: 6px; padding: 0; font-size: 20px;
        }
        QToolButton[buttonRole="quiet"]:hover { color: %1; background: %7; }
        QPushButton#smartPreviewOpen {
            color: white; background: #4f46e5; border: 1px solid #4f46e5;
            border-radius: 8px; padding: 8px 16px; font-weight: 600;
        }
        QPushButton#smartPreviewOpen:hover { background: #4338ca; border-color: #4338ca; }
        QLabel[panelTitle="true"] { color: %1; font-size: 13px; font-weight: 600; }
        QLabel#smartPanelIcon { background: transparent; padding: 0; }
        QLabel#smartPanelCount {
            color: %5; background: %4; border-radius: 10px;
            padding: 2px 8px; font-size: 11px; font-weight: 600;
        }
        QToolButton[panelControl="true"] {
            color: %5; background: %4; border: 1px solid %3;
            border-radius: 6px; padding: 2px 8px; font-size: 12px;
        }
        QToolButton[panelControl="true"]:hover { color: %1; border-color: %6; }
        QListWidget { color: %1; background: transparent; border: none; outline: none; }
        QListWidget::item { min-height: 26px; padding: 4px 6px; border-radius: 7px; }
        QListWidget::item:hover { background: %7; }
        QListWidget::item:selected { background: %7; color: %1; }
        QListWidget#smartRootFolderList::item, QListWidget#smartChildFolderList::item {
            border: 1px solid transparent; border-radius: 8px; padding: 3px 4px;
        }
        QListWidget#smartRootFolderList::item:selected, QListWidget#smartChildFolderList::item:selected {
            background: %2; border-color: %3; color: %6;
        }
        QListWidget::item:disabled { color: %5; }
        QWidget#fileResultCard {
            background: transparent; border: 1px solid transparent; border-radius: 10px;
        }
        QWidget#fileResultCard:hover { background: %4; border-color: %3; }
        QWidget#fileResultCard[selected="true"] {background: %7; border-color: #6366f1;}
        QListWidget#smartFileList::item {padding: 0; border: none;}
        QListWidget#smartFileList::item:hover, QListWidget#smartFileList::item:selected {background: transparent;}
        QLabel[folderBadge="true"] {color: %5; background: %4; border: 1px solid %3; border-radius: 4px; padding: 1px 6px; font-size: 10px;}
        QLabel[cardName="true"] { color: %1; font-size: 13px; font-weight: 600; }
        QLabel[cardMeta="true"] { color: %5; font-size: 11px; }
        QLabel[cardSnippet="true"] { color: %5; font-size: 12px; }
        QLabel[matchScore="true"] {
            color: #10b981; background: transparent; font-size: 11px;
            font-weight: 600; padding: 2px 4px;
        }
        QLabel[matchLevel="medium"] { color: #f59e0b; }
        QToolButton[previewAction="true"] {
            color: %6; background: %7; border: none;
            border-radius: 6px; padding: 3px 6px; font-size: 11px; font-weight: 600;
        }
        QToolButton[previewAction="true"]:hover { color: white; background: #4f46e5; }
        QToolButton[loadMoreResults="true"] {
            color: %6; background: %7; border: 1px solid %3;
            border-radius: 8px; padding: 8px 16px; font-weight: 600;
        }
        QToolButton[loadMoreResults="true"]:hover { color: white; background: %6; }
        QProgressBar#smartIndexProgress {
            color: %1; background: %4; border: 1px solid %3;
            border-radius: 7px; text-align: center; font-size: 11px;
            font-weight: 600;
        }
        QProgressBar#smartIndexProgress::chunk {
            background: %6; border-radius: 5px;
        }
        QSplitter#smartMainSplitter::handle:horizontal {
            background: transparent; width: 6px;
        }
        QSplitter#smartMainSplitter::handle:horizontal:hover {
            background: %6;
        }
        QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; border: none; }
        QScrollBar:vertical { background: transparent; width: 6px; margin: 2px; }
        QScrollBar::handle:vertical { background: %3; border-radius: 3px; min-height: 24px; }
        QScrollBar::handle:vertical:hover { background: %5; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
    )")
                              .arg(cssColor(m_textColor))
                              .arg(cssColor(m_cardColor))
                              .arg(cssColor(cardBorder))
                              .arg(cssColor(fieldBg))
                              .arg(cssColor(m_mutedColor))
                              .arg(cssColor(m_accentColor))
                              .arg(cssColor(hover))
                              .arg(cssColor(accentHover));

    setStyleSheet(style);
    styleModernCombos(this);
    updateRoundedMask();
    update();
    emit themeChanged(m_surfaceColor, m_textColor);
}

void SmartSpaceWidget::setSimpleTheme(int mode, bool persist)
{
    const int normalized = (mode == 2 ? 2 : 1);
    if (m_themeMode != normalized) {
        m_themeMode = normalized;
        applyTheme();
    }
    if (persist)
        saveSettings();
}

void SmartSpaceWidget::refreshInheritedTheme()
{
    // Standalone solid themes do not inherit a desktop wallpaper or material.
}

void SmartSpaceWidget::applyMenuTheme(QMenu *menu) const
{
    if (!menu)
        return;

    // A QMenu is a separate native window.  On its first show UKUI can expose
    // the opaque backing rectangle for one frame before the stylesheet's
    // rounded background is polished.  Prepare translucency and a matching
    // mask before mapping the window so the first and later openings match.
    menu->setAttribute(Qt::WA_TranslucentBackground, true);
    menu->setAutoFillBackground(false);
    if (!menu->property("smartRoundedMenuPrepared").toBool()) {
        menu->setProperty("smartRoundedMenuPrepared", true);
        connect(menu, &QMenu::aboutToShow, menu, [menu] {
            auto applyRoundedMask = [menu] {
                if (menu->width() <= 1 || menu->height() <= 1)
                    return;
                QPainterPath path;
                path.addRoundedRect(QRectF(menu->rect()).adjusted(0.5, 0.5, -0.5, -0.5), 12.0,
                                    12.0);
                menu->setMask(QRegion(path.toFillPolygon().toPolygon()));
            };
            menu->ensurePolished();
            menu->adjustSize();
            applyRoundedMask();
            QTimer::singleShot(0, menu, applyRoundedMask);
        });
    }

    const bool extColors = (m_themeMode == 6 && m_customColorsEnabled);
    const qreal customOpacity = qBound(25, m_customOpacity, 100) / 100.0;
    auto customBackground = [customOpacity](QColor color) {
        color.setAlpha(qRound(color.alpha() * customOpacity));
        return color;
    };
    QColor menuBg =
        extColors && m_customMenuBg.isValid() ? customBackground(m_customMenuBg) : m_cardColor;
    if (m_themeMode == 3)
        menuBg = QColor(28, 33, 42, 255);
    QColor menuText = extColors && m_customMenuText.isValid() ? m_customMenuText : m_textColor;
    QColor menuHover = extColors && m_customMenuHover.isValid()
                           ? customBackground(m_customMenuHover)
                           : m_accentColor;
    menuHover.setAlpha(extColors && m_customMenuHover.isValid() ? menuHover.alpha() : 225);
    if (m_themeMode == 3)
        menuHover = QColor(48, 88, 118);

    QColor disabled = extColors && m_customMuted.isValid() ? m_customMuted : m_mutedColor;
    disabled.setAlpha(105);
    QColor separator = extColors && m_customBorder.isValid() ? m_customBorder : m_borderColor;
    separator.setAlpha(qMax(45, separator.alpha()));
    menu->setStyleSheet(QStringLiteral(R"(
        QMenu {
            background-color: %1; color: %2;
            border: 1px solid %3; border-radius: 12px; padding: 5px;
            icon-size: 20px;
        }
        QMenu::item {
            background-color: transparent; padding: 6px 24px 6px 12px;
            border-radius: 6px; min-height: 18px;
        }
        QMenu::item:selected { background-color: %4; color: %2; }
        QMenu::item:disabled { color: %5; background-color: transparent; }
        QMenu::separator {
            height: 1px; background-color: %6; margin: 4px 7px;
        }
        QMenu::indicator { width: 14px; height: 14px; }
    )")
                            .arg(cssColor(menuBg), cssColor(menuText), cssColor(separator),
                                 cssColor(menuHover), cssColor(disabled), cssColor(separator)));
}

void SmartSpaceWidget::configureIndexScheduling()
{
    m_periodicRefresh.stop();
    if (m_indexMode == 2) {
        m_periodicRefresh.setInterval(qBound(5, m_indexIntervalMinutes, 1440) * 60 * 1000);
        m_periodicRefresh.start();
    }
}

QWidget *SmartSpaceWidget::createPanel(const QString &title, QWidget *content,
                                       const QString &objectName)
{
    auto *panel = new QFrame;
    panel->setObjectName(objectName);
    QWidget *page = panel;
    if (objectName == QLatin1String("smartFilePanel")) {
        // Preview and results share the same material. Only one page paints at
        // a time, so transparent preview text never overlaps the result list.
        m_resultStack = new QStackedWidget(panel);
        m_resultStack->setObjectName(QStringLiteral("smartResultStack"));
        m_resultStack->setFrameShape(QFrame::NoFrame);
        m_resultPage = new QWidget(m_resultStack);
        m_resultPage->setObjectName(QStringLiteral("smartResultPage"));
        m_resultStack->addWidget(m_resultPage);
        auto *container = new QVBoxLayout(panel);
        container->setContentsMargins(0, 0, 0, 0);
        container->addWidget(m_resultStack);
        page = m_resultPage;
    }
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(6);
    auto *label = new QLabel(title, panel);
    label->setProperty("panelTitle", true);
    auto *titleRow = new QHBoxLayout;
    titleRow->setContentsMargins(0, 0, 0, 0);
    titleRow->setSpacing(4);
    auto *panelIcon = new QLabel(panel);
    panelIcon->setObjectName(QStringLiteral("smartPanelIcon"));
    panelIcon->setPixmap(modernRailGlyph(objectName == QLatin1String("smartFolderPanel")
                                             ? QStringLiteral("folder")
                                             : QStringLiteral("list"),
                                         QColor(QStringLiteral("#2563eb")))
                             .scaled(QSize(18, 18), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    titleRow->addWidget(panelIcon);
    titleRow->addWidget(label);
    titleRow->addStretch(1);
    if (objectName == QLatin1String("smartFolderPanel"))
        m_folderTitleLabel = label;
    else if (objectName == QLatin1String("smartFilePanel")) {
        m_fileTitleLabel = label;
        auto makeDensityButton = [panel, titleRow](const QString &text, const QString &tip) {
            auto *button = new QToolButton(panel);
            button->setText(text);
            button->setToolTip(tip);
            button->setProperty("panelControl", true);
            button->setFixedSize(28, 26);
            titleRow->addWidget(button);
            return button;
        };
        m_densityDownButton = makeDensityButton(QStringLiteral("−"),
                                                QStringLiteral("缩小文件与文件夹，显示更多内容"));
        m_densityUpButton =
            makeDensityButton(QStringLiteral("＋"), QStringLiteral("放大文件与文件夹"));
        m_densityDownButton->hide();
        m_densityUpButton->hide();
        connect(m_densityDownButton, &QToolButton::clicked, this,
                [this] { setResultDensity(m_resultDensity - 1); });
        connect(m_densityUpButton, &QToolButton::clicked, this,
                [this] { setResultDensity(m_resultDensity + 1); });
    }
    auto *count = new QLabel(QStringLiteral("0 项"), panel);
    count->setObjectName(QStringLiteral("smartPanelCount"));
    count->setAlignment(Qt::AlignCenter);
    count->setMinimumWidth(42);
    if (objectName == QLatin1String("smartFolderPanel"))
        m_folderCountLabel = count;
    else if (objectName == QLatin1String("smartFilePanel"))
        m_fileCountLabel = count;
    titleRow->insertWidget(2, count);
    layout->addLayout(titleRow);
    layout->addWidget(content, 1);
    return panel;
}

QListWidget *SmartSpaceWidget::createEntryList()
{
    auto *list = new SmartEntryList;
    list->setIconSize(QSize(22, 22));
    list->setSpacing(6);
    list->setUniformItemSizes(false);
    list->setMouseTracking(true);
    list->setToolTipDuration(15000);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setContextMenuPolicy(Qt::CustomContextMenu);
    return list;
}

void SmartSpaceWidget::rebuildFileScopePill()
{
    if (!m_scopePillLayout || !m_scopePillContainer)
        return;
    clearButtonLayout(m_scopePillLayout);
    auto *scope = new QToolButton(m_scopePillContainer);
    scope->setText(QStringLiteral("全部文件"));
    scope->setToolTip(QStringLiteral("当前检索范围：全部索引文件\n"
                                     "空间切换和保存检索规则已取消"));
    scope->setProperty("pill", true);
    scope->setEnabled(false);
    scope->setFixedWidth(154);
    m_scopePillLayout->addWidget(scope);
    m_scopePillLayout->addStretch(1);
}

void SmartSpaceWidget::rebuildCategoryPills()
{
    if (!m_categoryPillLayout || !m_categoryPillContainer || !m_categoryCombo)
        return;
    clearButtonLayout(m_categoryPillLayout);
    if (m_categoryButtonGroup) {
        delete m_categoryButtonGroup;
        m_categoryButtonGroup = nullptr;
    }
    auto *picker = new QToolButton(m_categoryPillContainer);
    QString summary = QStringLiteral("全部类型");
    if (m_selectedCategories.size() == 1)
        summary = categoryTitle(m_selectedCategories.first());
    else if (m_selectedCategories.size() > 1)
        summary = QStringLiteral("已选 %1 种类型").arg(m_selectedCategories.size());
    QStringList selectedTitles;
    for (const QString &category : m_selectedCategories)
        selectedTitles << categoryTitle(category);
    picker->setText(summary + QStringLiteral("  ⌄"));
    picker->setProperty("pill", true);
    picker->setPopupMode(QToolButton::InstantPopup);
    picker->setToolTip(
        m_selectedCategories.isEmpty()
            ? QStringLiteral("当前搜索全部文件类型")
            : QStringLiteral("已选：%1").arg(selectedTitles.join(QStringLiteral("、"))));
    auto *menu = new QMenu(picker);
    applyMenuTheme(menu);
    QAction *all = menu->addAction(
        QStringLiteral("%1  全部类型")
            .arg(m_selectedCategories.isEmpty() ? QStringLiteral("✔") : QStringLiteral("○")));
    connect(all, &QAction::triggered, this, [this] {
        m_selectedCategories.clear();
        rebuildCategoryPills();
        updateResults(true);
    });
    menu->addSeparator();
    for (int index = 1; index < m_categoryCombo->count(); ++index) {
        const QString category = m_categoryCombo->itemData(index).toString();
        const bool selected = m_selectedCategories.contains(category);
        QAction *action = menu->addAction(
            QStringLiteral("%1  %2").arg(selected ? QStringLiteral("✔") : QStringLiteral("○"),
                                         m_categoryCombo->itemText(index)));
        connect(action, &QAction::triggered, this, [this, category] {
            if (m_selectedCategories.contains(category))
                m_selectedCategories.removeAll(category);
            else
                m_selectedCategories << category;
            rebuildCategoryPills();
            updateResults(true);
        });
    }
    picker->setMenu(menu);
    m_categoryPillLayout->addWidget(picker);
    m_categoryPillLayout->addStretch(1);
}

void SmartSpaceWidget::showHeaderMenu()
{
    QMenu menu(this);
    menu.setWindowTitle(QStringLiteral("知识库"));
    applyMenuTheme(&menu);
    QAction *idle = menu.addAction(smartMenuIcon(QStringLiteral("fast-full")),
                                   QStringLiteral("① 全量扫描 · 建立文件清单"));
    QAction *textBuild = menu.addAction(smartMenuIcon(QStringLiteral("refresh")),
                                        QStringLiteral("② 文本建库 · 提取正文"));
    QAction *ocrIndex = menu.addAction(smartMenuIcon(QStringLiteral("ocr")),
                                       QStringLiteral("③ OCR补全 · 图片 / 扫描 PDF"));
    const bool taskBusy = m_libraryJobBusy || m_knowledgeProcess;
    for (auto *action : {idle, textBuild, ocrIndex})
        action->setEnabled(!taskBusy);
    menu.addSeparator();
    QAction *refresh = menu.addAction(smartMenuIcon(QStringLiteral("refresh")),
                                      QStringLiteral("增量更新 · 重新扫描并复用正文"));
    refresh->setEnabled(!taskBusy);
    menu.addSeparator();
    QAction *manager = menu.addAction(QIcon::fromTheme(QStringLiteral("folder-open")),
                                      QStringLiteral("知识库管理 / 导入与导出…"));
    manager->setObjectName("knowledgeManagerAction");
    connect(manager, &QAction::triggered, this, &SmartSpaceWidget::showKnowledgeManager);
    QAction *knowledgeBuild = menu.addAction(QIcon::fromTheme(QStringLiteral("system-search")),
                                             QStringLiteral("分阶段建库向导…"));
    QAction *knowledgeStats = menu.addAction(QIcon::fromTheme(QStringLiteral("view-statistics")),
                                             QStringLiteral("查看知识库状态"));
    QAction *knowledgeStop = menu.addAction(
        QIcon::fromTheme(QStringLiteral("media-playback-pause")), QStringLiteral("暂停知识库任务"));
    const bool knowledgeBusy = m_knowledgeProcess != nullptr;
    knowledgeBuild->setEnabled(!knowledgeBusy);
    knowledgeStats->setEnabled(true);
    knowledgeStop->setEnabled(knowledgeBusy);
    menu.addSeparator();
    QAction *settings =
        menu.addAction(smartMenuIcon(QStringLiteral("settings")), QStringLiteral("AI空间设置"));
    QAction *close =
        menu.addAction(smartMenuIcon(QStringLiteral("close")), QStringLiteral("关闭AI空间"));

    connect(refresh, &QAction::triggered, this, &SmartSpaceWidget::startIndexing);
    connect(idle, &QAction::triggered, this,
            [this] { startGuidedBuild(false, false, QStringLiteral("manifest")); });
    connect(textBuild, &QAction::triggered, this,
            [this] { startGuidedBuild(false, false, QStringLiteral("text")); });
    connect(ocrIndex, &QAction::triggered, this, &SmartSpaceWidget::startOcrIndex);
    connect(knowledgeBuild, &QAction::triggered, this, &SmartSpaceWidget::startKnowledgeBuild);
    connect(knowledgeStats, &QAction::triggered, this, &SmartSpaceWidget::showKnowledgeStatus);
    connect(knowledgeStop, &QAction::triggered, this, [this] {
        if (m_knowledgeProcess) {
            m_knowledgeProcess->setProperty("userPaused", true);
            terminateKnowledgeProcess(false);
            m_statusLabel->setText(QStringLiteral("正在暂停知识库任务 · 已完成内容会保留"));
        }
    });
    connect(settings, &QAction::triggered, this, &SmartSpaceWidget::showSettingsDialog);
    connect(close, &QAction::triggered, this, &SmartSpaceWidget::closeRequested);
    execMenuAt(menu, m_knowledgeMenuButton);
}

void SmartSpaceWidget::updateResponsiveLayout()
{
    if (!m_splitter)
        return;
    const bool compact = width() < 760;
    const bool breakpointChanged = property("smartCompactLayout").toBool() != compact;
    setProperty("smartCompactLayout", compact);
    if (m_folderPanel)
        m_folderPanel->setVisible(!compact);
    if (breakpointChanged) {
        rebuildFileScopePill();
        rebuildCategoryPills();
    }
    if (!compact && m_splitter->sizes().size() == 2) {
        const int available = qMax(1, m_splitter->width());
        if (m_splitter->sizes().at(0) < 80)
            m_splitter->setSizes({available * 34 / 100, available * 66 / 100});
    }
    const bool docked = width() >= 1100;
    if (m_previewPanel && m_resultStack) {
        if (docked && m_previewPanel->parentWidget() != m_splitter) {
            m_resultStack->setCurrentWidget(m_resultPage);
            m_previewPanel->setParent(m_splitter);
            m_splitter->addWidget(m_previewPanel);
        } else if (!docked && m_previewPanel->parentWidget() != m_resultStack) {
            m_previewPanel->setParent(m_resultStack);
            m_resultStack->addWidget(m_previewPanel);
        }
        if (docked) {
            m_resultStack->setCurrentWidget(m_resultPage);
            m_previewPanel->setVisible(m_previewVisible);
            if (m_previewVisible && !property("previewDocked").toBool())
                m_splitter->setSizes({width() * 3 / 12, width() * 5 / 12, width() * 4 / 12});
        } else
            m_resultStack->setCurrentWidget(
                m_previewVisible ? static_cast<QWidget *>(m_previewPanel) : m_resultPage);
        setProperty("previewDocked", docked && m_previewVisible);
        setProperty("previewVisible", m_previewVisible);
    }
}

void SmartSpaceWidget::restoreSettings()
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("smartSpace"));
    // Remove settings from the retired remote/semantic feature set.  This is
    // deliberately done during restore so old API keys do not remain in the
    // user's configuration after upgrading.
    const QStringList obsoleteKeys{
        QStringLiteral("aiEnabled"),        QStringLiteral("aiUseMonitorConfig"),
        QStringLiteral("aiApiUrl"),         QStringLiteral("aiApiKey"),
        QStringLiteral("aiModel"),          QStringLiteral("aiSendContent"),
        QStringLiteral("knowledgeEnabled"), QStringLiteral("knowledgeSendContent"),
        QStringLiteral("currentSpace")};
    for (const QString &key : obsoleteKeys)
        settings.remove(key);
    m_roots = settings.value(QStringLiteral("roots"), defaultRoots()).toStringList();
    m_excludedFolders = settings.value(QStringLiteral("excludedFolders")).toStringList();
    m_hiddenFolders = settings.value(QStringLiteral("hiddenFolders")).toStringList();
    m_indexedExtensions = settings.value(QStringLiteral("indexedExtensions")).toStringList();
    m_visibleExtensions = settings.value(QStringLiteral("visibleExtensions")).toStringList();
    m_ocrImages = settings.value(QStringLiteral("ocrImages"), true).toBool();
    m_maxItems = qBound(1000, settings.value(QStringLiteral("maxItems"), 25000).toInt(), 100000);
    m_maxContentPerFile =
        qBound(2000, settings.value(QStringLiteral("maxContentPerFile"), 12000).toInt(), 100000);
    m_maxTotalContentMb =
        qBound(1, settings.value(QStringLiteral("maxTotalContentMb"), 16).toInt(), 64);
    m_indexMode = qBound(0, settings.value(QStringLiteral("indexMode"), 0).toInt(), 2);
    m_indexIntervalMinutes =
        qBound(5, settings.value(QStringLiteral("indexIntervalMinutes"), 30).toInt(), 1440);
    const int storedThemeMode =
        qBound(0, settings.value(QStringLiteral("themeMode"), 3).toInt(), 6);
    m_customBaseSkin = qBound(0, settings.value(QStringLiteral("customBaseSkin"), 0).toInt(), 4);
    m_themeMode = (storedThemeMode == 2 ? 2 : 1);
    m_customOpacity = qBound(25, settings.value(QStringLiteral("customOpacity"), 100).toInt(), 100);
    m_customColorsEnabled = settings.value(QStringLiteral("customColorsEnabled"), false).toBool();
    m_customSurface = QColor(settings.value(QStringLiteral("customSurface")).toString());
    m_customCard = QColor(settings.value(QStringLiteral("customCard")).toString());
    m_customBorder = QColor(settings.value(QStringLiteral("customBorder")).toString());
    m_customText = QColor(settings.value(QStringLiteral("customText")).toString());
    m_customMuted = QColor(settings.value(QStringLiteral("customMuted")).toString());
    m_customAccent = QColor(settings.value(QStringLiteral("customAccent")).toString());
    m_customSearchBg = QColor(settings.value(QStringLiteral("customSearchBg")).toString());
    m_customSearchText = QColor(settings.value(QStringLiteral("customSearchText")).toString());
    m_customButtonBg = QColor(settings.value(QStringLiteral("customButtonBg")).toString());
    m_customButtonText = QColor(settings.value(QStringLiteral("customButtonText")).toString());
    m_customButtonHover = QColor(settings.value(QStringLiteral("customButtonHover")).toString());
    m_customMenuBg = QColor(settings.value(QStringLiteral("customMenuBg")).toString());
    m_customMenuText = QColor(settings.value(QStringLiteral("customMenuText")).toString());
    m_customMenuHover = QColor(settings.value(QStringLiteral("customMenuHover")).toString());
    m_customFontFamily = settings.value(QStringLiteral("customFontFamily")).toString();
    m_customFontSize = qBound(10, settings.value(QStringLiteral("customFontSize"), 13).toInt(), 18);
    m_customFontBold = settings.value(QStringLiteral("customFontBold"), false).toBool();
    m_resultDensity =
        qBound(kMinResultDensity, settings.value(QStringLiteral("resultDensity"), 0).toInt(),
               kMaxResultDensity);
    m_resultSortMode = qBound(0, settings.value(QStringLiteral("resultSortMode"), 0).toInt(), 2);
    m_autoStart = settings.value(QStringLiteral("autoStart"), false).toBool();
    m_alwaysOnTop = settings.value(QStringLiteral("alwaysOnTop"), false).toBool();
    settings.remove(QStringLiteral("defaultHidden"));
    m_knowledgeDirectory =
        settings
            .value(QStringLiteral("knowledgeDirectory"),
                   QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
                       QStringLiteral("/ai-space/knowledge"))
            .toString();
    m_libraryStorageDirectory =
        settings.value(QStringLiteral("libraryStorageDirectory"), m_knowledgeDirectory).toString();
    m_storageConfirmed = settings.value(QStringLiteral("storageConfirmed"), false).toBool();
    m_storageReserveGiB =
        qBound(1, settings.value(QStringLiteral("storageReserveGiB"), 2).toInt(), 1024);
    m_libraryQuotaGiB =
        qBound(1, settings.value(QStringLiteral("libraryQuotaGiB"), 32).toInt(), 4096);
    m_ocrCpuPercent = qBound(5, settings.value(QStringLiteral("ocrCpuPercent"), 25).toInt(), 100);
    m_ocrLanguages =
        settings.value(QStringLiteral("ocrLanguages"), QStringLiteral("chi_sim+eng")).toString();
    m_ocrMemoryMiB =
        qBound(512, settings.value(QStringLiteral("ocrMemoryMiB"), 1024).toInt(), 8192);
    m_knowledgeChunkSize =
        qBound(600, settings.value(QStringLiteral("knowledgeChunkSize"), 1200).toInt(), 3000);
    m_knowledgeOverlap = qBound(0, settings.value(QStringLiteral("knowledgeOverlap"), 120).toInt(),
                                m_knowledgeChunkSize / 3);
    m_providerApprovedDigest = settings.value(QStringLiteral("providerApprovedDigest")).toString();
    m_providerConfig =
        settings.value(QStringLiteral("providerConfig"), providerConfigPath()).toString();
    const QSize storedSize =
        settings.value(QStringLiteral("size"), QSize(kDefaultWidth, kDefaultHeight)).toSize();
    resize(storedSize.expandedTo(minimumSize()));
    QPoint position = settings.value(QStringLiteral("position"), QPoint(-1, -1)).toPoint();
    settings.endGroup();
    if (m_densityDownButton)
        m_densityDownButton->setEnabled(m_resultDensity > kMinResultDensity);
    if (m_densityUpButton)
        m_densityUpButton->setEnabled(m_resultDensity < kMaxResultDensity);
    if (m_densityDownButton)
        m_densityDownButton->setToolTip(QStringLiteral("缩小文件与文件夹 · 当前第 %1/%2 档")
                                            .arg(m_resultDensity - kMinResultDensity + 1)
                                            .arg(kResultDensityLevels));
    if (m_densityUpButton)
        m_densityUpButton->setToolTip(QStringLiteral("放大文件与文件夹 · 当前第 %1/%2 档")
                                          .arg(m_resultDensity - kMinResultDensity + 1)
                                          .arg(kResultDensityLevels));
    if (m_sortCombo) {
        const QSignalBlocker blocker(m_sortCombo);
        m_sortCombo->setCurrentIndex(m_resultSortMode);
    }

    const QString testRoots = QString::fromUtf8(qgetenv("AI_SPACE_ROOTS"));
    if (!testRoots.isEmpty())
        m_roots = testRoots.split(QDir::listSeparator(), QString::SkipEmptyParts);
    if (qEnvironmentVariableIsSet("AI_SPACE_OCR"))
        m_ocrImages = qEnvironmentVariableIntValue("AI_SPACE_OCR") != 0;

    if ((position.x() < 0 || position.y() < 0)) {
        const QSize area = parentWidget() ? parentWidget()->size() : QSize(1920, 1080);
        position = QPoint(48, std::max(64, area.height() - height() - 100));
    }
    {
        move(boundedPosition(position));
    }
}

void SmartSpaceWidget::saveSettings() const
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("smartSpace"));
    settings.setValue(QStringLiteral("roots"), m_roots);
    settings.setValue(QStringLiteral("excludedFolders"), m_excludedFolders);
    settings.setValue(QStringLiteral("hiddenFolders"), m_hiddenFolders);
    settings.setValue(QStringLiteral("indexedExtensions"), m_indexedExtensions);
    settings.setValue(QStringLiteral("visibleExtensions"), m_visibleExtensions);
    settings.setValue(QStringLiteral("ocrImages"), m_ocrImages);
    settings.setValue(QStringLiteral("maxItems"), m_maxItems);
    settings.setValue(QStringLiteral("maxContentPerFile"), m_maxContentPerFile);
    settings.setValue(QStringLiteral("maxTotalContentMb"), m_maxTotalContentMb);
    settings.setValue(QStringLiteral("indexMode"), m_indexMode);
    settings.setValue(QStringLiteral("indexIntervalMinutes"), m_indexIntervalMinutes);
    settings.setValue(QStringLiteral("themeMode"), m_themeMode);
    settings.remove(QStringLiteral("glassOptics"));
    settings.setValue(QStringLiteral("customBaseSkin"), m_customBaseSkin);
    settings.setValue(QStringLiteral("customOpacity"), m_customOpacity);
    settings.setValue(QStringLiteral("customColorsEnabled"), m_customColorsEnabled);
    settings.setValue(QStringLiteral("customSurface"), m_customSurface.isValid()
                                                           ? m_customSurface.name(QColor::HexArgb)
                                                           : QString());
    settings.setValue(QStringLiteral("customCard"),
                      m_customCard.isValid() ? m_customCard.name(QColor::HexArgb) : QString());
    settings.setValue(QStringLiteral("customBorder"),
                      m_customBorder.isValid() ? m_customBorder.name(QColor::HexArgb) : QString());
    settings.setValue(QStringLiteral("customText"),
                      m_customText.isValid() ? m_customText.name(QColor::HexArgb) : QString());
    settings.setValue(QStringLiteral("customMuted"),
                      m_customMuted.isValid() ? m_customMuted.name(QColor::HexArgb) : QString());
    settings.setValue(QStringLiteral("customAccent"),
                      m_customAccent.isValid() ? m_customAccent.name(QColor::HexArgb) : QString());
    settings.setValue(QStringLiteral("customSearchBg"), m_customSearchBg.isValid()
                                                            ? m_customSearchBg.name(QColor::HexArgb)
                                                            : QString());
    settings.setValue(QStringLiteral("customSearchText"),
                      m_customSearchText.isValid() ? m_customSearchText.name(QColor::HexArgb)
                                                   : QString());
    settings.setValue(QStringLiteral("customButtonBg"), m_customButtonBg.isValid()
                                                            ? m_customButtonBg.name(QColor::HexArgb)
                                                            : QString());
    settings.setValue(QStringLiteral("customButtonText"),
                      m_customButtonText.isValid() ? m_customButtonText.name(QColor::HexArgb)
                                                   : QString());
    settings.setValue(QStringLiteral("customButtonHover"),
                      m_customButtonHover.isValid() ? m_customButtonHover.name(QColor::HexArgb)
                                                    : QString());
    settings.setValue(QStringLiteral("customMenuBg"),
                      m_customMenuBg.isValid() ? m_customMenuBg.name(QColor::HexArgb) : QString());
    settings.setValue(QStringLiteral("customMenuText"), m_customMenuText.isValid()
                                                            ? m_customMenuText.name(QColor::HexArgb)
                                                            : QString());
    settings.setValue(QStringLiteral("customMenuHover"),
                      m_customMenuHover.isValid() ? m_customMenuHover.name(QColor::HexArgb)
                                                  : QString());
    settings.setValue(QStringLiteral("customFontFamily"), m_customFontFamily);
    settings.setValue(QStringLiteral("customFontSize"), m_customFontSize);
    settings.setValue(QStringLiteral("customFontBold"), m_customFontBold);
    settings.setValue(QStringLiteral("resultDensity"), m_resultDensity);
    settings.setValue(QStringLiteral("resultSortMode"), m_resultSortMode);
    settings.setValue(QStringLiteral("autoStart"), m_autoStart);
    settings.setValue(QStringLiteral("alwaysOnTop"), m_alwaysOnTop);
    settings.remove(QStringLiteral("defaultHidden"));
    settings.setValue(QStringLiteral("knowledgeDirectory"), m_knowledgeDirectory);
    settings.setValue(QStringLiteral("libraryStorageDirectory"), m_libraryStorageDirectory);
    settings.setValue(QStringLiteral("storageConfirmed"), m_storageConfirmed);
    settings.setValue(QStringLiteral("storageReserveGiB"), m_storageReserveGiB);
    settings.setValue(QStringLiteral("libraryQuotaGiB"), m_libraryQuotaGiB);
    settings.setValue(QStringLiteral("ocrCpuPercent"), m_ocrCpuPercent);
    settings.setValue(QStringLiteral("ocrMemoryMiB"), m_ocrMemoryMiB);
    settings.setValue(QStringLiteral("ocrLanguages"), m_ocrLanguages);
    settings.setValue(QStringLiteral("knowledgeChunkSize"), m_knowledgeChunkSize);
    settings.setValue(QStringLiteral("knowledgeOverlap"), m_knowledgeOverlap);
    settings.setValue(QStringLiteral("providerConfig"), m_providerConfig);
    settings.setValue(QStringLiteral("providerApprovedDigest"), m_providerApprovedDigest);
    {
        const QRect normal =
            isWindowMaximized() && m_normalGeometry.isValid() ? m_normalGeometry : geometry();
        settings.setValue(QStringLiteral("position"), normal.topLeft());
        settings.setValue(QStringLiteral("size"), normal.size());
    }
    settings.endGroup();
    settings.sync();
}

bool SmartSpaceWidget::autoStartEnabled()
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("smartSpace"));
    const bool enabled = settings.value(QStringLiteral("autoStart"), false).toBool();
    settings.endGroup();
    return enabled;
}

void SmartSpaceWidget::setAutoStart(bool enabled)
{
    if (m_autoStart == enabled) {
        if (enabled)
            emit autoStartChanged(enabled);
        return;
    }
    m_autoStart = enabled;
    saveSettings();
    emit autoStartChanged(enabled);
}

void SmartSpaceWidget::setAlwaysOnTop(bool enabled)
{
    if (m_alwaysOnTop == enabled) {
        updateActionState();
        return;
    }
    m_alwaysOnTop = enabled;
    updateActionState();
    saveSettings();
    emit alwaysOnTopChanged(enabled);
    // The controller has recreated the native surface when this returns.
    updateRoundedMask();
    update();
}

void SmartSpaceWidget::setResultDensity(int density)
{
    const int bounded = qBound(kMinResultDensity, density, kMaxResultDensity);
    if (m_resultDensity == bounded)
        return;
    m_resultDensity = bounded;
    if (m_densityDownButton) {
        m_densityDownButton->setEnabled(m_resultDensity > kMinResultDensity);
        m_densityDownButton->setToolTip(QStringLiteral("缩小文件与文件夹 · 当前第 %1/%2 档")
                                            .arg(m_resultDensity - kMinResultDensity + 1)
                                            .arg(kResultDensityLevels));
    }
    if (m_densityUpButton) {
        m_densityUpButton->setEnabled(m_resultDensity < kMaxResultDensity);
        m_densityUpButton->setToolTip(QStringLiteral("放大文件与文件夹 · 当前第 %1/%2 档")
                                          .arg(m_resultDensity - kMinResultDensity + 1)
                                          .arg(kResultDensityLevels));
    }
    updateFolderDensity();
    updateResults(false);
    saveSettings();
}

void SmartSpaceWidget::recreateNativeSurface(bool translucent)
{
    // WA_TranslucentBackground affects the X11 visual chosen while the native
    // window is created.  Reparenting an already-created 24-bit child window
    // is insufficient: Qt otherwise keeps the old visual and the alpha edge
    // is discarded.  Destroy only native resources; QObject/widget state and
    // the index remain intact and children are recreated lazily on show().
    const bool wasVisible = isVisible();
    if (wasVisible)
        hide();
    destroy(true, true);
    const bool glassSurface = false;
    // Only top-level windows get an ARGB visual. Desktop children share the
    // canvas backing store and must preserve its pixels outside the material.
    setAttribute(Qt::WA_TranslucentBackground, translucent);
    setAttribute(Qt::WA_NoSystemBackground, translucent);
    setAttribute(Qt::WA_OpaquePaintEvent, !translucent && !glassSurface);
    setAttribute(Qt::WA_StyledBackground, false);
    setAutoFillBackground(!(translucent || glassSurface));
    updateRoundedMask();
    if (wasVisible)
        show();
}

QPoint SmartSpaceWidget::boundedPosition(const QPoint &position) const
{
    QRect bounds;
    if (isWindow()) {
        QScreen *screen = QGuiApplication::screenAt(QRect(position, size()).center());
        if (!screen)
            screen = QGuiApplication::primaryScreen();
        if (screen)
            bounds = screen->availableGeometry();
    } else if (parentWidget()) {
        bounds = parentWidget()->rect();
    }
    if (!bounds.isValid())
        return position;

    int x =
        qBound(bounds.left(), position.x(), std::max(bounds.left(), bounds.right() - width() + 1));
    int y =
        qBound(bounds.top(), position.y(), std::max(bounds.top(), bounds.bottom() - height() + 1));
    constexpr int snapDistance = 32;
    if (qAbs(x - bounds.left()) <= snapDistance)
        x = bounds.left();
    if (qAbs(x + width() - bounds.right() - 1) <= snapDistance)
        x = bounds.right() - width() + 1;
    if (qAbs(y - bounds.top()) <= snapDistance)
        y = bounds.top();
    if (qAbs(y + height() - bounds.bottom() - 1) <= snapDistance)
        y = bounds.bottom() - height() + 1;
    return QPoint(x, y);
}

void SmartSpaceWidget::updateRoundedMask()
{
    if (isWindow()) {
        // Top-level windows use an ARGB backing surface.  Keeping the
        // integer QRegion here would discard antialiased edge alpha.
        clearMask();
        return;
    }
    QPainterPath path;
    path.addRoundedRect(rect(), 12, 12);
    setMask(QRegion(path.toFillPolygon().toPolygon()));
}

QString SmartSpaceWidget::indexPath() const
{
    const QString catalog =
        QDir(m_knowledgeDirectory).filePath(QStringLiteral("catalog/index.json"));
    if (QFileInfo::exists(catalog))
        return catalog;
    const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    return cache + QStringLiteral("/smart-space/index.json");
}

QString SmartSpaceWidget::uiStreamIndexPath() const
{
    return indexPath() + QStringLiteral(".ui.bin.gz");
}

QString SmartSpaceWidget::resumeIndexPath() const
{
    return indexPath() + QStringLiteral(".fast.resume.jsonl");
}

QString SmartSpaceWidget::ocrResumeIndexPath() const
{
    return indexPath() + QStringLiteral(".ocr.resume.jsonl");
}

QString SmartSpaceWidget::providerConfigPath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) +
           QStringLiteral("/ai-space/providers.json");
}

QString SmartSpaceWidget::indexerPath() const
{
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QStringList candidates{
        appDir.filePath(QStringLiteral("libexec/ai-space/smart_space_indexer.py")),
        appDir.filePath(QStringLiteral(AI_SPACE_LIBEXEC_RELATIVE "/smart_space_indexer.py"))};
    for (const QString &candidate : candidates) {
        if (QFileInfo::exists(candidate))
            return QDir::cleanPath(candidate);
    }
    return QString();
}

QString SmartSpaceWidget::knowledgeWorkerPath() const
{
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QStringList candidates{
        appDir.filePath(QStringLiteral("libexec/ai-space/smart_space_knowledge.py")),
        appDir.filePath(QStringLiteral(AI_SPACE_LIBEXEC_RELATIVE "/smart_space_knowledge.py"))};
    for (const QString &candidate : candidates) {
        if (QFileInfo::exists(candidate))
            return QDir::cleanPath(candidate);
    }
    return QString();
}

QString SmartSpaceWidget::knowledgeDbPath() const
{
    QString directory = m_knowledgeDirectory.trimmed();
    if (directory.isEmpty()) {
        directory = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
                    QStringLiteral("/ai-space/knowledge");
    }
    const QString db = QDir(directory).filePath(QStringLiteral("smart-space-knowledge.sqlite"));
    if (!QFileInfo::exists(db)) {
        QSettings legacy(QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) +
                             QStringLiteral("/kylin/ukui-fences.ini"),
                         QSettings::IniFormat);
        const QString old =
            QDir(legacy
                     .value(QStringLiteral("smartSpace/knowledgeDirectory"),
                            QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
                                QStringLiteral("/ukui-fences/knowledge"))
                     .toString())
                .filePath(QStringLiteral("smart-space-knowledge.sqlite"));
        if (QFileInfo::exists(old))
            return old;
    }
    return db;
}

QString SmartSpaceWidget::skillDirectoryPath() const
{
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QStringList candidates{
        appDir.filePath(QStringLiteral("share/ai-space/skills/ai-space-index-query")),
        appDir.filePath(QStringLiteral(AI_SPACE_SKILL_RELATIVE))};
    for (const auto &candidate : candidates) {
        const auto clean = QDir::cleanPath(candidate);
        if (QFileInfo(QDir(clean).filePath(QStringLiteral("SKILL.md"))).isFile())
            return clean;
    }
    return {};
}

void SmartSpaceWidget::showSettings()
{
    showSettingsDialog();
}

void SmartSpaceWidget::startFastFullIndex()
{
    startIdleFullIndex();
}

void SmartSpaceWidget::hideFolder(const QString &path)
{
    hideFolderFromNavigation(path);
}

void SmartSpaceWidget::excludeFolder(const QString &path)
{
    excludeFolderFromIndex(path);
}

void SmartSpaceWidget::startIdleFullIndex()
{
    if (!m_storageConfirmed)
        showKnowledgeManager();
    else
        startGuidedBuild(false);
}
void SmartSpaceWidget::startOcrIndex()
{
    if (!m_storageConfirmed)
        showKnowledgeManager();
    else
        startGuidedBuild(false, true);
}

void SmartSpaceWidget::stopIdleFullIndex()
{
    if (!m_knowledgeProcess)
        return;
    m_knowledgeProcess->setProperty("userPaused", true);
    terminateKnowledgeProcess(false);
}

bool SmartSpaceWidget::applyIndexSnapshot(IndexSnapshot snapshot)
{
    QElapsedTimer elapsed;
    elapsed.start();
    setProperty("indexLoading", false);
    if (!snapshot.valid) {
        if (!indexBusy())
            m_statusLabel->setText(snapshot.error);
        return false;
    }
    // Exclusions may have changed while the initial snapshot was being read.
    auto &entries = snapshot.entries;
    entries.erase(
        std::remove_if(entries.begin(), entries.end(),
                       [this](const SmartSpaceEntry &entry) { return pathExcluded(entry.path); }),
        entries.end());
    m_entries = std::move(entries);
    m_libraryOnlyEntryCount = 0;
    updateResults(true);
    rebuildWatches();
    if (!indexBusy())
        m_statusLabel->setText(snapshot.statusPrefix + QString::number(m_entries.size()) +
                               snapshot.statusSuffix);
    if (qEnvironmentVariableIsSet("UKUI_FENCES_STARTUP_TRACE"))
        qInfo() << "[AiSpaceStartup] index-applied" << elapsed.elapsed() << "ms";
    setProperty("indexApplyMs", elapsed.elapsed());
    if (!m_executedQuery.isEmpty() && !m_searchUsesRegex)
        startKnowledgeSearch(m_executedQuery);
    emit indexLoaded();
#if defined(__GLIBC__)
    QTimer::singleShot(0, [] { malloc_trim(0); });
#endif
    return true;
}

bool SmartSpaceWidget::loadIndex()
{
    ++m_indexLoadRevision; // Retire an initial read when a newer index is published.
    return applyIndexSnapshot(readIndexSnapshot(indexPath(), uiStreamIndexPath(),
                                                ocrResumeIndexPath(), resumeIndexPath(),
                                                m_excludedFolders, m_maxItems));
}

void SmartSpaceWidget::loadIndexAsync()
{
    const auto revision = ++m_indexLoadRevision;
    const QString canonical = indexPath(), stream = uiStreamIndexPath();
    const QString ocr = ocrResumeIndexPath(), resume = resumeIndexPath();
    const int maxItems = m_maxItems;
    setProperty("indexLoading", true);
    m_statusLabel->setText(QStringLiteral("正在加载本地索引…"));
    rebuildWatches();
    BackgroundTask::run(
        this,
        [=] {
            QElapsedTimer elapsed;
            elapsed.start();
            // Apply the current exclusions on delivery, including folders restored
            // while I/O was pending. Do not freeze the constructor's scope here.
            auto snapshot = readIndexSnapshot(canonical, stream, ocr, resume, {}, maxItems);
            if (qEnvironmentVariableIsSet("UKUI_FENCES_STARTUP_TRACE"))
                qInfo() << "[AiSpaceStartup] index-read" << elapsed.elapsed() << "ms"
                        << snapshot.entries.size() << "entries";
            return snapshot;
        },
        [this, revision](const IndexSnapshot &snapshot) {
            if (revision == m_indexLoadRevision)
                applyIndexSnapshot(snapshot);
        });
}

void SmartSpaceWidget::rebuildWatches()
{
    if (!m_watcher)
        return;
    const QStringList oldDirs = m_watcher->directories();
    if (!oldDirs.isEmpty())
        m_watcher->removePaths(oldDirs);

    // Manual and periodic updates do not consume file-change events. Avoid
    // thousands of directory stats/watches on every startup in those modes.
    if (m_indexMode != 1)
        return;

    QStringList paths;
    QSet<QString> seenPaths;
    for (const QString &root : m_roots) {
        if (QDir(root).exists() && !pathExcluded(root) && !seenPaths.contains(root)) {
            paths << root;
            seenPaths.insert(root);
        }
    }
    for (const SmartSpaceEntry &entry : m_entries) {
        if (!entry.isDir || pathExcluded(entry.path))
            continue;
        if (paths.size() >= kMaxWatchedDirectories)
            break;
        if (QDir(entry.path).exists() && !seenPaths.contains(entry.path)) {
            paths << entry.path;
            seenPaths.insert(entry.path);
        }
    }
    m_watcher->addPaths(paths);
}

void SmartSpaceWidget::setKnowledgeMigrationPending(bool pending)
{
    m_uiState.migrationPending = pending;
    setProperty("knowledgeMigrationPending", pending);
    auto *label = findChild<QLabel *>("knowledgeMigrationStatus");
    if (!label && pending) {
        label = new QLabel(QStringLiteral("旧知识库正在后台导入 · 文件检索可正常使用"),
                           m_contentContainer);
        label->setObjectName("knowledgeMigrationStatus");
        label->setWordWrap(true);
        m_contentContainer->layout()->addWidget(label);
    }
    if (label)
        label->setVisible(pending);
}

void SmartSpaceWidget::scheduleReindex()
{
    if (m_indexMode != 1 || !m_automaticIndexingReady)
        return;
    if (m_libraryJobBusy || m_uiState.managerVisible || m_settingsViewVisible) {
        m_reindexPending = true;
        return;
    }
    m_reindexDebounce.start();
}

void SmartSpaceWidget::updateActionState()
{
    if (!m_pinButton)
        return;
    const QSignalBlocker blocker(m_pinButton);
    m_pinButton->setChecked(m_alwaysOnTop);
    m_pinButton->setText(m_alwaysOnTop ? QStringLiteral("已置顶") : QStringLiteral("置顶"));
    m_pinButton->setToolTip(m_alwaysOnTop ? QStringLiteral("取消置顶，作为普通应用窗口显示")
                                          : QStringLiteral("让AI空间悬浮在所有应用上方"));
}

bool SmartSpaceWidget::entryMatches(const SmartSpaceEntry &entry) const
{
    if (pathExcluded(entry.path) || (!entry.isDir && !extensionVisible(entry)))
        return false;
    if (!m_executedQuery.isEmpty()) {
        if (m_searchUsesRegex) {
            const bool matches = m_searchExpression.match(entry.name).hasMatch() ||
                                 m_searchExpression.match(entry.path).hasMatch() ||
                                 m_searchExpression.match(entry.content).hasMatch();
            if (!matches)
                return false;
        } else {
            for (const QString &term : m_searchTerms) {
                const bool matches = entry.name.contains(term, Qt::CaseInsensitive) ||
                                     entry.path.contains(term, Qt::CaseInsensitive) ||
                                     entry.content.contains(term, Qt::CaseInsensitive);
                if (!matches)
                    return false;
            }
        }
    }

    if (!categoryMatches(entry))
        return false;
    return true;
}

bool SmartSpaceWidget::fileMatchesCurrentResults(const SmartSpaceEntry &entry,
                                                 bool respectScope) const
{
    if (entry.isDir)
        return false;
    const bool knowledgeMatch =
        !m_executedQuery.isEmpty() && m_knowledgeScores.contains(entry.path);
    const bool visibleKnowledgeMatch = knowledgeMatch && !pathExcluded(entry.path) &&
                                       extensionVisible(entry) && categoryMatches(entry);
    if (!entryMatches(entry) && !visibleKnowledgeMatch)
        return false;
    if (respectScope && !pathInsideScope(entry.path))
        return false;
    return true;
}

bool SmartSpaceWidget::commitSearchQuery(const QString &query, bool showError)
{
    const QString normalized = query.trimmed();
    const bool regexMode = normalized.startsWith(QStringLiteral("re:"), Qt::CaseInsensitive);
    QRegularExpression expression;
    QStringList terms;
    if (regexMode) {
        const QString pattern = normalized.mid(3).trimmed();
        expression =
            QRegularExpression(pattern, QRegularExpression::CaseInsensitiveOption |
                                            QRegularExpression::UseUnicodePropertiesOption);
        if (pattern.isEmpty() || !expression.isValid()) {
            if (showError && m_statusLabel) {
                m_statusLabel->setText(
                    pattern.isEmpty()
                        ? QStringLiteral("正则表达式为空，请在 re: 后输入规则")
                        : QStringLiteral("正则表达式无效：%1").arg(expression.errorString()));
            }
            return false;
        }
    } else if (!normalized.isEmpty()) {
        terms =
            normalized.split(QRegularExpression(QStringLiteral("\\s+")), QString::SkipEmptyParts);
    }
    m_executedQuery = normalized;
    m_searchUsesRegex = regexMode;
    m_searchExpression = expression;
    m_searchTerms = terms;
    return true;
}

void SmartSpaceWidget::executeSearch()
{
    if (!m_searchEdit || !commitSearchQuery(m_searchEdit->text(), true))
        return;
    ++m_knowledgeSearchRevision;
    if (m_knowledgeSearchProcess) {
        m_knowledgeSearchProcess->disconnect(this);
        m_knowledgeSearchProcess->kill();
        m_knowledgeSearchProcess->deleteLater();
        m_knowledgeSearchProcess = nullptr;
    }
    hidePreview();
    m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(),
                                   [](const SmartSpaceEntry &entry) { return entry.libraryOnly; }),
                    m_entries.end());
    m_libraryOnlyEntryCount = 0;
    for (auto &entry : m_entries) {
        entry.libraryReference.clear();
        entry.libraryDb.clear();
    }
    m_knowledgeScores.clear();
    m_knowledgeExtractionStates.clear();
    m_knowledgeAvailability.clear();
    m_knowledgeSnippets.clear();
    updateResults(true);
    if (!m_searchUsesRegex && !m_executedQuery.isEmpty())
        startKnowledgeSearch(m_executedQuery);
}

bool SmartSpaceWidget::categoryMatches(const SmartSpaceEntry &entry) const
{
    return m_selectedCategories.isEmpty() || m_selectedCategories.contains(entry.category);
}

QString SmartSpaceWidget::entryToolTip(const SmartSpaceEntry &entry) const
{
    const QString body = relevantSnippetHtml(entry, 3, 42, 86);
    const QString ocr = entry.ocrStatus == QLatin1String("pending")
                            ? QStringLiteral("<br><span style='color:#d97706'><b>待检测/OCR</b> · "
                                             "会先检测 PDF 文本层，必要时才运行 OCR</span>")
                            : QString();
    return QStringLiteral("<qt><div style='min-width:320px;max-width:460px'>"
                          "<b>%1</b>%2<br><br>%3</div></qt>")
        .arg(entry.name.toHtmlEscaped(), ocr, body);
}

QString SmartSpaceWidget::relevantSnippetHtml(const SmartSpaceEntry &entry, int maxContexts,
                                              int before, int after) const
{
    const QString content = entry.content.simplified();
    QString snippetQuery = m_executedQuery;
    if (m_searchUsesRegex && snippetQuery.startsWith(QStringLiteral("re:"), Qt::CaseInsensitive))
        snippetQuery = snippetQuery.mid(3);
    const QStringList terms = localQueryTerms(snippetQuery);

    struct Hit {
        int start;
        int length;
    };
    QVector<Hit> hits;
    for (const QString &term : terms) {
        if (term.size() < 2)
            continue;
        int from = 0;
        while (from < content.size() && hits.size() < 96) {
            const int at = content.indexOf(term, from, Qt::CaseInsensitive);
            if (at < 0)
                break;
            hits.append({at, term.size()});
            from = at + qMax(1, term.size());
        }
    }
    std::sort(hits.begin(), hits.end(), [](const Hit &left, const Hit &right) {
        return left.start == right.start ? left.length > right.length : left.start < right.start;
    });

    auto highlighted = [this, &terms](const QString &value) {
        QString html;
        int offset = 0;
        while (offset < value.size()) {
            int next = -1;
            int length = 0;
            for (const QString &term : terms) {
                const int at = value.indexOf(term, offset, Qt::CaseInsensitive);
                if (at >= 0 && (next < 0 || at < next || (at == next && term.size() > length))) {
                    next = at;
                    length = term.size();
                }
            }
            if (next < 0) {
                html += value.mid(offset).toHtmlEscaped();
                break;
            }
            html += value.mid(offset, next - offset).toHtmlEscaped();
            html += QStringLiteral("<b style='color:%1'>%2</b>")
                        .arg(m_accentColor.name(), value.mid(next, length).toHtmlEscaped());
            offset = next + length;
        }
        return html;
    };

    QStringList snippets;
    int previousEnd = -1;
    for (const Hit &hit : hits) {
        const int start = qMax(0, hit.start - qMax(0, before));
        const int end = qMin(content.size(), hit.start + hit.length + qMax(0, after));
        if (start <= previousEnd)
            continue;
        QString excerpt = highlighted(content.mid(start, end - start));
        if (start > 0)
            excerpt.prepend(QStringLiteral("…"));
        if (end < content.size())
            excerpt.append(QStringLiteral("…"));
        snippets << excerpt;
        previousEnd = end;
        if (snippets.size() == qMax(1, maxContexts))
            break;
    }
    if (snippets.isEmpty() && !content.isEmpty()) {
        const int fallbackLength = qMax(80, before + after);
        QString excerpt = content.left(fallbackLength).toHtmlEscaped();
        if (content.size() > fallbackLength)
            excerpt.append(QStringLiteral("…"));
        snippets << excerpt;
    }

    return snippets.isEmpty()
               ? (entry.ocrStatus == QLatin1String("pending")
                      ? QStringLiteral("待检测/OCR：已收录文件信息，PDF 将先检测文本层。")
                      : QStringLiteral("当前索引没有可展示的正文片段。"))
               : snippets.join(QStringLiteral("<br><br>"));
}

bool SmartSpaceWidget::pathInsideScope(const QString &path) const
{
    return m_scopePath.isEmpty() || pathIsInside(path, m_scopePath);
}

bool SmartSpaceWidget::pathExcluded(const QString &path) const
{
    for (const QString &excluded : m_excludedFolders) {
        if (pathIsInside(path, excluded))
            return true;
    }
    return false;
}

bool SmartSpaceWidget::folderHidden(const QString &path) const
{
    const QString clean = normalizedPath(path);
    for (const QString &hidden : m_hiddenFolders) {
        if (clean == normalizedPath(hidden))
            return true;
    }
    return false;
}

bool SmartSpaceWidget::extensionVisible(const SmartSpaceEntry &entry) const
{
    return entry.isDir || formatAllowed(entry.suffix, m_visibleExtensions);
}

void SmartSpaceWidget::hideFolderFromNavigation(const QString &path)
{
    const QString clean = normalizedPath(path);
    if (clean.isEmpty() || m_hiddenFolders.contains(clean))
        return;
    m_hiddenFolders << clean;
    if (!m_scopePath.isEmpty() && pathIsInside(m_scopePath, clean))
        m_scopePath.clear();
    saveSettings();
    updateResults(true);
    QString displayName = QFileInfo(clean).fileName();
    if (displayName.isEmpty())
        displayName = clean;
    m_statusLabel->setText(
        QStringLiteral("已从左侧隐藏“%1” · 文件仍可搜索 · 可在设置中恢复").arg(displayName));
}

void SmartSpaceWidget::excludeFolderFromIndex(const QString &path)
{
    const QString clean = normalizedPath(path);
    if (clean.isEmpty() || pathExcluded(clean))
        return;
    if (qEnvironmentVariableIntValue("UKUI_FENCES_TEST_CONFIRM_EXCLUDE") != 1 &&
        smartQuestion(
            this, QStringLiteral("取消索引文件夹"),
            QStringLiteral(
                "将从当前结果移除“%1”及其全部子项，后续增量、快速全量和 OCR 都会跳过。\n\n"
                "文件本身不会删除，可在设置 → 索引范围中恢复。是否继续？")
                .arg(clean),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    m_excludedFolders << clean;
    m_hiddenFolders.removeAll(clean);
    if (m_libraryJobBusy)
        stopIdleFullIndex();
    m_entries.erase(
        std::remove_if(m_entries.begin(), m_entries.end(),
                       [this](const SmartSpaceEntry &entry) { return pathExcluded(entry.path); }),
        m_entries.end());
    if (!m_scopePath.isEmpty() && pathIsInside(m_scopePath, clean))
        m_scopePath.clear();
    saveSettings();
    rebuildWatches();
    updateResults(true);
    m_statusLabel->setText(
        QStringLiteral("已排除索引“%1” · 点击底部“增量”后更新文件清单 · 设置中可恢复")
            .arg(QFileInfo(clean).fileName()));
}

QVector<int> SmartSpaceWidget::matchingFileIndexes() const
{
    QVector<int> result;
    for (int i = 0; i < m_entries.size(); ++i) {
        const SmartSpaceEntry &entry = m_entries.at(i);
        if (fileMatchesCurrentResults(entry))
            result.append(i);
    }
    // An empty search has no meaningful relevance order.  Keeping snapshot
    // order avoids scoring and sorting tens of thousands of files at startup.
    if (m_resultSortMode == 0 && m_executedQuery.trimmed().isEmpty())
        return result;

    const QString foldedQuery = m_executedQuery.trimmed().toCaseFolded();
    QStringList foldedTerms;
    foldedTerms.reserve(m_searchTerms.size());
    for (const QString &term : m_searchTerms)
        foldedTerms.append(term.toCaseFolded());
    auto nameMatchRank = [this, &foldedQuery, &foldedTerms](const SmartSpaceEntry &entry) {
        if (m_searchUsesRegex)
            return m_searchExpression.match(entry.name).hasMatch() ? 3 : 0;
        const QString foldedName = entry.name.toCaseFolded();
        if (!foldedQuery.isEmpty() && foldedName.contains(foldedQuery))
            return 3;
        int hits = 0;
        for (const QString &term : foldedTerms)
            hits += foldedName.contains(term) ? 1 : 0;
        if (!foldedTerms.isEmpty() && hits == foldedTerms.size())
            return 2;
        return hits > 0 ? 1 : 0;
    };

    struct RelevanceMetric {
        int index = -1;
        int percent = -1;
        int nameRank = 0;
        int score = 0;
        int knowledgeScore = 0;
    };
    if (m_resultSortMode == 0) {
        QVector<RelevanceMetric> ranked;
        ranked.reserve(result.size());
        for (const int index : result) {
            const SmartSpaceEntry &entry = m_entries.at(index);
            RelevanceMetric metric;
            metric.index = index;
            metric.percent = relevancePercent(entry);
            metric.nameRank = nameMatchRank(entry);
            metric.score = localEntryScore(entry, m_executedQuery);
            metric.knowledgeScore = m_knowledgeScores.value(entry.path);
            ranked.append(metric);
        }
        std::stable_sort(ranked.begin(), ranked.end(),
                         [this](const RelevanceMetric &left, const RelevanceMetric &right) {
                             // “相关度”与文件卡片显示的“匹配 xx%”必须是同一个
                             // 概念。百分比较低的结果不得因隐藏的子分数排到前面；
                             // 文件名和知识库分数只用于百分比相同的稳定排序。
                             if (left.percent != right.percent)
                                 return left.percent > right.percent;
                             if (left.nameRank != right.nameRank)
                                 return left.nameRank > right.nameRank;
                             if (left.knowledgeScore != right.knowledgeScore)
                                 return left.knowledgeScore > right.knowledgeScore;
                             if (left.score != right.score)
                                 return left.score > right.score;
                             return QString::localeAwareCompare(m_entries.at(left.index).name,
                                                                m_entries.at(right.index).name) < 0;
                         });
        result.clear();
        result.reserve(ranked.size());
        for (const RelevanceMetric &metric : ranked)
            result.append(metric.index);
        return result;
    }

    std::stable_sort(result.begin(), result.end(), [this, &nameMatchRank](int a, int b) {
        const SmartSpaceEntry &left = m_entries.at(a);
        const SmartSpaceEntry &right = m_entries.at(b);
        if (m_resultSortMode == 1 && left.modified != right.modified)
            return left.modified > right.modified;
        if (m_resultSortMode == 2 && left.size != right.size)
            return left.size > right.size;
        return QString::localeAwareCompare(left.name, right.name) < 0;
    });
    return result;
}

int SmartSpaceWidget::relevancePercent(const SmartSpaceEntry &entry) const
{
    const int knowledgePercent = m_knowledgeScores.value(entry.path, -1);
    const QString query = m_executedQuery.trimmed();
    if (query.isEmpty())
        return -1;
    if (m_searchUsesRegex)
        return m_searchExpression.match(entry.name).hasMatch() ? 96 : 72;
    const QString folded = query.toCaseFolded();
    if (entry.name.toCaseFolded().contains(folded))
        return 98;
    const int score = localEntryScore(entry, query);
    if (score <= 0)
        return knowledgePercent;
    int percent = 30 + (score * 69 / (score + 350));
    if (entry.content.toCaseFolded().contains(folded))
        percent = qMax(percent, 90);
    return qMax(knowledgePercent, qBound(35, percent, 97));
}

QWidget *SmartSpaceWidget::createFileCard(const SmartSpaceEntry &entry, QListWidgetItem *item)
{
    auto *card = new SmartFileCard(
        [this, path = entry.path, item] {
            if (item)
                m_fileList->setCurrentItem(item);
            openPath(path);
        },
        m_fileList);
    card->setObjectName(QStringLiteral("fileResultCard"));
    card->setAttribute(Qt::WA_Hover, true);
    const QString toolTip = entryToolTip(entry);
    auto *row = new QHBoxLayout(card);
    static const int iconExtents[] = {14, 16, 18, 20, 25, 30, 34, 38};
    static const int cardHeights[] = {24, 28, 33, 38, 50, 64, 78, 94};
    static const int previewExtents[] = {20, 22, 24, 26, 28, 32, 32, 32};
    const int densityIndex =
        qBound(0, m_resultDensity - kMinResultDensity, kResultDensityLevels - 1);
    const bool compact = m_resultDensity < 0;
    const bool ultraCompact = m_resultDensity <= -2;
    const int iconExtent = iconExtents[densityIndex];
    const int cardHeight = cardHeights[densityIndex];
    row->setContentsMargins(m_resultDensity <= -3 ? 4
                            : ultraCompact        ? 6
                                                  : 9,
                            m_resultDensity <= -3 ? 0
                            : compact             ? 2
                                                  : 5,
                            m_resultDensity <= -3 ? 4 : 8,
                            m_resultDensity <= -3 ? 0
                            : compact             ? 2
                                                  : 5);
    row->setSpacing(m_resultDensity <= -3 ? 4 : compact ? 6 : 8);

    auto *icon = new QLabel(card);
    icon->setFixedSize(iconExtent + 4, iconExtent + 4);
    icon->setAlignment(Qt::AlignCenter);
    icon->setPixmap(entryIcon(entry).pixmap(iconExtent, iconExtent));
    row->addWidget(icon, 0, Qt::AlignVCenter);

    auto *textContainer = new QWidget(card);
    textContainer->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto *texts = new QVBoxLayout(textContainer);
    texts->setContentsMargins(0, 0, 0, 0);
    texts->setSpacing(2);
    auto *name = new QLabel(entry.name, card);
    name->setProperty("cardName", true);
    name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    name->setTextInteractionFlags(Qt::NoTextInteraction);
    name->setMouseTracking(true);
    name->installEventFilter(this);
    name->setToolTip(toolTip);
    texts->addWidget(name);

    QStringList metadata;
    const QString size = formattedFileSize(entry.size);
    const QString modified = formattedModified(entry.modified);
    if (!size.isEmpty())
        metadata << size;
    if (!modified.isEmpty())
        metadata << modified;
    if (entry.ocrStatus == QLatin1String("pending"))
        metadata << QStringLiteral("待检测/OCR");
    if (m_knowledgeAvailability.value(entry.path) == QLatin1String("unseen"))
        metadata << QStringLiteral("本轮未发现来源 · 保留库内正文");
    const QString knowledgeState = m_knowledgeExtractionStates.value(entry.path);
    if (knowledgeState == QLatin1String("partial"))
        metadata << QStringLiteral("正文部分提取");
    else if (knowledgeState == QLatin1String("failed") ||
             knowledgeState == QLatin1String("missing"))
        metadata << QStringLiteral("提取未完成 · 使用已存快照");
    auto *meta = new QLabel(metadata.join(QStringLiteral(" · ")), card);
    meta->setProperty("cardMeta", true);
    meta->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    meta->setVisible(m_resultDensity >= 0);
    auto *metadataRow = new QHBoxLayout;
    metadataRow->setContentsMargins(0, 0, 0, 0);
    metadataRow->setSpacing(6);
    metadataRow->addWidget(meta);
    const QString parentName =
        entry.libraryOnly ? entry.libraryName : QFileInfo(entry.path).dir().dirName();
    if (!parentName.isEmpty() && !ultraCompact) {
        auto *folderBadge = new QLabel(card);
        folderBadge->setProperty("folderBadge", true);
        QFont badgeFont = font();
        badgeFont.setPixelSize(10);
        folderBadge->setFont(badgeFont);
        folderBadge->setText(QFontMetrics(badgeFont).elidedText(parentName, Qt::ElideRight, 164));
        folderBadge->setMaximumWidth(180);
        folderBadge->setToolTip(entry.libraryOnly
                                    ? QStringLiteral("知识库副本 · %1").arg(entry.originalPath)
                                    : QFileInfo(entry.path).absolutePath());
        metadataRow->insertWidget(0, folderBadge);
    }

    metadataRow->addStretch(1);
    texts->addLayout(metadataRow);

    auto *snippet = new QLabel(card);
    snippet->setProperty("cardSnippet", true);
    snippet->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    snippet->setTextFormat(Qt::RichText);
    snippet->setText(relevantSnippetHtml(entry, 1, 24, 70));
    snippet->setTextInteractionFlags(Qt::NoTextInteraction);
    snippet->setVisible(!ultraCompact);
    texts->addWidget(snippet);
    row->addWidget(textContainer, 1);

    const int matchPercent = relevancePercent(entry);
    if (matchPercent >= 0) {
        auto *match = new QLabel(ultraCompact ? QStringLiteral("%1%").arg(matchPercent)
                                              : QStringLiteral("匹配 %1%").arg(matchPercent),
                                 card);
        match->setProperty("matchScore", true);
        match->setProperty("matchLevel",
                           matchPercent >= 85 ? QStringLiteral("high") : QStringLiteral("medium"));
        match->setToolTip(
            QStringLiteral("本地相关度 %1%：由文件名、路径和正文命中综合换算").arg(matchPercent));
        row->addWidget(match, 0, Qt::AlignVCenter);
    }

    auto *preview = new QToolButton(card);
    preview->setText(QStringLiteral("预览"));
    preview->setAccessibleName(QStringLiteral("预览 %1").arg(entry.name));
    preview->setToolTip(QStringLiteral("预览相关内容"));
    preview->setProperty("previewAction", true);
    preview->setToolButtonStyle(Qt::ToolButtonTextOnly);
    preview->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    const int previewExtent = previewExtents[densityIndex];
    preview->setFixedSize(52, qMax(22, previewExtent));
    connect(preview, &QToolButton::clicked, this,
            [this, path = entry.path] { showPreviewForPath(path); });
    auto *actionSlot = new QWidget(card);
    actionSlot->setFixedSize(preview->size());
    auto *actionLayout = new QHBoxLayout(actionSlot);
    actionLayout->setContentsMargins(0, 0, 0, 0);
    actionLayout->addWidget(preview);
    row->addWidget(actionSlot, 0, Qt::AlignVCenter);
    card->setPreviewButton(preview);

    // Text metrics vary with the system font and DPI. Keep density as a lower
    // bound so the final snippet line is not clipped by a fixed row height.
    card->ensurePolished();
    if (item)
        item->setSizeHint(QSize(0, qMax(cardHeight, card->minimumSizeHint().height())));
    return card;
}

const SmartSpaceEntry *SmartSpaceWidget::entryForPath(const QString &path) const
{
    for (const SmartSpaceEntry &entry : m_entries) {
        if (entry.path == path)
            return &entry;
    }
    return nullptr;
}

void SmartSpaceWidget::updateResults(bool resetFolders)
{
    if (resetFolders) {
        m_scopePath.clear();
        clearFolderLevels();
        appendRootLevel();
    }

    QStringList categories = m_selectedCategories;
    QStringList visibleExtensions = m_visibleExtensions;
    QStringList excludedFolders = m_excludedFolders;
    categories.sort(Qt::CaseInsensitive);
    visibleExtensions.sort(Qt::CaseInsensitive);
    excludedFolders.sort(Qt::CaseInsensitive);
    const QString pageKey =
        QStringLiteral("%1\n%2\n%3\n%4\n%5\n%6\n%7\n%8")
            .arg(m_executedQuery, categories.join(QLatin1Char(',')),
                 visibleExtensions.join(QLatin1Char(',')), excludedFolders.join(QLatin1Char('\n')),
                 m_scopePath, QString::number(m_resultSortMode), QString(),
                 QString::number(m_entries.size()));
    if (pageKey != m_resultPageKey) {
        m_resultPageKey = pageKey;
        m_visibleResultLimit = kInitialResultsPageSize;
    }

    m_fileList->setSpacing(m_resultDensity <= -3   ? 0
                           : m_resultDensity <= -2 ? 1
                           : m_resultDensity < 0   ? 2
                                                   : 4);
    m_fileList->clear();
    const QVector<int> matches = matchingFileIndexes();
    const int visible = std::min(matches.size(), m_visibleResultLimit);
    for (int i = 0; i < visible; ++i) {
        const SmartSpaceEntry &entry = m_entries.at(matches.at(i));
        auto *item = new QListWidgetItem(m_fileList);
        item->setData(PathRole, entry.path);
        m_fileList->setItemWidget(item, createFileCard(entry, item));
    }
    if (visible < matches.size()) {
        const int remaining = matches.size() - visible;
        const int nextBatch = std::min(kResultsPageSize, remaining);
        auto *moreItem = new QListWidgetItem(m_fileList);
        moreItem->setFlags(Qt::NoItemFlags);
        moreItem->setSizeHint(QSize(0, 44));
        auto *loadMore = new QToolButton(m_fileList);
        loadMore->setProperty("loadMoreResults", true);
        loadMore->setToolButtonStyle(Qt::ToolButtonTextOnly);
        loadMore->setCursor(Qt::PointingHandCursor);
        loadMore->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        loadMore->setText(QStringLiteral("加载更多 %1 个 · 剩余 %2").arg(nextBatch).arg(remaining));
        loadMore->setToolTip(
            QStringLiteral("首次显示 32 项，之后每次增加 100 项，避免大量结果一次创建控件"));
        m_fileList->setItemWidget(moreItem, loadMore);
        connect(loadMore, &QToolButton::clicked, this, [this, visible] {
            m_visibleResultLimit += kResultsPageSize;
            updateResults(false);
            if (visible < m_fileList->count()) {
                if (QListWidgetItem *firstNew = m_fileList->item(visible))
                    m_fileList->scrollToItem(firstNew, QAbstractItemView::PositionAtCenter);
            }
        });
    }
    if (visible == 0) {
        auto *empty = new QListWidgetItem(
            QIcon::fromTheme(QStringLiteral("edit-find")),
            m_executedQuery.isEmpty() ? QStringLiteral("输入关键词检索知识库，或从向导建立个人库")
                                      : QStringLiteral("没有匹配的文件或知识 · 请查看检索状态"),
            m_fileList);
        empty->setFlags(Qt::NoItemFlags);
    }

    QString scope =
        m_scopePath.isEmpty() ? QStringLiteral("全部目录") : QFileInfo(m_scopePath).fileName();
    if (scope.isEmpty())
        scope = m_scopePath;
    if (m_folderTitleLabel)
        m_folderTitleLabel->setText(QStringLiteral("文件夹拾取"));
    if (m_fileTitleLabel)
        m_fileTitleLabel->setText(QStringLiteral("匹配结果"));
    if (m_fileCountLabel)
        m_fileCountLabel->setText(
            QStringLiteral("%1 项").arg(QLocale::system().toString(matches.size())));
    if (m_folderCountLabel) {
        int folderCount = 0;
        if (!m_folderLevelWidgets.isEmpty()) {
            const QList<QListWidget *> lists =
                m_folderLevelWidgets.constLast()->findChildren<QListWidget *>();
            if (!lists.isEmpty()) {
                for (int row = 0; row < lists.first()->count(); ++row) {
                    if (lists.first()->item(row)->flags() != Qt::NoItemFlags)
                        ++folderCount;
                }
            }
        }
        m_folderCountLabel->setText(
            QStringLiteral("%1 项").arg(QLocale::system().toString(folderCount)));
    }
    m_statusLabel->setText(QStringLiteral("%1%2 · 匹配 %3 个文件%4")
                               .arg(scope)
                               .arg(QString())
                               .arg(matches.size())
                               .arg(matches.size() > visible
                                        ? QStringLiteral(" · 显示前 %1 个").arg(visible)
                                        : QString()));
    update();
    if (parentWidget())
        parentWidget()->update(geometry().adjusted(-4, -4, 4, 4));
}

void SmartSpaceWidget::clearFolderLevels(int fromLevel)
{
    while (m_folderLevelWidgets.size() > fromLevel) {
        QWidget *widget = m_folderLevelWidgets.takeLast();
        m_folderLevelsLayout->removeWidget(widget);
        delete widget;
    }
}

void SmartSpaceWidget::resetFolderScopeFromBlankClick()
{
    if (m_scopePath.isEmpty())
        return;
    // The clicked list is part of the hierarchy that updateResults destroys.
    // Defer rebuilding until the mouse event has returned to Qt.
    QTimer::singleShot(0, this, [this] {
        if (m_scopePath.isEmpty())
            return;
        updateResults(true);
    });
}

void SmartSpaceWidget::updateFolderDensity()
{
    static const int rowHeights[] = {22, 25, 28, 30, 36, 44, 52, 62};
    static const int iconSizes[] = {13, 15, 16, 18, 21, 24, 28, 32};
    const int densityIndex =
        qBound(0, m_resultDensity - kMinResultDensity, kResultDensityLevels - 1);
    const int rowHeight = rowHeights[densityIndex];
    for (int level = 0; level < m_folderLevelWidgets.size(); ++level) {
        const QList<QListWidget *> lists =
            m_folderLevelWidgets.at(level)->findChildren<QListWidget *>();
        for (QListWidget *list : lists) {
            list->setIconSize(QSize(iconSizes[densityIndex], iconSizes[densityIndex]));
            list->setSpacing(m_resultDensity <= -3   ? 0
                             : m_resultDensity <= -1 ? 1
                             : m_resultDensity >= 1  ? 4
                                                     : 2);
            for (int row = 0; row < list->count(); ++row)
                list->item(row)->setSizeHint(QSize(0, rowHeight));
            if (level > 0) {
                const int visibleRows = qBound(2, list->count(), 5);
                list->setMaximumHeight(qMin(320, visibleRows * rowHeight + 12));
            }
        }
    }
}

void SmartSpaceWidget::appendRootLevel()
{
    clearFolderLevels();
    auto *frame = new QFrame(m_folderLevelsContainer);
    auto *layout = new QVBoxLayout(frame);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    QListWidget *list = createEntryList();
    list->setObjectName(QStringLiteral("smartRootFolderList"));
    list->setItemDelegate(new FolderBadgeDelegate(list));
    static_cast<SmartEntryList *>(list)->setBlankClickAction(
        [this] { resetFolderScopeFromBlankClick(); });
    list->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    layout->addWidget(list);
    struct FolderResult {
        QString path;
        QString name;
        int count = 0;
    };
    QVector<FolderResult> folders;
    QHash<QString, int> folderIndexes;
    // Files in the same directory share a navigation group. Resolve its
    // root/relative path once per parent, not once per document (100k+ items).
    QHash<QString, QHash<QString, QString>> groupByParent;
    QStringList configuredRoots;
    for (const auto &root : m_roots)
        configuredRoots << normalizedPath(root);
    for (const auto &entry : m_entries) {
        if (!fileMatchesCurrentResults(entry, false))
            continue;
        const QString parent = entry.path.left(entry.path.lastIndexOf(QLatin1Char('/')));
        auto &parents = groupByParent[entry.root];
        auto cached = parents.constFind(parent);
        QString key;
        if (cached != parents.constEnd())
            key = cached.value();
        else {
            QStringList roots;
            if (!entry.root.isEmpty())
                roots << normalizedPath(entry.root);
            for (const auto &root : configuredRoots)
                if (!roots.contains(root))
                    roots << root;
            for (const auto &root : roots) {
                const QString prefix = root == QStringLiteral("/") ? root : root + QLatin1Char('/');
                if (!entry.path.startsWith(prefix))
                    continue;
                const int separator = entry.path.indexOf(QLatin1Char('/'), prefix.size());
                if (separator < 0)
                    break;
                const QString candidate = entry.path.left(separator);
                if (!folderHidden(candidate))
                    key = candidate;
                break;
            }
            parents.insert(parent, key);
        }
        if (key.isEmpty())
            continue;
        const auto found = folderIndexes.constFind(key);
        if (found == folderIndexes.constEnd()) {
            folderIndexes.insert(key, folders.size());
            folders.append({key, QFileInfo(key).fileName(), 1});
        } else
            ++folders[found.value()].count;
    }
    setProperty("navigationGroups", folders.size());
    std::sort(folders.begin(), folders.end(), [](const FolderResult &a, const FolderResult &b) {
        return QString::localeAwareCompare(a.name, b.name) < 0;
    });
    for (const FolderResult &folder : folders) {
        auto *item =
            new QListWidgetItem(QIcon::fromTheme(QStringLiteral("folder")), folder.name, list);
        item->setData(kFolderCountRole, folder.count);
        item->setData(PathRole, normalizedPath(folder.path));
        item->setToolTip(folder.path);
    }
    if (folders.isEmpty()) {
        auto *empty = new QListWidgetItem(
            QIcon::fromTheme(QStringLiteral("folder")),
            m_entries.isEmpty() ? QStringLiteral("本地目录为空 · 已导入的知识库可直接搜索")
                                : QStringLiteral("当前条件下没有子文件夹"),
            list);
        empty->setFlags(Qt::NoItemFlags);
    }
    connect(list, &QListWidget::itemClicked, this,
            [this](QListWidgetItem *item) { folderActivated(item, 0); });
    connect(list, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *item) { openPath(item->data(PathRole).toString()); });
    connect(list, &QListWidget::customContextMenuRequested, this,
            [this, list](const QPoint &pos) { showEntryMenu(list, pos); });
    frame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_folderLevelsLayout->insertWidget(m_folderLevelsLayout->count() - 1, frame, 1);
    m_folderLevelWidgets.append(frame);
    updateFolderDensity();
}

void SmartSpaceWidget::appendFolderLevel(const QString &parentPath, int level)
{
    clearFolderLevels(level);
    struct FolderResult {
        QString path;
        QString name;
        int count = 0;
    };
    QVector<FolderResult> folders;
    QHash<QString, int> folderIndexes;
    const QString cleanParent = normalizedPath(parentPath);
    for (const SmartSpaceEntry &entry : m_entries) {
        if (!fileMatchesCurrentResults(entry, false) || !pathIsInside(entry.path, cleanParent))
            continue;
        const QString relative = QDir(cleanParent).relativeFilePath(entry.path);
        const int separator = relative.indexOf(QLatin1Char('/'));
        if (separator <= 0)
            continue;
        const QString key = normalizedPath(QDir(cleanParent).filePath(relative.left(separator)));
        if (folderHidden(key))
            continue;
        const auto found = folderIndexes.constFind(key);
        if (found == folderIndexes.constEnd()) {
            folderIndexes.insert(key, folders.size());
            folders.append({key, QFileInfo(key).fileName(), 1});
        } else {
            ++folders[found.value()].count;
        }
    }
    std::sort(folders.begin(), folders.end(), [](const FolderResult &a, const FolderResult &b) {
        return QString::localeAwareCompare(a.name, b.name) < 0;
    });
    if (folders.isEmpty())
        return;

    auto *frame = new QFrame(m_folderLevelsContainer);
    auto *layout = new QVBoxLayout(frame);
    layout->setContentsMargins(8, 7, 0, 0);
    layout->setSpacing(4);
    QString title = QFileInfo(parentPath).fileName();
    if (title.isEmpty())
        title = parentPath;
    auto *label = new QLabel(QStringLiteral("↳ %1").arg(title), frame);
    label->setProperty("panelTitle", true);
    layout->addWidget(label);
    QListWidget *list = createEntryList();
    list->setObjectName(QStringLiteral("smartChildFolderList"));
    list->setItemDelegate(new FolderBadgeDelegate(list));
    static_cast<SmartEntryList *>(list)->setBlankClickAction(
        [this] { resetFolderScopeFromBlankClick(); });
    list->setMaximumHeight(190);
    layout->addWidget(list);
    for (const FolderResult &folder : folders) {
        auto *item =
            new QListWidgetItem(QIcon::fromTheme(QStringLiteral("folder")), folder.name, list);
        item->setData(kFolderCountRole, folder.count);
        item->setData(PathRole, folder.path);
        item->setToolTip(folder.path);
    }
    connect(list, &QListWidget::itemClicked, this,
            [this, level](QListWidgetItem *item) { folderActivated(item, level); });
    connect(list, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *item) { openPath(item->data(PathRole).toString()); });
    connect(list, &QListWidget::customContextMenuRequested, this,
            [this, list](const QPoint &pos) { showEntryMenu(list, pos); });
    m_folderLevelsLayout->insertWidget(m_folderLevelsLayout->count() - 1, frame);
    m_folderLevelWidgets.append(frame);
    updateFolderDensity();
}

void SmartSpaceWidget::folderActivated(QListWidgetItem *item, int level)
{
    if (!item)
        return;
    m_scopePath = normalizedPath(item->data(PathRole).toString());
    appendFolderLevel(m_scopePath, level + 1);
    updateResults(false);
    m_folderScroll->ensureVisible(0, m_folderLevelsContainer->height(), 0, 20);
}

int SmartSpaceWidget::indexedEntryCount() const
{
    return m_entries.size() - m_libraryOnlyEntryCount;
}
int SmartSpaceWidget::resultCount() const
{
    int count = 0;
    if (m_fileList)
        for (int row = 0; row < m_fileList->count(); ++row)
            if (!m_fileList->item(row)->data(PathRole).toString().isEmpty())
                ++count;
    return count;
}
void SmartSpaceWidget::searchFor(const QString &query)
{
    m_searchEdit->setText(query);
    executeSearch();
}

void SmartSpaceWidget::openFileItem(QListWidgetItem *item)
{
    if (item)
        openPath(item->data(PathRole).toString());
}

void SmartSpaceWidget::openPath(const QString &path)
{
    if (path.startsWith(QLatin1String("aispace://"))) {
        showPreviewForPath(path);
        return;
    }
    const DesktopItem entry = DesktopItem::fromPath(path);
    if (!entry.isValid())
        return;
    entry.openAsync(this, [this](bool ok, const QString &error) {
        if (!ok)
            smartWarning(this, QStringLiteral("无法打开项目"), error);
    });
}

void SmartSpaceWidget::showEntryMenu(QListWidget *list, const QPoint &position)
{
    QListWidgetItem *item = list ? list->itemAt(position) : nullptr;
    if (!item)
        return;
    list->setCurrentItem(item);
    const QString path = item->data(PathRole).toString();
    if (path.isEmpty())
        return;

    QMenu menu(this);
    applyMenuTheme(&menu);
    if (path.startsWith(QLatin1String("aispace://"))) {
        auto *read = menu.addAction(QStringLiteral("阅读库内正文"));
        auto *copy = menu.addAction(QStringLiteral("复制知识引用"));
        connect(read, &QAction::triggered, this, [this, path] { showPreviewForPath(path); });
        connect(copy, &QAction::triggered, this,
                [path] { QApplication::clipboard()->setText(path); });
        menu.exec(list->viewport()->mapToGlobal(position));
        return;
    }
    QAction *open = menu.addAction(QIcon::fromTheme(QStringLiteral("document-open")),
                                   QFileInfo(path).isDir() ? QStringLiteral("用文件管理器打开")
                                                           : QStringLiteral("打开"));
    connect(open, &QAction::triggered, this, [this, path] { openPath(path); });
    if (QFileInfo(path).isDir()) {
        menu.addSeparator();
        QAction *hideFolder = menu.addAction(QIcon::fromTheme(QStringLiteral("view-hidden")),
                                             QStringLiteral("取消显示（仅隐藏左栏）"));
        hideFolder->setToolTip(QStringLiteral("不影响索引和右侧搜索结果"));
        QAction *excludeFolder = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-delete")),
                                                QStringLiteral("取消索引此文件夹…"));
        excludeFolder->setToolTip(QStringLiteral("从索引中排除此文件夹及全部子项，不删除本地文件"));
        connect(hideFolder, &QAction::triggered, this,
                [this, path] { hideFolderFromNavigation(path); });
        connect(excludeFolder, &QAction::triggered, this,
                [this, path] { excludeFolderFromIndex(path); });
    } else {
        QAction *containing = menu.addAction(QIcon::fromTheme(QStringLiteral("folder-open")),
                                             QStringLiteral("打开所在文件夹"));
        connect(containing, &QAction::triggered, this,
                [this, path] { openPath(QFileInfo(path).absolutePath()); });
    }
    menu.exec(list->viewport()->mapToGlobal(position));
}

void SmartSpaceWidget::setEditMode(bool edit)
{
    m_editMode = edit;
    if (m_sizeGrip)
        m_sizeGrip->setVisible(true);
    update();
}

void SmartSpaceWidget::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    painter.setClipRegion(event->region());
    painter.setRenderHint(QPainter::Antialiasing);
    if (isWindow()) {
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(rect(), Qt::transparent);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    }
    const bool maxed = isWindowMaximized();
    const qreal radius = maxed ? 0.0 : AiSpace::WindowMetrics::CornerRadius;
    QPainterPath path;
    path.addRoundedRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5), radius, radius);
    const QColor shell =
        isActiveWindow() ? m_surfaceColor : QColor(m_themeMode == 2 ? "#f8fafc" : "#0f172a");
    painter.fillPath(path, shell);
    if (!maxed) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(m_borderColor, 1));
        painter.drawPath(path);
    }
}

void SmartSpaceWidget::moveEvent(QMoveEvent *event)
{
    QWidget::moveEvent(event);
    emit geometryChanged();
}

void SmartSpaceWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateRoundedMask();
    updateResponsiveLayout();
    if (!m_isMaximized) {
        if (!m_resizing)
            move(boundedPosition(pos()));
    }
    m_geometrySaveTimer.start();
    emit geometryChanged();
}
void SmartSpaceWidget::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);
    applyMenuTheme(&menu);
    QAction *pin =
        menu.addAction(m_alwaysOnTop ? QStringLiteral("取消置顶") : QStringLiteral("置顶"));
    menu.addSeparator();
    QAction *refresh = menu.addAction(smartMenuIcon(QStringLiteral("refresh")),
                                      QStringLiteral("增量更新 · 扫描并复用正文"));
    QAction *settings =
        menu.addAction(smartMenuIcon(QStringLiteral("settings")), QStringLiteral("AI空间设置"));
    QAction *idleIndex =
        menu.addAction(smartMenuIcon(QStringLiteral("fast-full")), QStringLiteral("① 全量扫描"));
    QAction *textIndex =
        menu.addAction(smartMenuIcon(QStringLiteral("refresh")), QStringLiteral("② 文本建库"));
    textIndex->setEnabled(!m_libraryJobBusy);
    QAction *ocrIndex =
        menu.addAction(smartMenuIcon(QStringLiteral("ocr")), QStringLiteral("③ OCR补全"));
    idleIndex->setEnabled(!m_libraryJobBusy);
    ocrIndex->setEnabled(!m_libraryJobBusy);

    QAction *selected = menu.exec(event->globalPos());
    if (selected == pin)
        setAlwaysOnTop(!m_alwaysOnTop);
    else if (selected == refresh)
        startIndexing();
    else if (selected == settings)
        showSettingsDialog();
    else if (selected == idleIndex)
        startGuidedBuild(false, false, QStringLiteral("manifest"));
    else if (selected == textIndex)
        startGuidedBuild(false, false, QStringLiteral("text"));
    else if (selected == ocrIndex)
        startOcrIndex();
}

bool SmartSpaceWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_statusLabel && event->type() == QEvent::MouseButtonRelease &&
        static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
        showKnowledgeStatus();
        return true;
    }
    if (watched == m_statusLabel && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Space) {
            showKnowledgeStatus();
            return true;
        }
    }
    auto *title = qobject_cast<QLabel *>(watched);
    if (title && title->property("cardName").toBool()) {
        if (event->type() == QEvent::Leave) {
            QToolTip::hideText();
        } else if (event->type() == QEvent::ToolTip || event->type() == QEvent::MouseMove) {
            const QPoint pos = event->type() == QEvent::ToolTip
                                   ? static_cast<QHelpEvent *>(event)->pos()
                                   : static_cast<QMouseEvent *>(event)->pos();
            const int textEnd = title->contentsRect().left() +
                                title->fontMetrics().horizontalAdvance(title->text());
            if (pos.x() >= textEnd) {
                QToolTip::hideText();
                if (event->type() == QEvent::ToolTip)
                    return true;
            }
        }
    }
    if ((watched == m_folderLevelsContainer ||
         (m_folderScroll && watched == m_folderScroll->viewport())) &&
        event->type() == QEvent::MouseButtonPress) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() == Qt::LeftButton && !m_scopePath.isEmpty()) {
            resetFolderScopeFromBlankClick();
            return true;
        }
    }
    if (watched == m_indexProgress) {
        if ((event->type() == QEvent::Enter || event->type() == QEvent::ToolTip) &&
            m_indexProgress && m_indexProgress->isVisible()) {
            QToolTip::showText(m_indexProgress->mapToGlobal(QPoint(m_indexProgress->width() / 2,
                                                                   m_indexProgress->height() + 4)),
                               m_indexProgress->toolTip(), m_indexProgress);
            return event->type() == QEvent::ToolTip;
        }
        if (event->type() == QEvent::Leave)
            QToolTip::hideText();
    }
    return QWidget::eventFilter(watched, event);
}

namespace
{
constexpr int kResizeLeft = 1;
constexpr int kResizeRight = 2;
constexpr int kResizeTop = 4;
constexpr int kResizeBottom = 8;
constexpr int kResizeBand = 6;
} // namespace

int SmartSpaceWidget::hitResizeEdges(const QPoint &pos) const
{
    if (isWindowMaximized() || !rect().contains(pos))
        return 0;
    int edges = 0;
    if (pos.x() <= kResizeBand)
        edges |= kResizeLeft;
    else if (pos.x() >= width() - kResizeBand - 1)
        edges |= kResizeRight;
    if (pos.y() <= kResizeBand - 2)
        edges |= kResizeTop;
    else if (pos.y() >= height() - kResizeBand - 1)
        edges |= kResizeBottom;
    return edges;
}

void SmartSpaceWidget::updateResizeCursor(const QPoint &pos)
{
    const int edges = hitResizeEdges(pos);
    Qt::CursorShape shape = Qt::ArrowCursor;
    if ((edges & kResizeLeft && edges & kResizeTop) ||
        (edges & kResizeRight && edges & kResizeBottom))
        shape = Qt::SizeFDiagCursor;
    else if ((edges & kResizeRight && edges & kResizeTop) ||
             (edges & kResizeLeft && edges & kResizeBottom))
        shape = Qt::SizeBDiagCursor;
    else if (edges & (kResizeLeft | kResizeRight))
        shape = Qt::SizeHorCursor;
    else if (edges & (kResizeTop | kResizeBottom))
        shape = Qt::SizeVerCursor;
    if (!edges) {
        if (testAttribute(Qt::WA_SetCursor))
            unsetCursor();
    } else if (!testAttribute(Qt::WA_SetCursor) || cursor().shape() != shape) {
        setCursor(shape);
    }
}

static bool insideTitleBar(QWidget *self, QWidget *bar, const QPoint &pos)
{
    if (!bar || !bar->isVisible())
        return false;
    const QRect area(bar->mapTo(self, QPoint(0, 0)), bar->size());
    return area.contains(pos);
}

void SmartSpaceWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    const int edges = hitResizeEdges(event->pos());
    if (edges) {
        m_resizing = true;
        m_resizeEdges = edges;
        m_resizeStartGlobal = event->globalPos();
        m_resizeStartGeom = geometry();
        event->accept();
        return;
    }
    if (insideTitleBar(this, m_actionRail, event->pos())) {
        m_dragging = true;
        m_dragStartGlobal = event->globalPos();
        m_dragOffset = event->globalPos() - frameGeometry().topLeft();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void SmartSpaceWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (m_resizing) {
        const QPoint delta = event->globalPos() - m_resizeStartGlobal;
        QRect g = m_resizeStartGeom;
        const QSize minimum = minimumSize();
        if (m_resizeEdges & kResizeLeft)
            g.setLeft(qMin(g.left() + delta.x(), g.right() - minimum.width() + 1));
        if (m_resizeEdges & kResizeRight)
            g.setRight(qMax(g.right() + delta.x(), g.left() + minimum.width() - 1));
        if (m_resizeEdges & kResizeTop)
            g.setTop(qMin(g.top() + delta.y(), g.bottom() - minimum.height() + 1));
        if (m_resizeEdges & kResizeBottom)
            g.setBottom(qMax(g.bottom() + delta.y(), g.top() + minimum.height() - 1));
        setGeometry(g);
        event->accept();
        return;
    }
    if (m_dragging) {
        if (isWindowMaximized()) {
            if ((event->globalPos() - m_dragStartGlobal).manhattanLength() < 8)
                return;
            // Restore under the pointer, keeping the same relative grab point.
            const qreal ratio = width() > 0 ? qreal(m_dragOffset.x()) / width() : 0.5;
            setWindowMaximized(false);
            m_dragOffset = QPoint(qRound(width() * ratio), qMin(m_dragOffset.y(), 24));
        }
        move(event->globalPos() - m_dragOffset);
        event->accept();
        return;
    }
    if (!(event->buttons() & Qt::LeftButton))
        updateResizeCursor(event->pos());
    QWidget::mouseMoveEvent(event);
}

void SmartSpaceWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_resizing || m_dragging) {
        const bool dragged = m_dragging;
        m_resizing = false;
        m_dragging = false;
        m_resizeEdges = 0;
        updateResizeCursor(event->pos());
        if (dragged && !isWindowMaximized())
            move(boundedPosition(pos()));
        saveSettings();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void SmartSpaceWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && insideTitleBar(this, m_actionRail, event->pos()) &&
        !hitResizeEdges(event->pos())) {
        m_dragging = false;
        toggleMaximized();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void SmartSpaceWidget::setWindowMaximized(bool maximized)
{
    if (maximized == m_isMaximized) {
        updateWindowStateButtons();
        return;
    }
    if (maximized) {
        m_normalGeometry = geometry();
        QScreen *screen = QGuiApplication::screenAt(geometry().center());
        if (!screen)
            screen = QGuiApplication::primaryScreen();
        m_isMaximized = true;
        if (screen)
            setGeometry(screen->availableGeometry());
        showMaximized();
    } else {
        m_isMaximized = false;
        showNormal();
        QRect target = m_normalGeometry.isValid()
                           ? m_normalGeometry
                           : QRect(pos(), QSize(kDefaultWidth, kDefaultHeight));
        target.setSize(target.size().expandedTo(minimumSize()));
        setGeometry(target);
        move(boundedPosition(target.topLeft()));
    }
    if (layout())
        layout()->setContentsMargins(m_isMaximized ? 0 : 1, m_isMaximized ? 0 : 1,
                                     m_isMaximized ? 0 : 1, m_isMaximized ? 0 : 1);
    updateWindowStateButtons();
    updateRoundedMask();
    update();
    m_geometrySaveTimer.start();
    emit geometryChanged();
}

void SmartSpaceWidget::toggleMaximized()
{
    setWindowMaximized(!isWindowMaximized());
}

void SmartSpaceWidget::minimizeWindow()
{
    showMinimized();
}

void SmartSpaceWidget::updateWindowStateButtons()
{
    if (auto *bar = qobject_cast<AiSpace::WindowTitleBarWidget *>(m_actionRail))
        bar->setAppearance(m_themeMode == 2, isActiveWindow(), isWindowMaximized());
    if (m_sizeGrip)
        m_sizeGrip->setVisible(!isWindowMaximized());
}

void SmartSpaceWidget::setSettingsViewVisible(bool visible)
{
    if (!m_mainStack || !m_settingsHost || !m_workspacePage)
        return;
    if (visible) {
        // Rebuild every time so the page reflects the current saved values.
        if (QLayout *hostLayout = m_settingsHost->layout()) {
            while (QLayoutItem *item = hostLayout->takeAt(0)) {
                if (QWidget *w = item->widget())
                    w->deleteLater();
                delete item;
            }
            hostLayout->addWidget(createSettingsPage(m_settingsHost));
        }
        m_mainStack->setCurrentWidget(m_settingsHost);
    } else {
        m_mainStack->setCurrentWidget(m_workspacePage);
    }
    m_settingsViewVisible = visible;
    if (m_searchNavButton) {
        m_searchNavButton->setVisible(visible);
        const QSignalBlocker blocker(m_searchNavButton);
        m_searchNavButton->setChecked(!visible);
    }
    if (m_moreButton) {
        const QSignalBlocker blocker(m_moreButton);
        m_moreButton->setChecked(visible);
    }
    emit geometryChanged();
}

// Focus changes affect only the title controls.
bool SmartSpaceWidget::event(QEvent *event)
{
    const bool repaint =
        event->type() == QEvent::WindowActivate || event->type() == QEvent::WindowDeactivate;
    const bool result = QWidget::event(event);
    if (event->type() == QEvent::WindowStateChange) {
        const bool nativeMaximized = isMaximized();
        if (nativeMaximized && !m_isMaximized) {
            m_normalGeometry = normalGeometry();
            m_isMaximized = true;
        } else if (!nativeMaximized && !isMinimized())
            m_isMaximized = false;
        updateWindowStateButtons();
        updateRoundedMask();
    }
    if (repaint && m_actionRail) {
        updateWindowStateButtons();
        update();
    }
    return result;
}
