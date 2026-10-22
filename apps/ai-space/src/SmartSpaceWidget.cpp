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

class CompactKnowledgeStatus : public QLabel {
public:
    explicit CompactKnowledgeStatus(QWidget *parent):QLabel(parent){}
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);painter.setPen(palette().color(QPalette::WindowText));
        const QString brief=text().simplified();
        painter.drawText(contentsRect(),Qt::AlignVCenter|Qt::AlignLeft,fontMetrics().elidedText(brief+QStringLiteral(" · 详情 ›"),Qt::ElideRight,contentsRect().width()));
    }
};

namespace {

// UKUI's native message helper ignores our surface styling but inherits its
// light foreground. Keep confirmations inside a regular, explicitly styled
// QDialog so the window and its text always use the same color scheme.
QMessageBox::StandardButton smartMessage(QWidget *parent, const QString &title,
    const QString &text, QMessageBox::Icon icon, QMessageBox::StandardButtons buttons,
    QMessageBox::StandardButton defaultButton)
{
    QDialog dialog(parent, Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint);
    dialog.setObjectName("smartSpaceMessage");
    dialog.setWindowTitle(title);
    const QColor inherited = parent ? parent->palette().color(QPalette::WindowText) : QColor("#0f172a");
    const bool dark = inherited.lightness() > 128;
    const QString bg = dark ? "#131c31" : "#ffffff";
    const QString fg = dark ? "#f1f5f9" : "#0f172a";
    const QString border = dark ? "#26354d" : "#cbd5e1";
    const QString button = dark ? "#1e293b" : "#f1f5f9";
    const QString accent = dark ? "#3b82f6" : "#2563eb";
    QPalette pal = dialog.palette();
    pal.setColor(QPalette::Window, QColor(bg));
    pal.setColor(QPalette::WindowText, QColor(fg));
    pal.setColor(QPalette::ButtonText, QColor(fg));
    dialog.setPalette(pal);
    dialog.setStyleSheet(QStringLiteral(
        "QDialog#smartSpaceMessage { background: %1; color: %2; }"
        "QDialog#smartSpaceMessage QLabel { color: %2; background: transparent; font-size: 14px; }"
        "QDialog#smartSpaceMessage QPushButton { color: %2; background: %3; border: 1px solid %4;"
        "border-radius: 8px; padding: 7px 18px; min-width: 64px; font-weight: 500; }"
        "QDialog#smartSpaceMessage QPushButton:focus, QDialog#smartSpaceMessage QPushButton:hover {"
        "border: 1px solid %5; background: %5; color: #ffffff; }").arg(bg, fg, button, border, accent));
    auto *layout = new QVBoxLayout(&dialog); layout->setContentsMargins(24,22,24,20);
    auto *row = new QHBoxLayout; row->setSpacing(16);
    auto *symbol = new QLabel(&dialog);
    symbol->setPixmap(dialog.style()->standardIcon(icon==QMessageBox::Question
        ? QStyle::SP_MessageBoxQuestion : icon==QMessageBox::Warning
        ? QStyle::SP_MessageBoxWarning : QStyle::SP_MessageBoxInformation).pixmap(32,32));
    row->addWidget(symbol,0,Qt::AlignTop);
    auto *label = new QLabel(text,&dialog); label->setObjectName("smartMessageText");
    label->setTextFormat(Qt::PlainText); label->setWordWrap(true); label->setMinimumWidth(380);
    row->addWidget(label,1); layout->addLayout(row); layout->addSpacing(18);
    auto *box = new QDialogButtonBox(&dialog); layout->addWidget(box);
    QMessageBox::StandardButton result = QMessageBox::Cancel;
    for (auto choice : {QMessageBox::Yes,QMessageBox::No,QMessageBox::Ok,QMessageBox::Cancel}) {
        if (!buttons.testFlag(choice)) continue;
        const bool startsTask = title == QStringLiteral("快速全量索引")
            || title == QStringLiteral("OCR 检测与补全");
        const QString caption = choice==QMessageBox::Yes ? (startsTask ? "开始" : "确定") : choice==QMessageBox::No
            ? "取消" : choice==QMessageBox::Ok ? "确定" : "取消";
        auto *buttonWidget = box->addButton(caption, choice==QMessageBox::Yes || choice==QMessageBox::Ok
            ? QDialogButtonBox::AcceptRole : QDialogButtonBox::RejectRole);
        buttonWidget->setProperty("messageChoice",int(choice));
        buttonWidget->setDefault(choice==defaultButton);
        if (choice==defaultButton) buttonWidget->setFocus();
        QObject::connect(buttonWidget,&QPushButton::clicked,&dialog,[&dialog,&result,choice] {
            result=choice; dialog.accept();
        });
    }
    dialog.resize(560,dialog.sizeHint().height());
    dialog.exec();
    return result;
}
QMessageBox::StandardButton smartQuestion(QWidget *p,const QString &title,const QString &text,
    QMessageBox::StandardButtons buttons,QMessageBox::StandardButton defaultButton)
{ return smartMessage(p,title,text,QMessageBox::Question,buttons,defaultButton); }
void smartInformation(QWidget *p,const QString &title,const QString &text)
{ smartMessage(p,title,text,QMessageBox::Information,QMessageBox::Ok,QMessageBox::Ok); }
void smartWarning(QWidget *p,const QString &title,const QString &text)
{ smartMessage(p,title,text,QMessageBox::Warning,QMessageBox::Ok,QMessageBox::Ok); }

constexpr int kDefaultWidth = 980;
constexpr int kDefaultHeight = 620;
// Rich result cards use a few lightweight child widgets each. Build them in
// explicit pages so the complete index remains searchable without creating
// thousands of QWidget objects at once.
constexpr int kResultsPageSize = 100;
constexpr int kInitialResultsPageSize = 32;
constexpr int kMaxWatchedDirectories = 512;
constexpr int kKnowledgeErrorLimit = 64 * 1024;
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

bool signalOwnedProcessGroup(QProcess *process, int signalNumber)
{
    if (!process || process->state() == QProcess::NotRunning)
        return false;
    const qint64 rawPid = process->processId();
    if (rawPid <= 1 || rawPid > std::numeric_limits<pid_t>::max())
        return false;
    const pid_t pid = static_cast<pid_t>(rawPid);
    // A negative PID is only safe when this exact child is the process-group
    // leader created by setsid. Never signal an inherited desktop/session
    // group if the launcher or platform behavior changes.
    if (::getpgid(pid) != pid)
        return false;
    return ::kill(-pid, signalNumber) == 0;
}

void appendLimitedProcessOutput(QProcess *process, QByteArray &buffer,
                                const QByteArray &chunk, int limit)
{
    const int room = qMax(0, limit - buffer.size());
    if (room > 0)
        buffer.append(chunk.left(room));
    if (chunk.size() > room && process)
        process->setProperty("responseTruncated", true);
}

class AspectRatioSizeGrip final : public QWidget
{
public:
    AspectRatioSizeGrip(QWidget *target, QWidget *parent)
        : QWidget(parent), m_target(target)
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
        m_ratio = qMax(0.1, m_startSize.width() /
                              static_cast<qreal>(qMax(1, m_startSize.height())));
        grabMouse();
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_dragging || !m_target)
            return QWidget::mouseMoveEvent(event);
        const QPoint delta = event->globalPos() - m_startGlobal;
        const qreal horizontal = qAbs(delta.x()) /
            static_cast<qreal>(qMax(1, m_startSize.width()));
        const qreal vertical = qAbs(delta.y()) /
            static_cast<qreal>(qMax(1, m_startSize.height()));
        int width = horizontal >= vertical
            ? m_startSize.width() + delta.x()
            : qRound((m_startSize.height() + delta.y()) * m_ratio);

        const QSize minimum = m_target->minimumSize();
        int minimumWidth = qMax(minimum.width(),
                                qCeil(minimum.height() * m_ratio));
        int maximumWidth = QWIDGETSIZE_MAX;
        if (QWidget *container = m_target->parentWidget()) {
            const QPoint origin = m_target->isWindow()
                ? container->mapFromGlobal(m_startPosition) : m_startPosition;
            const int availableWidth = container->width() - origin.x();
            const int availableHeight = container->height() - origin.y();
            maximumWidth = qMin(availableWidth,
                                qFloor(availableHeight * m_ratio));
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
        painter.drawLine(width() - 5, height() - 12,
                         width() - 12, height() - 5);
        painter.drawLine(width() - 5, height() - 7,
                         width() - 7, height() - 5);
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

struct FormatOption {
    const char *id;
    const char *label;
};

const QVector<FormatOption> &formatOptions()
{
    static const QVector<FormatOption> options{
        {"pdf", "PDF"},
        {"docx", "DOCX"}, {"doc", "DOC"}, {"wps", "WPS"}, {"odt", "ODT"},
        {"pptx", "PPTX"}, {"ppt", "PPT"}, {"dps", "DPS"}, {"odp", "ODP"},
        {"xlsx", "XLSX"}, {"xls", "XLS"}, {"et", "ET"}, {"ods", "ODS"},
        {"txt", "TXT"}, {"md", "MD"}, {"markdown", "MARKDOWN"},
        {"csv", "CSV"}, {"tsv", "TSV"}, {"rtf", "RTF"},
        {"json", "JSON"}, {"xml", "XML"}, {"yaml", "YAML"},
        {"yml", "YML"}, {"html", "HTML"}, {"htm", "HTM"},
        {"log", "LOG"}, {"ini", "INI"}, {"conf", "CONF"},
        {"jpg", "JPG"}, {"jpeg", "JPEG"}, {"png", "PNG"},
        {"webp", "WEBP"}, {"tif", "TIF"}, {"tiff", "TIFF"},
        {"bmp", "BMP"}, {"gif", "GIF"},
        {"css", "CSS"}, {"js", "JS"}, {"ts", "TS"},
        {"cpp", "CPP"}, {"c", "C"}, {"h", "H"}, {"hpp", "HPP"},
        {"py", "PY"}, {"sh", "SH"}, {"desktop", "DESKTOP"},
        {"__other__", "其他或无扩展名"}
    };
    return options;
}

const QSet<QString> &knownFormatIds()
{
    static const QSet<QString> ids = [] {
        QSet<QString> result;
        for (const FormatOption &option : formatOptions()) {
            const QString id = QString::fromLatin1(option.id);
            if (id != QLatin1String("__other__"))
                result.insert(id);
        }
        return result;
    }();
    return ids;
}

bool formatAllowed(const QString &suffix, const QStringList &selected)
{
    if (selected.isEmpty())
        return true;
    const QString extension = suffix.toLower().remove(QLatin1Char('.'));
    if (selected.contains(extension))
        return true;
    return selected.contains(QStringLiteral("__other__")) &&
           !knownFormatIds().contains(extension);
}

QString cssColor(const QColor &color)
{
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(color.red()).arg(color.green()).arg(color.blue()).arg(color.alpha());
}

void styleModernCombos(QWidget *root)
{
    if (!root)
        return;
    const QList<QComboBox *> combos = root->findChildren<QComboBox *>();
    for (QComboBox *combo : combos) {
        if (!qobject_cast<QListView *>(combo->view())) {
            auto *view = new QListView(combo);
            view->setFrameShape(QFrame::NoFrame);
            combo->setView(view);
        }
    }
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
            ? static_cast<QMouseEvent *>(event)->globalPos() : QCursor::pos();
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
        pos.setX(qBound(avail.left(), pos.x(), qMax(avail.left(), avail.right() + 1 - menu.width())));
        pos.setY(qBound(avail.top(), pos.y(), qMax(avail.top(), avail.bottom() + 1 - menu.height())));
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
    star.cubicTo(c.x() + r * 0.14, c.y() - r * 0.14, c.x() + r * 0.14, c.y() - r * 0.14, c.x() + r, c.y());
    star.cubicTo(c.x() + r * 0.14, c.y() + r * 0.14, c.x() + r * 0.14, c.y() + r * 0.14, c.x(), c.y() + r);
    star.cubicTo(c.x() - r * 0.14, c.y() + r * 0.14, c.x() - r * 0.14, c.y() + r * 0.14, c.x() - r, c.y());
    star.cubicTo(c.x() - r * 0.14, c.y() - r * 0.14, c.x() - r * 0.14, c.y() - r * 0.14, c.x(), c.y() - r);
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
            painter.drawLine(
                QPointF(24 + std::cos(radians) * 10,
                        24 + std::sin(radians) * 10),
                QPointF(24 + std::cos(radians) * 16,
                        24 + std::sin(radians) * 16));
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

QIcon modernRailIcon(const QString &key, const QColor &normal,
                     const QColor &active, const QColor &selected)
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
    if (!source) return;
    auto repair = [source] {
        QWidget *top = source->window();
        QWidget *embedded = source;
        while (embedded->parentWidget() && embedded->parentWidget() != top)
            embedded = embedded->parentWidget();
        if (top) top->repaint();
        if (embedded && embedded != top) embedded->repaint();
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
        if (urls.isEmpty()) return;
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
class FolderBadgeDelegate final : public QStyledItemDelegate {
public:
    explicit FolderBadgeDelegate(QObject *parent) : QStyledItemDelegate(parent) {}
    void paint(QPainter *p,const QStyleOptionViewItem &option,const QModelIndex &index) const override {
        if (!index.data(kFolderCountRole).isValid()) {
            QStyledItemDelegate::paint(p,option,index);return;
        }
        QStyleOptionViewItem opt(option);initStyleOption(&opt,index);
        const QString title=opt.text;
        const QIcon icon=opt.icon;
        opt.text.clear();opt.icon=QIcon();
        const QWidget *widget=opt.widget;
        (widget?widget->style():qApp->style())->drawControl(QStyle::CE_ItemViewItem,&opt,p,widget);
        const bool light=widget && widget->window()->property("smartLightTheme").toBool();
        const bool selected=opt.state & QStyle::State_Selected;
        const QColor ink=selected?QColor(light?"#4f46e5":"#a5b4fc"):opt.palette.color(QPalette::Text);
        const QColor muted=light?QColor("#64748b"):QColor("#94a3b8");
        p->save();p->setRenderHint(QPainter::Antialiasing);
        const int extent=qMin(opt.decorationSize.width(),qMax(12,opt.rect.height()-10));
        const QRect iconRect(opt.rect.left()+10,opt.rect.center().y()-extent/2,extent,extent);
        icon.paint(p,iconRect,Qt::AlignCenter);
        QFont badgeFont=opt.font;badgeFont.setPixelSize(10);p->setFont(badgeFont);
        const QString count=QLocale().toString(index.data(kFolderCountRole).toInt());
        const int badgeWidth=qMin(64,p->fontMetrics().horizontalAdvance(count)+14);
        const int badgeHeight=qMin(20,opt.rect.height()-6);
        const QRect badge(opt.rect.right()-badgeWidth-9,opt.rect.center().y()-badgeHeight/2,badgeWidth,badgeHeight);
        p->setPen(Qt::NoPen);p->setBrush(selected?QColor(light?"#eef2ff":"#30395e"):QColor(light?"#e8edf3":"#1e293b"));
        p->drawRoundedRect(badge,5,5);p->setPen(selected?ink:muted);p->drawText(badge,Qt::AlignCenter,count);
        QFont titleFont=opt.font;titleFont.setBold(selected);p->setFont(titleFont);p->setPen(ink);
        const QRect textRect(iconRect.right()+9,opt.rect.top(),qMax(0,badge.left()-iconRect.right()-16),opt.rect.height());
        p->drawText(textRect,Qt::AlignVCenter|Qt::AlignLeft,p->fontMetrics().elidedText(title,Qt::ElideRight,textRect.width()));
        p->restore();
    }
};

class SmartFileCard final : public QWidget
{
public:
    explicit SmartFileCard(std::function<void()> activate,
                           QWidget *parent = nullptr)
        : QWidget(parent), m_activate(std::move(activate))
    {
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_StyledBackground,true);
    }
    void setPreviewButton(QToolButton *button) {
        m_preview=button;button->installEventFilter(this);updatePreviewVisibility();
    }
    void setSelectedState(bool selected) {
        if (m_selected==selected) return;
        m_selected=selected;setProperty("selected",selected);
        style()->unpolish(this);style()->polish(this);update();updatePreviewVisibility();
    }
protected:
    void enterEvent(QEvent *event) override {m_hovered=true;updatePreviewVisibility();QWidget::enterEvent(event);}
    void leaveEvent(QEvent *event) override {m_hovered=false;updatePreviewVisibility();QWidget::leaveEvent(event);}
    bool eventFilter(QObject *watched,QEvent *event) override {
        if (watched==m_preview && (event->type()==QEvent::FocusIn || event->type()==QEvent::FocusOut))
            updatePreviewVisibility(event->type()==QEvent::FocusIn);
        return QWidget::eventFilter(watched,event);
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
        if (event->button() == Qt::LeftButton && rect().contains(event->pos()) &&
            m_activate) {
            m_activate();
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

private:
    void updatePreviewVisibility(bool focused=false) {
        if (m_preview) m_preview->setVisible((isVisible() && rect().contains(mapFromGlobal(QCursor::pos()))) || m_hovered || m_selected || focused || m_preview->hasFocus());
    }
    QToolButton *m_preview=nullptr;
    bool m_hovered=false,m_selected=false;
    std::function<void()> m_activate;
};

QString normalizedPath(QString path)
{
    path = QDir::cleanPath(path);
    while (path.size() > 1 && path.endsWith(QLatin1Char('/')))
        path.chop(1);
    return path;
}

bool pathIsInside(const QString &path, const QString &folder)
{
    const QString child = normalizedPath(path);
    const QString parent = normalizedPath(folder);
    return child == parent || child.startsWith(parent + QLatin1Char('/'));
}

bool copyDirectoryTree(const QString &sourcePath, const QString &targetPath,
                       QString *error)
{
    const QDir source(sourcePath);
    if (!source.exists()) {
        if (error)
            *error = QStringLiteral("源目录不存在");
        return false;
    }
    if (!QDir().mkpath(targetPath)) {
        if (error)
            *error = QStringLiteral("无法创建目标目录");
        return false;
    }
    const QFileInfoList entries = source.entryInfoList(
        QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden | QDir::System,
        QDir::DirsFirst | QDir::Name);
    for (const QFileInfo &entry : entries) {
        const QString target = QDir(targetPath).filePath(entry.fileName());
        if (entry.isDir()) {
            if (!copyDirectoryTree(entry.absoluteFilePath(), target, error))
                return false;
            continue;
        }
        QFile::remove(target);
        if (!QFile::copy(entry.absoluteFilePath(), target)) {
            if (error)
                *error = QStringLiteral("无法复制文件：%1")
                    .arg(entry.fileName());
            return false;
        }
    }
    return true;
}

QString categoryTitle(const QString &category)
{
    if (category == QLatin1String("image")) return QStringLiteral("图片");
    if (category == QLatin1String("pdf")) return QStringLiteral("PDF");
    if (category == QLatin1String("presentation")) return QStringLiteral("演示");
    if (category == QLatin1String("spreadsheet")) return QStringLiteral("表格");
    if (category == QLatin1String("document")) return QStringLiteral("文档");
    return QStringLiteral("其他");
}

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

QString formattedFileSize(qint64 size)
{
    if (size < 0)
        return QString();
    constexpr qreal kib = 1024.0;
    constexpr qreal mib = kib * 1024.0;
    constexpr qreal gib = mib * 1024.0;
    if (size >= gib)
        return QStringLiteral("%1 GB").arg(size / gib, 0, 'f', 1);
    if (size >= mib)
        return QStringLiteral("%1 MB").arg(size / mib, 0, 'f', 1);
    if (size >= kib)
        return QStringLiteral("%1 KB").arg(size / kib, 0, 'f', 0);
    return QStringLiteral("%1 B").arg(size);
}

QString formattedModified(const QDateTime &modified)
{
    if (!modified.isValid())
        return QString();
    const QDateTime local = modified.toLocalTime();
    const QDate today = QDate::currentDate();
    if (local.date() == today)
        return QStringLiteral("今天 %1").arg(local.time().toString(QStringLiteral("HH:mm")));
    if (local.date() == today.addDays(-1))
        return QStringLiteral("昨天 %1").arg(local.time().toString(QStringLiteral("HH:mm")));
    return local.toString(QStringLiteral("MM-dd HH:mm"));
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
    const QRegularExpression parts(
        QStringLiteral("[\\p{Han}]+|[A-Za-z]+|[0-9]+"));
    QRegularExpressionMatchIterator iterator = parts.globalMatch(query);
    while (iterator.hasNext()) {
        const QString part = iterator.next().captured();
        add(part);
        const bool han = QRegularExpression(
            QStringLiteral("^[\\p{Han}]+$")).match(part).hasMatch();
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
    std::sort(result.begin(), result.end(), [](const QString &left,
                                                const QString &right) {
        return left.size() == right.size() ? left < right
                                           : left.size() > right.size();
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
        if (name.contains(foldedQuery)) score += kExactNameScore;
        if (content.contains(foldedQuery)) score += kExactContentScore;
        if (path.contains(foldedQuery)) score += kExactPathScore;
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
        if (inName) score += kTermNameScore + lengthBonus;
        if (inContent) score += kTermContentScore + lengthBonus;
        if (inPath) score += kTermPathScore + lengthBonus;
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

SmartSpaceWidget::SmartSpaceWidget(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent,false);
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
    connect(&m_geometrySaveTimer, &QTimer::timeout, this, [this] {
        saveSettings();
    });
    applyTheme();
    rebuildFileScopePill();
    rebuildCategoryPills();
    updateActionState();
    updateWindowStateButtons();
    updateRoundedMask();

    m_jobController=new KnowledgeJobController(this);

    m_watcher = new QFileSystemWatcher(this);
    connect(m_watcher, &QFileSystemWatcher::directoryChanged,
            this, [this] { scheduleReindex(); });
    connect(m_watcher, &QFileSystemWatcher::fileChanged,
            this, [this] { scheduleReindex(); });

    m_reindexDebounce.setSingleShot(true);
    m_reindexDebounce.setInterval(4000);
    connect(&m_reindexDebounce, &QTimer::timeout,
            this, &SmartSpaceWidget::startIndexing);

    connect(&m_periodicRefresh, &QTimer::timeout,
            this, &SmartSpaceWidget::startIndexing);
    configureIndexScheduling();
    // Login brings up the desktop, file manager and session services in
    // several waves. QFileSystemWatcher can report those startup changes as
    // ordinary directory changes, so keep automatic policies disarmed while
    // the desktop session settles.
    QTimer::singleShot(30000, this, [this] {
        m_automaticIndexingReady = true;
    });

    if (QFileInfo::exists(indexPath())) {
        loadIndexAsync();
    } else {
        rebuildWatches();
        m_statusLabel->setText(QStringLiteral(
            "尚未建立索引 · 从知识库管理选择存储盘并分阶段建库"));
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
    if(m_providerAuditProcess){m_providerAuditProcess->kill();m_providerAuditProcess->waitForFinished(200);}
    if(m_knowledgeSearchProcess){m_knowledgeSearchProcess->kill();m_knowledgeSearchProcess->waitForFinished(200);}
    if(m_libraryPreviewProcess){m_libraryPreviewProcess->kill();m_libraryPreviewProcess->waitForFinished(200);}
    saveSettings();
    delete m_jobController;m_jobController=nullptr;
    m_knowledgeProcess=nullptr;
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
    auto makeUtility = [this](const QString &key,const QString &tip) {
        auto *button = new QToolButton(m_actionRail);
        button->setProperty("titleControl",true);
        button->setProperty("railIconKey",key);
        button->setToolTip(tip); button->setAccessibleName(tip);
        button->setFixedSize(28,26); button->setCursor(Qt::PointingHandCursor);
        return button;
    };
    m_pinButton = makeUtility("pin",QStringLiteral("置顶窗口")); m_pinButton->setCheckable(true);
    m_themeToggleButton = makeUtility("moon",QStringLiteral("切换明暗主题"));
    m_moreButton = makeUtility("settings",QStringLiteral("偏好设置")); m_moreButton->setCheckable(true);
    titleBar->attachHostUtilityButtons(m_pinButton,m_themeToggleButton,m_moreButton);
    m_searchNavButton = new QToolButton(m_actionRail);
    m_searchNavButton->setText(QStringLiteral("‹ 返回检索"));
    m_searchNavButton->setProperty("titleControl",true);
    m_searchNavButton->hide();
    static_cast<QHBoxLayout *>(titleBar->layout())->insertWidget(4,m_searchNavButton);
    connect(m_searchNavButton,&QToolButton::clicked,this,[this]{setSettingsViewVisible(false);});
    connect(m_moreButton,&QToolButton::clicked,this,[this]{setSettingsViewVisible(!m_settingsViewVisible);});
    m_minimizeButton=titleBar->minimizeButton();
    m_maximizeButton=titleBar->maximizeButton();
    m_windowCloseButton=titleBar->closeButton();
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

    auto makeToolButton = [this](QHBoxLayout *row, const QString &icon,
                                 const QString &text, const QString &tip,
                                 const QString &role) {
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
    m_searchEdit->setPlaceholderText(
        QStringLiteral("搜索文件与内容 · Enter 检索"));
    m_searchEdit->setToolTip(QStringLiteral(
        "输入后按 Enter 执行搜索\n"
        "空格分隔多个必须同时命中的条件，例如：宁波 pdf\n"
        "正则表达式以 re: 开头，例如：re:^宁波.*\\.pdf$"));
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setMinimumHeight(36);
    m_searchEdit->setMinimumWidth(120);
    m_searchEdit->addAction(QIcon::fromTheme(QStringLiteral("edit-find")),
                            QLineEdit::LeadingPosition);
    commandRow->addWidget(m_searchEdit, 1);
    m_searchButton = makeToolButton(
        commandRow, QString(), QStringLiteral("搜索"),
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
    connect(m_categoryCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index) {
        m_selectedCategories.clear();
        if (m_categoryCombo && index > 0)
            m_selectedCategories << m_categoryCombo->itemData(index).toString();
        updateResults(true);
    });

    m_sortCombo=new QComboBox(m_header);
    m_sortCombo->setObjectName("smartSortFilter");
    m_sortCombo->addItem(QStringLiteral("按相关度"),0);
    m_sortCombo->addItem(QStringLiteral("按修改时间"),1);
    m_sortCombo->addItem(QStringLiteral("按文件大小"),2);
    m_sortCombo->setFixedWidth(146);m_sortCombo->setMinimumHeight(36);
    commandRow->addWidget(m_sortCombo);
    connect(m_sortCombo,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this](int index){
        m_resultSortMode=index;saveSettings();updateResults(false);
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
    m_folderPanel = createPanel(
        QStringLiteral("文件夹拾取"), m_folderScroll,
        QStringLiteral("smartFolderPanel"));

    m_fileList = createEntryList();
    m_fileList->setObjectName("smartFileList");
    connect(m_fileList,&QListWidget::itemSelectionChanged,this,[this]{
        for (int row=0;row<m_fileList->count();++row) {
            auto *item=m_fileList->item(row);
            if (auto *card=dynamic_cast<SmartFileCard *>(m_fileList->itemWidget(item))) card->setSelectedState(item->isSelected());
        }
    });
    m_filePanel = createPanel(
        QStringLiteral("匹配结果"), m_fileList,
        QStringLiteral("smartFilePanel"));

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
    m_previewCloseButton->setFixedSize(28,28);
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
    connect(m_previewText,&QTextBrowser::anchorClicked,this,[this](const QUrl &url){
        if(url.scheme()==QLatin1String("aispace-next"))if(const auto *entry=entryForPath(m_previewPath))readLibraryPreview(*entry,url.path().toInt());
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
    m_splitter->setStretchFactor(2,1);
    m_previewPanel->hide();

    m_statusContainer = new QWidget(this);
    m_statusContainer->setObjectName("smartStatusBar");
    m_statusContainer->setFixedHeight(AiSpace::WindowMetrics::BottomStatusHeight);
    auto *statusRow = new QHBoxLayout(m_statusContainer);
    statusRow->setContentsMargins(10,0,4,0); statusRow->setSpacing(4);
    m_statusLabel = new CompactKnowledgeStatus(m_statusContainer);
    m_statusLabel->setObjectName(QStringLiteral("smartSpaceStatus"));
    m_statusLabel->setText(QStringLiteral("尚未建立索引"));m_statusLabel->setCursor(Qt::PointingHandCursor);m_statusLabel->setToolTip(QStringLiteral("点击查看完整状态、OCR 待处理量与资源限制"));m_statusLabel->installEventFilter(this);m_statusLabel->setFocusPolicy(Qt::StrongFocus);
    m_statusLabel->setMinimumWidth(0);
    m_statusLabel->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
    statusRow->addWidget(m_statusLabel,1);
    m_indexProgress->setParent(m_statusContainer);
    m_indexProgress->setMinimumWidth(70); m_indexProgress->setMaximumWidth(180);
    statusRow->addWidget(m_indexProgress,1);
    auto createTask = [&](const QString &key,const QString &text,const QString &tip) {
        auto *button = new QToolButton(m_statusContainer);
        button->setProperty("railAction",true);button->setProperty("railIconKey",key);
        button->setText(text);button->setToolTip(tip);button->setAccessibleName(tip);
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setIconSize(QSize(14,14));button->setFixedHeight(28);
        statusRow->addWidget(button);return button;
    };
    m_idleIndexButton=createTask("database",QStringLiteral("① 全量扫描"),QStringLiteral("第1步：建立文件清单，不提取正文"));
    m_textIndexButton=createTask("refresh",QStringLiteral("② 文本建库"),QStringLiteral("第2步：按已有清单提取正文，不运行OCR"));
    m_ocrIndexButton=createTask("ocr",QStringLiteral("③ OCR补全"),QStringLiteral("第3步：按已扫描清单识别图片与扫描PDF"));
    m_refreshButton=createTask("refresh",QStringLiteral("增量"),QStringLiteral("在当前知识库基础上重新扫描；未变化的正文会复用，更新或删除有变化的记录"));
    m_pauseIndexButton=createTask("pause",QStringLiteral("暂停"),QStringLiteral("安全暂停当前索引"));
    m_pauseIndexButton->setEnabled(false);
    m_knowledgeMenuButton=createTask("knowledge",QStringLiteral("知识库 ▾"),QStringLiteral("知识库管理"));
    m_knowledgeMenuButton->setObjectName("smartKnowledgeMenu");
    connect(m_knowledgeMenuButton,&QToolButton::clicked,this,&SmartSpaceWidget::showHeaderMenu);
    m_sizeGrip=new AspectRatioSizeGrip(this,m_statusContainer);
    m_sizeGrip->setToolTip(QStringLiteral("拖动缩放AI空间"));
    statusRow->addWidget(m_sizeGrip);
    outer->addWidget(m_statusContainer);
    auto *searchShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+K")),this);
    connect(searchShortcut,&QShortcut::activated,this,[this]{
        setSettingsViewVisible(false);m_searchEdit->setFocus();m_searchEdit->selectAll();
    });

    connect(m_textIndexButton,&QToolButton::clicked,this,[this]{startGuidedBuild(false,false,QStringLiteral("text"));});
    connect(m_refreshButton,&QToolButton::clicked,this,&SmartSpaceWidget::startIndexing);
    connect(m_themeToggleButton, &QToolButton::clicked, this, [this] {
        const int nextMode = (m_themeMode == 2 ? 1 : 2);
        setSimpleTheme(nextMode);
    });
    connect(m_idleIndexButton, &QToolButton::clicked,
            this, [this]{startGuidedBuild(false,false,QStringLiteral("manifest"));});
    connect(m_ocrIndexButton, &QToolButton::clicked,
            this, &SmartSpaceWidget::startOcrIndex);
    connect(m_pauseIndexButton,&QToolButton::clicked,this,[this]{
        if(m_libraryJobBusy){m_statusLabel->setText(QStringLiteral("正在暂停知识库任务 · 已提交内容会保留"));stopIdleFullIndex();}
    });
    connect(m_searchButton, &QToolButton::clicked,
            this, &SmartSpaceWidget::executeSearch);
    connect(m_searchEdit, &QLineEdit::returnPressed,
            this, &SmartSpaceWidget::executeSearch);
    connect(m_pinButton, &QToolButton::clicked, this, [this] {
        setAlwaysOnTop(!m_alwaysOnTop);
    });
    connect(m_minimizeButton, &QToolButton::clicked,
            this, &SmartSpaceWidget::minimizeWindow);
    connect(m_maximizeButton, &QToolButton::clicked,
            this, &SmartSpaceWidget::toggleMaximized);
    connect(m_windowCloseButton, &QToolButton::clicked,
            this, &SmartSpaceWidget::closeRequested);
    connect(m_previewCloseButton, &QToolButton::clicked,
            this, &SmartSpaceWidget::hidePreview);
    connect(m_previewOpenButton, &QPushButton::clicked, this, [this] {
        openPath(m_previewPath);
    });
    connect(m_searchEdit, &QLineEdit::textChanged, this, [this] {
        m_statusLabel->setText(QStringLiteral(
            "搜索条件已修改 · 按 Enter 后生效"));
    });
    connect(m_fileList, &QListWidget::itemClicked,
            this, &SmartSpaceWidget::openFileItem);
    connect(m_fileList, &QListWidget::customContextMenuRequested,
            this, [this](const QPoint &pos) { showEntryMenu(m_fileList, pos); });
    auto *previewShortcut = new QShortcut(QKeySequence(Qt::Key_Space), m_fileList);
    previewShortcut->setContext(Qt::WidgetShortcut);
    connect(previewShortcut, &QShortcut::activated, this, [this] {
        if (QListWidgetItem *item = m_fileList->currentItem())
            showPreviewForPath(item->data(PathRole).toString());
    });
    auto *closePreviewShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    closePreviewShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(closePreviewShortcut, &QShortcut::activated,
            this, &SmartSpaceWidget::hidePreview);

    rebuildCategoryPills();

}

void SmartSpaceWidget::applyTheme()
{
    const bool lightMode = (m_themeMode == 2);
    setProperty("smartLightTheme",lightMode);
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
        const QList<QToolButton *> railButtons =
            findChildren<QToolButton *>();
        for (QToolButton *button : railButtons) {
            const QString key = button->property("railIconKey").toString();
            if (key.isEmpty())
                continue;
            if (button->property("titleClose").toBool()) {
                button->setIcon(modernRailIcon(
                    key, m_mutedColor, Qt::white, Qt::white));
            } else if (button->property("navSegment").toBool()) {
                button->setIcon(modernRailIcon(
                    key, m_mutedColor, m_accentColor, m_accentColor));
            } else if (button->property("titleControl").toBool()) {
                button->setIcon(modernRailIcon(
                    key, m_mutedColor, m_textColor, m_accentColor));
            } else {
                button->setIcon(modernRailIcon(
                    key, m_mutedColor, m_accentColor, Qt::white));
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
    const int normalized = (mode==2 ? 2 : 1);
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
                path.addRoundedRect(
                    QRectF(menu->rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                    12.0, 12.0);
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
    QColor menuBg = extColors && m_customMenuBg.isValid()
        ? customBackground(m_customMenuBg) : m_cardColor;
    if (m_themeMode == 3)
        menuBg = QColor(28, 33, 42, 255);
    QColor menuText = extColors && m_customMenuText.isValid() ? m_customMenuText : m_textColor;
    QColor menuHover = extColors && m_customMenuHover.isValid()
        ? customBackground(m_customMenuHover) : m_accentColor;
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
    )").arg(cssColor(menuBg), cssColor(menuText),
             cssColor(separator), cssColor(menuHover), cssColor(disabled),
             cssColor(separator)));
}

void SmartSpaceWidget::configureIndexScheduling()
{
    m_periodicRefresh.stop();
    if (m_indexMode == 2) {
        m_periodicRefresh.setInterval(
            qBound(5, m_indexIntervalMinutes, 1440) * 60 * 1000);
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
    panelIcon->setPixmap(modernRailGlyph(
        objectName == QLatin1String("smartFolderPanel")
            ? QStringLiteral("folder") : QStringLiteral("list"),
        QColor(QStringLiteral("#2563eb"))).scaled(
            QSize(18, 18), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    titleRow->addWidget(panelIcon);
    titleRow->addWidget(label);
    titleRow->addStretch(1);
    if (objectName == QLatin1String("smartFolderPanel"))
        m_folderTitleLabel = label;
    else if (objectName == QLatin1String("smartFilePanel")) {
        m_fileTitleLabel = label;
        auto makeDensityButton = [panel, titleRow](const QString &text,
                                                   const QString &tip) {
            auto *button = new QToolButton(panel);
            button->setText(text);
            button->setToolTip(tip);
            button->setProperty("panelControl", true);
            button->setFixedSize(28, 26);
            titleRow->addWidget(button);
            return button;
        };
        m_densityDownButton = makeDensityButton(
            QStringLiteral("−"), QStringLiteral("缩小文件与文件夹，显示更多内容"));
        m_densityUpButton = makeDensityButton(
            QStringLiteral("＋"), QStringLiteral("放大文件与文件夹"));
        m_densityDownButton->hide();
        m_densityUpButton->hide();
        connect(m_densityDownButton, &QToolButton::clicked, this, [this] {
            setResultDensity(m_resultDensity - 1);
        });
        connect(m_densityUpButton, &QToolButton::clicked, this, [this] {
            setResultDensity(m_resultDensity + 1);
        });
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
    scope->setToolTip(QStringLiteral(
        "当前检索范围：全部索引文件\n"
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
    picker->setToolTip(m_selectedCategories.isEmpty()
        ? QStringLiteral("当前搜索全部文件类型")
        : QStringLiteral("已选：%1").arg(selectedTitles.join(QStringLiteral("、"))));
    auto *menu = new QMenu(picker);
    applyMenuTheme(menu);
    QAction *all = menu->addAction(QStringLiteral("%1  全部类型")
        .arg(m_selectedCategories.isEmpty() ? QStringLiteral("✔")
                                            : QStringLiteral("○")));
    connect(all, &QAction::triggered, this, [this] {
        m_selectedCategories.clear();
        rebuildCategoryPills();
        updateResults(true);
    });
    menu->addSeparator();
    for (int index = 1; index < m_categoryCombo->count(); ++index) {
        const QString category = m_categoryCombo->itemData(index).toString();
        const bool selected = m_selectedCategories.contains(category);
        QAction *action = menu->addAction(QStringLiteral("%1  %2")
            .arg(selected ? QStringLiteral("✔") : QStringLiteral("○"),
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
    QAction *idle=menu.addAction(smartMenuIcon(QStringLiteral("fast-full")),QStringLiteral("① 全量扫描 · 建立文件清单"));
    QAction *textBuild=menu.addAction(smartMenuIcon(QStringLiteral("refresh")),QStringLiteral("② 文本建库 · 提取正文"));
    QAction *ocrIndex=menu.addAction(smartMenuIcon(QStringLiteral("ocr")),QStringLiteral("③ OCR补全 · 图片 / 扫描 PDF"));
    const bool taskBusy=m_libraryJobBusy || m_knowledgeProcess;
    for(auto *action:{idle,textBuild,ocrIndex})action->setEnabled(!taskBusy);
    menu.addSeparator();
    QAction *refresh=menu.addAction(smartMenuIcon(QStringLiteral("refresh")),QStringLiteral("增量更新 · 重新扫描并复用正文"));refresh->setEnabled(!taskBusy);
    menu.addSeparator();
    QAction *manager = menu.addAction(QIcon::fromTheme(QStringLiteral("folder-open")),QStringLiteral("知识库管理 / 导入与导出…"));
    manager->setObjectName("knowledgeManagerAction");
    connect(manager,&QAction::triggered,this,&SmartSpaceWidget::showKnowledgeManager);
    QAction *knowledgeBuild = menu.addAction(
        QIcon::fromTheme(QStringLiteral("system-search")),
        QStringLiteral("分阶段建库向导…"));
    QAction *knowledgeStats = menu.addAction(
        QIcon::fromTheme(QStringLiteral("view-statistics")),
        QStringLiteral("查看知识库状态"));
    QAction *knowledgeStop = menu.addAction(
        QIcon::fromTheme(QStringLiteral("media-playback-pause")),
        QStringLiteral("暂停知识库任务"));
    const bool knowledgeBusy = m_knowledgeProcess != nullptr;
    knowledgeBuild->setEnabled(!knowledgeBusy);
    knowledgeStats->setEnabled(true);
    knowledgeStop->setEnabled(knowledgeBusy);
    menu.addSeparator();
    QAction *settings = menu.addAction(
        smartMenuIcon(QStringLiteral("settings")),
        QStringLiteral("AI空间设置"));
    QAction *close = menu.addAction(
        smartMenuIcon(QStringLiteral("close")),
        QStringLiteral("关闭AI空间"));

    connect(refresh, &QAction::triggered, this, &SmartSpaceWidget::startIndexing);
    connect(idle,&QAction::triggered,this,[this]{startGuidedBuild(false,false,QStringLiteral("manifest"));});
    connect(textBuild,&QAction::triggered,this,[this]{startGuidedBuild(false,false,QStringLiteral("text"));});
    connect(ocrIndex, &QAction::triggered, this, &SmartSpaceWidget::startOcrIndex);
    connect(knowledgeBuild, &QAction::triggered, this,
            &SmartSpaceWidget::startKnowledgeBuild);
    connect(knowledgeStats,&QAction::triggered,this,&SmartSpaceWidget::showKnowledgeStatus);
    connect(knowledgeStop, &QAction::triggered, this, [this] {
        if (m_knowledgeProcess) {
            m_knowledgeProcess->setProperty("userPaused", true);
            terminateKnowledgeProcess(false);
            m_statusLabel->setText(QStringLiteral(
                "正在暂停知识库任务 · 已完成内容会保留"));
        }
    });
    connect(settings, &QAction::triggered, this,
            &SmartSpaceWidget::showSettingsDialog);
    connect(close, &QAction::triggered, this, &SmartSpaceWidget::closeRequested);
    execMenuAt(menu, m_knowledgeMenuButton);
}

void SmartSpaceWidget::updateResponsiveLayout()
{
    if (!m_splitter)
        return;
    const bool compact = width() < 760;
    const bool breakpointChanged =
        property("smartCompactLayout").toBool() != compact;
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
            m_splitter->setSizes({available * 34 / 100,
                                  available * 66 / 100});
    }
    const bool docked = width() >= 1100;
    if (m_previewPanel && m_resultStack) {
        if (docked && m_previewPanel->parentWidget()!=m_splitter) {
            m_resultStack->setCurrentWidget(m_resultPage);
            m_previewPanel->setParent(m_splitter);m_splitter->addWidget(m_previewPanel);
        } else if (!docked && m_previewPanel->parentWidget()!=m_resultStack) {
            m_previewPanel->setParent(m_resultStack);m_resultStack->addWidget(m_previewPanel);
        }
        if (docked) {
            m_resultStack->setCurrentWidget(m_resultPage);
            m_previewPanel->setVisible(m_previewVisible);
            if (m_previewVisible && !property("previewDocked").toBool())
                m_splitter->setSizes({width()*3/12,width()*5/12,width()*4/12});
        } else m_resultStack->setCurrentWidget(m_previewVisible ? static_cast<QWidget *>(m_previewPanel) : m_resultPage);
        setProperty("previewDocked",docked && m_previewVisible);
        setProperty("previewVisible",m_previewVisible);
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
        QStringLiteral("aiEnabled"),
        QStringLiteral("aiUseMonitorConfig"),
        QStringLiteral("aiApiUrl"),
        QStringLiteral("aiApiKey"),
        QStringLiteral("aiModel"),
        QStringLiteral("aiSendContent"),
        QStringLiteral("knowledgeEnabled"),
        QStringLiteral("knowledgeSendContent"),
        QStringLiteral("currentSpace")};
    for (const QString &key : obsoleteKeys)
        settings.remove(key);
    m_roots = settings.value(QStringLiteral("roots"), defaultRoots()).toStringList();
    m_excludedFolders = settings.value(
        QStringLiteral("excludedFolders")).toStringList();
    m_hiddenFolders = settings.value(
        QStringLiteral("hiddenFolders")).toStringList();
    m_indexedExtensions = settings.value(
        QStringLiteral("indexedExtensions")).toStringList();
    m_visibleExtensions = settings.value(
        QStringLiteral("visibleExtensions")).toStringList();
    m_ocrImages = settings.value(QStringLiteral("ocrImages"), true).toBool();
    m_maxItems = qBound(1000, settings.value(
        QStringLiteral("maxItems"), 25000).toInt(), 100000);
    m_maxContentPerFile = qBound(2000, settings.value(
        QStringLiteral("maxContentPerFile"), 12000).toInt(), 100000);
    m_maxTotalContentMb = qBound(1, settings.value(
        QStringLiteral("maxTotalContentMb"), 16).toInt(), 64);
    m_indexMode = qBound(0, settings.value(
        QStringLiteral("indexMode"), 0).toInt(), 2);
    m_indexIntervalMinutes = qBound(5, settings.value(
        QStringLiteral("indexIntervalMinutes"), 30).toInt(), 1440);
    const int storedThemeMode = qBound(0, settings.value(
        QStringLiteral("themeMode"), 3).toInt(), 6);
    m_customBaseSkin = qBound(0, settings.value(
        QStringLiteral("customBaseSkin"), 0).toInt(), 4);
    m_themeMode = (storedThemeMode==2 ? 2 : 1);
    m_customOpacity = qBound(25, settings.value(
        QStringLiteral("customOpacity"), 100).toInt(), 100);
    m_customColorsEnabled = settings.value(
        QStringLiteral("customColorsEnabled"), false).toBool();
    m_customSurface = QColor(settings.value(
        QStringLiteral("customSurface")).toString());
    m_customCard = QColor(settings.value(
        QStringLiteral("customCard")).toString());
    m_customBorder = QColor(settings.value(
        QStringLiteral("customBorder")).toString());
    m_customText = QColor(settings.value(
        QStringLiteral("customText")).toString());
    m_customMuted = QColor(settings.value(
        QStringLiteral("customMuted")).toString());
    m_customAccent = QColor(settings.value(
        QStringLiteral("customAccent")).toString());
    m_customSearchBg = QColor(settings.value(
        QStringLiteral("customSearchBg")).toString());
    m_customSearchText = QColor(settings.value(
        QStringLiteral("customSearchText")).toString());
    m_customButtonBg = QColor(settings.value(
        QStringLiteral("customButtonBg")).toString());
    m_customButtonText = QColor(settings.value(
        QStringLiteral("customButtonText")).toString());
    m_customButtonHover = QColor(settings.value(
        QStringLiteral("customButtonHover")).toString());
    m_customMenuBg = QColor(settings.value(
        QStringLiteral("customMenuBg")).toString());
    m_customMenuText = QColor(settings.value(
        QStringLiteral("customMenuText")).toString());
    m_customMenuHover = QColor(settings.value(
        QStringLiteral("customMenuHover")).toString());
    m_customFontFamily = settings.value(
        QStringLiteral("customFontFamily")).toString();
    m_customFontSize = qBound(10, settings.value(
        QStringLiteral("customFontSize"), 13).toInt(), 18);
    m_customFontBold = settings.value(
        QStringLiteral("customFontBold"), false).toBool();
    m_resultDensity = qBound(kMinResultDensity, settings.value(
        QStringLiteral("resultDensity"), 0).toInt(), kMaxResultDensity);
    m_resultSortMode = qBound(0, settings.value(
        QStringLiteral("resultSortMode"), 0).toInt(), 2);
    m_autoStart = settings.value(QStringLiteral("autoStart"), false).toBool();
    m_alwaysOnTop = settings.value(
        QStringLiteral("alwaysOnTop"), false).toBool();
    settings.remove(QStringLiteral("defaultHidden"));
    m_knowledgeDirectory = settings.value(
        QStringLiteral("knowledgeDirectory"),
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
            + QStringLiteral("/ai-space/knowledge")).toString();
    m_libraryStorageDirectory = settings.value(QStringLiteral("libraryStorageDirectory"),m_knowledgeDirectory).toString();
    m_storageConfirmed = settings.value(QStringLiteral("storageConfirmed"),false).toBool();
    m_storageReserveGiB = qBound(1,settings.value(QStringLiteral("storageReserveGiB"),2).toInt(),1024);
    m_libraryQuotaGiB = qBound(1,settings.value(QStringLiteral("libraryQuotaGiB"),32).toInt(),4096);
    m_ocrCpuPercent = qBound(5,settings.value(QStringLiteral("ocrCpuPercent"),25).toInt(),100);
    m_ocrMemoryMiB = qBound(256,settings.value(QStringLiteral("ocrMemoryMiB"),1024).toInt(),8192);
    m_knowledgeChunkSize = qBound(600, settings.value(
        QStringLiteral("knowledgeChunkSize"), 1200).toInt(), 3000);
    m_knowledgeOverlap = qBound(0, settings.value(
        QStringLiteral("knowledgeOverlap"), 120).toInt(),
        m_knowledgeChunkSize / 3);
    m_providerApprovedDigest=settings.value(QStringLiteral("providerApprovedDigest")).toString();
    m_providerConfig = settings.value(QStringLiteral("providerConfig"),
                                      providerConfigPath()).toString();
    const QSize storedSize = settings.value(
        QStringLiteral("size"), QSize(kDefaultWidth, kDefaultHeight)).toSize();
    resize(storedSize.expandedTo(minimumSize()));
    QPoint position = settings.value(QStringLiteral("position"),
                                     QPoint(-1, -1)).toPoint();
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
        const QSignalBlocker blocker(m_sortCombo);m_sortCombo->setCurrentIndex(m_resultSortMode);
    }

    const QString testRoots = QString::fromUtf8(qgetenv("AI_SPACE_ROOTS"));
    if (!testRoots.isEmpty())
        m_roots = testRoots.split(QDir::listSeparator(), QString::SkipEmptyParts);
    if (qEnvironmentVariableIsSet("AI_SPACE_OCR"))
        m_ocrImages = qEnvironmentVariableIntValue("AI_SPACE_OCR") != 0;

    if ((position.x() < 0 || position.y() < 0)) {
        const QSize area = parentWidget() ? parentWidget()->size() : QSize(1920, 1080);
        position = QPoint(48,
                          std::max(64, area.height() - height() - 100));
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
    settings.setValue(QStringLiteral("indexIntervalMinutes"),
                      m_indexIntervalMinutes);
    settings.setValue(QStringLiteral("themeMode"), m_themeMode);
    settings.remove(QStringLiteral("glassOptics"));
    settings.setValue(QStringLiteral("customBaseSkin"), m_customBaseSkin);
    settings.setValue(QStringLiteral("customOpacity"), m_customOpacity);
    settings.setValue(QStringLiteral("customColorsEnabled"), m_customColorsEnabled);
    settings.setValue(QStringLiteral("customSurface"),
                      m_customSurface.isValid() ? m_customSurface.name(QColor::HexArgb) : QString());
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
    settings.setValue(QStringLiteral("customSearchBg"),
                      m_customSearchBg.isValid() ? m_customSearchBg.name(QColor::HexArgb) : QString());
    settings.setValue(QStringLiteral("customSearchText"),
                      m_customSearchText.isValid() ? m_customSearchText.name(QColor::HexArgb) : QString());
    settings.setValue(QStringLiteral("customButtonBg"),
                      m_customButtonBg.isValid() ? m_customButtonBg.name(QColor::HexArgb) : QString());
    settings.setValue(QStringLiteral("customButtonText"),
                      m_customButtonText.isValid() ? m_customButtonText.name(QColor::HexArgb) : QString());
    settings.setValue(QStringLiteral("customButtonHover"),
                      m_customButtonHover.isValid() ? m_customButtonHover.name(QColor::HexArgb) : QString());
    settings.setValue(QStringLiteral("customMenuBg"),
                      m_customMenuBg.isValid() ? m_customMenuBg.name(QColor::HexArgb) : QString());
    settings.setValue(QStringLiteral("customMenuText"),
                      m_customMenuText.isValid() ? m_customMenuText.name(QColor::HexArgb) : QString());
    settings.setValue(QStringLiteral("customMenuHover"),
                      m_customMenuHover.isValid() ? m_customMenuHover.name(QColor::HexArgb) : QString());
    settings.setValue(QStringLiteral("customFontFamily"), m_customFontFamily);
    settings.setValue(QStringLiteral("customFontSize"), m_customFontSize);
    settings.setValue(QStringLiteral("customFontBold"), m_customFontBold);
    settings.setValue(QStringLiteral("resultDensity"), m_resultDensity);
    settings.setValue(QStringLiteral("resultSortMode"), m_resultSortMode);
    settings.setValue(QStringLiteral("autoStart"), m_autoStart);
    settings.setValue(QStringLiteral("alwaysOnTop"), m_alwaysOnTop);
    settings.remove(QStringLiteral("defaultHidden"));
    settings.setValue(QStringLiteral("knowledgeDirectory"),
                      m_knowledgeDirectory);
    settings.setValue(QStringLiteral("libraryStorageDirectory"),m_libraryStorageDirectory);
    settings.setValue(QStringLiteral("storageConfirmed"),m_storageConfirmed);
    settings.setValue(QStringLiteral("storageReserveGiB"),m_storageReserveGiB);
    settings.setValue(QStringLiteral("libraryQuotaGiB"),m_libraryQuotaGiB);
    settings.setValue(QStringLiteral("ocrCpuPercent"),m_ocrCpuPercent);
    settings.setValue(QStringLiteral("ocrMemoryMiB"),m_ocrMemoryMiB);
    settings.setValue(QStringLiteral("knowledgeChunkSize"),
                      m_knowledgeChunkSize);
    settings.setValue(QStringLiteral("knowledgeOverlap"),
                      m_knowledgeOverlap);
    settings.setValue(QStringLiteral("providerConfig"), m_providerConfig);
    settings.setValue(QStringLiteral("providerApprovedDigest"),m_providerApprovedDigest);
    {
        const QRect normal = isWindowMaximized() && m_normalGeometry.isValid()
            ? m_normalGeometry : geometry();
        settings.setValue(QStringLiteral("position"),normal.topLeft());
        settings.setValue(QStringLiteral("size"),normal.size());
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
    if(m_autoStart==enabled){if(enabled)emit autoStartChanged(enabled);return;}
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
        m_densityDownButton->setToolTip(
            QStringLiteral("缩小文件与文件夹 · 当前第 %1/%2 档")
                .arg(m_resultDensity - kMinResultDensity + 1)
                .arg(kResultDensityLevels));
    }
    if (m_densityUpButton) {
        m_densityUpButton->setEnabled(m_resultDensity < kMaxResultDensity);
        m_densityUpButton->setToolTip(
            QStringLiteral("放大文件与文件夹 · 当前第 %1/%2 档")
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
        QScreen *screen = QGuiApplication::screenAt(
            QRect(position, size()).center());
        if (!screen)
            screen = QGuiApplication::primaryScreen();
        if (screen)
            bounds = screen->availableGeometry();
    } else if (parentWidget()) {
        bounds = parentWidget()->rect();
    }
    if (!bounds.isValid())
        return position;

    int x = qBound(bounds.left(), position.x(),
                   std::max(bounds.left(), bounds.right() - width() + 1));
    int y = qBound(bounds.top(), position.y(),
                   std::max(bounds.top(), bounds.bottom() - height() + 1));
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
    const QString catalog=QDir(m_knowledgeDirectory).filePath(QStringLiteral("catalog/index.json"));
    if(QFileInfo::exists(catalog))return catalog;
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
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
        + QStringLiteral("/ai-space/providers.json");
}

QString SmartSpaceWidget::indexerPath() const
{
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QStringList candidates{
        appDir.filePath(QStringLiteral("libexec/ai-space/smart_space_indexer.py")),
        appDir.filePath(QStringLiteral(AI_SPACE_LIBEXEC_RELATIVE "/smart_space_indexer.py"))
    };
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
        appDir.filePath(QStringLiteral(AI_SPACE_LIBEXEC_RELATIVE "/smart_space_knowledge.py"))
    };
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
        directory = QStandardPaths::writableLocation(
            QStandardPaths::GenericDataLocation)
            + QStringLiteral("/ai-space/knowledge");
    }
    const QString db=QDir(directory).filePath(QStringLiteral("smart-space-knowledge.sqlite"));
    if(!QFileInfo::exists(db)) {
        QSettings legacy(QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)+QStringLiteral("/kylin/ukui-fences.ini"),QSettings::IniFormat);
        const QString old=QDir(legacy.value(QStringLiteral("smartSpace/knowledgeDirectory"),QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)+QStringLiteral("/ukui-fences/knowledge")).toString()).filePath(QStringLiteral("smart-space-knowledge.sqlite"));
        if(QFileInfo::exists(old))return old;
    }
    return db;
}

QString SmartSpaceWidget::skillDirectoryPath() const
{
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QStringList candidates{
        appDir.filePath(QStringLiteral("share/ai-space/skills/ai-space-index-query")),
        appDir.filePath(QStringLiteral(AI_SPACE_SKILL_RELATIVE))
    };
    for(const auto &candidate:candidates) {
        const auto clean=QDir::cleanPath(candidate);
        if(QFileInfo(QDir(clean).filePath(QStringLiteral("SKILL.md"))).isFile())return clean;
    }
    return {};
}

void SmartSpaceWidget::startKnowledgeTask(
        const QString &task, const QStringList &arguments)
{
    if(property("knowledgeFailuresVisible").toBool()){m_statusLabel->setText(QStringLiteral("请先关闭失败项处理窗口"));return;}
    if(property("knowledgeMigrationPending").toBool()) {
        m_statusLabel->setText(QStringLiteral("旧知识库正在后台导入，当前仍可检索文件快照"));
        return;
    }
    if (m_knowledgeProcess) {
        m_statusLabel->setText(QStringLiteral(
            "知识库任务正在运行 · 可从管理菜单暂停"));
        return;
    }
    const QString worker = knowledgeWorkerPath();
    if (worker.isEmpty()) {
        m_statusLabel->setText(QStringLiteral("找不到本地知识库后台程序"));
        return;
    }
    m_knowledgeOutputBuffer.clear();
    m_knowledgeErrorBuffer.clear();
    m_knowledgeLastObject = QJsonObject();
    setProperty("knowledgeJobStarted",QDateTime::currentDateTime());setProperty("knowledgeStage",QString());setProperty("knowledgeJobOutcome",QStringLiteral("运行中"));
    m_knowledgeProcess=m_jobController->create();
    if(!m_knowledgeProcess)return;
    m_knowledgeProcess->setProperty("knowledgeTask", task);
    connect(m_knowledgeProcess, &QProcess::readyReadStandardOutput,
            this, &SmartSpaceWidget::handleKnowledgeOutput);
    connect(m_knowledgeProcess, &QProcess::readyReadStandardError,
            this, &SmartSpaceWidget::handleKnowledgeOutput);
    connect(m_knowledgeProcess,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus status) {
                finishKnowledgeTask(code, static_cast<int>(status));
            });
    QStringList command;
    if (task == QLatin1String("library")) {
        command << QFileInfo(worker).dir().filePath(QStringLiteral("knowledge_library.py"));
        command.append(arguments);
    } else if (task == QLatin1String("pipeline")) {
        command << QFileInfo(worker).dir().filePath(QStringLiteral("knowledge_jobs.py"));
        command.append(arguments);
    } else {
        command << worker << task;
        command.append(arguments);
    }
    if (task == QLatin1String("pipeline") || task == QLatin1String("library")) {
        m_libraryJobBusy = true;
        m_idleIndexButton->setEnabled(false);m_refreshButton->setEnabled(false);if(m_textIndexButton)m_textIndexButton->setEnabled(false);m_ocrIndexButton->setEnabled(false);
        m_pauseIndexButton->setEnabled(true);
    }
    const QString python = QStandardPaths::findExecutable(
        QStringLiteral("python3"));
    const QString setsid = QStandardPaths::findExecutable(
        QStringLiteral("setsid"));
    QString program = python.isEmpty() ? QStringLiteral("python3") : python;
    QStringList processArguments = command;
    m_knowledgeUsesProcessGroup = false;
    if (!setsid.isEmpty()) {
        processArguments.prepend(program);
        program = setsid;
        m_knowledgeUsesProcessGroup = true;
    }
    m_knowledgeProcess->setProperty(
        "usesProcessGroup", m_knowledgeUsesProcessGroup);
    QProcess *startingProcess = m_knowledgeProcess;
    connect(startingProcess, &QProcess::errorOccurred, this,
        [this, startingProcess](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart || m_knowledgeProcess != startingProcess) return;
            m_statusLabel->setText(QStringLiteral("知识库后台启动失败"));
            startingProcess->deleteLater(); m_knowledgeProcess = nullptr;
            if(m_libraryJobBusy){m_libraryJobBusy=false;m_indexProgress->hide();m_idleIndexButton->setEnabled(true);m_refreshButton->setEnabled(true);if(m_textIndexButton)m_textIndexButton->setEnabled(true);m_ocrIndexButton->setEnabled(true);m_pauseIndexButton->setEnabled(false);}
            m_knowledgeUsesProcessGroup = false;
        });
    startingProcess->start(program, processArguments);
    if (task == QLatin1String("stats")) {
        QPointer<QProcess> process = m_knowledgeProcess;
        QTimer::singleShot(30000, this, [this, process] {
            if (!process || m_knowledgeProcess != process ||
                process->state() == QProcess::NotRunning)
                return;
            process->setProperty("timedOut", true);
            terminateKnowledgeProcess(false);
            QTimer::singleShot(1200, this, [this, process] {
                if (!process || m_knowledgeProcess != process ||
                    process->state() == QProcess::NotRunning)
                    return;
                terminateKnowledgeProcess(true);
            });
        });
    }
}

void SmartSpaceWidget::terminateKnowledgeProcess(bool force)
{
    if(m_jobController)m_jobController->stop(force);
}

void SmartSpaceWidget::showKnowledgeStatus()
{
    if(property("knowledgeStatusVisible").toBool())return;
    QDialog dialog(this,Qt::Dialog|Qt::WindowTitleHint|Qt::WindowMinimizeButtonHint|Qt::WindowCloseButtonHint);
    dialog.setObjectName(QStringLiteral("knowledgeStatusDialog"));dialog.setWindowTitle(QStringLiteral("知识库 · 当前状态详情"));dialog.setWindowIcon(qApp->windowIcon());dialog.resize(760,630);
    const bool light=m_themeMode==2;
    dialog.setStyleSheet(QStringLiteral("QDialog,QPlainTextEdit{background:%1;color:%2;} QPlainTextEdit{border:0;padding:12px;} QPushButton{padding:8px 18px;background:%1;color:%2;border:1px solid #64748b;border-radius:6px;} QPushButton:disabled{color:#64748b;border-color:#64748b;}").arg(light?"#f1f5f9":"#111827",light?"#0f172a":"#e2e8f0"));
    auto *layout=new QVBoxLayout(&dialog);auto *heading=new QLabel(QStringLiteral("知识库 · 当前状态详情"));heading->setStyleSheet(QStringLiteral("font-size:18px;font-weight:600;color:%1;padding:8px;").arg(light?"#0f172a":"#e2e8f0"));layout->addWidget(heading);auto *body=new QPlainTextEdit;body->setObjectName(QStringLiteral("knowledgeStatusDetails"));body->setReadOnly(true);layout->addWidget(body,1);
    auto *buttons=new QHBoxLayout;auto *refresh=new QPushButton(QStringLiteral("刷新库统计"));auto *failuresButton=new QPushButton(QStringLiteral("处理失败 / 已忽略…"));failuresButton->setObjectName("knowledgeManageFailures");auto *close=new QPushButton(QStringLiteral("关闭"));buttons->addWidget(refresh);buttons->addWidget(failuresButton);buttons->addStretch();buttons->addWidget(close);layout->addLayout(buttons);connect(close,&QPushButton::clicked,&dialog,&QDialog::accept);
    auto *process=new QProcess(&dialog);QJsonObject snapshot;QString statsMessage=QStringLiteral("正在读取库统计…");QString lastText;QDateTime sampledAt;
    // Resolve database existence and legacy fallback in the statistics worker.
    // A sleeping removable disk must not block the dialog's live UI refresh.
    const QString statsWorker=QFileInfo(knowledgeWorkerPath()).dir().filePath(QStringLiteral("knowledge_library.py"));
    failuresButton->setShortcut(QKeySequence("Alt+F"));failuresButton->setToolTip(QStringLiteral("处理失败项与已忽略项（Alt+F）"));
    auto render=[&]{
        const bool busy=m_libraryJobBusy;
        QString stage=property("knowledgeStage").toString();
        const QString phase=stage==QLatin1String("manifest")?QStringLiteral("① 全量扫描"):stage==QLatin1String("text")?QStringLiteral("② 文本建库"):stage==QLatin1String("ocr")?QStringLiteral("③ OCR 补全"):stage==QLatin1String("retry")?QStringLiteral("失败项重试"):QStringLiteral("扫描 / 库文件操作");
        failuresButton->setEnabled(!busy && !snapshot.value("path").toString().isEmpty());
        QStringList lines;lines<<QStringLiteral("当前情况")<<QStringLiteral("任务：%1%2").arg(busy?QStringLiteral("运行中 · ")+phase:QStringLiteral("空闲"),property("knowledgeJobOutcome").toString().isEmpty()?QString():QStringLiteral(" · 最近任务：")+property("knowledgeJobOutcome").toString())<<m_statusLabel->text();
        if(busy){
            const auto job=m_knowledgeLastObject;
            if(job.contains(QStringLiteral("processed")) || job.contains(QStringLiteral("progress"))){const auto done=job.value(job.contains(QStringLiteral("processed"))?QStringLiteral("processed"):QStringLiteral("progress")).toDouble(),total=job.value(QStringLiteral("total")).toDouble();lines<<QStringLiteral("清单遍历：%1 / %2 项；尚未遍历 %3 项（含目录、跳过项和复用项，不等于待 OCR 文档）").arg(done,0,'f',0).arg(total,0,'f',0).arg(qMax(0.0,total-done),0,'f',0);}
            if(!job.value(QStringLiteral("path")).toString().isEmpty())lines<<QStringLiteral("当前文件：")+job.value(QStringLiteral("path")).toString();
            if(job.contains(QStringLiteral("blocks")))lines<<QStringLiteral("当前文件已提取 %1 个文本块（不是页数）").arg(job.value(QStringLiteral("blocks")).toInt());
            const auto started=property("knowledgeJobStarted").toDateTime();if(started.isValid())lines<<QStringLiteral("本轮耗时：%1 秒").arg(started.secsTo(QDateTime::currentDateTime()));
        }
        lines<<QString()<<QStringLiteral("知识库统计（已提交数据%1）").arg(sampledAt.isValid()?QStringLiteral(" · ")+sampledAt.toString("HH:mm:ss"):QString());
        if(snapshot.isEmpty())lines<<statsMessage;
        else {
            lines<<QStringLiteral("文档：%1　正文片段：%2　库文件：%3 GiB").arg(snapshot.value("documents").toInt()).arg(snapshot.value("chunks").toInt()).arg(snapshot.value("dbBytes").toDouble()/1073741824.0,0,'f',2);
            const auto states=snapshot.value("extractionStates").toObject();lines<<QStringLiteral("完整：%1　部分：%2　无正文：%3　失败：%4　来源缺失：%5").arg(states.value("complete").toInt()).arg(states.value("partial").toInt()).arg(states.value("no_text").toInt()).arg(states.value("failed").toInt()).arg(states.value("missing").toInt());
            lines<<QStringLiteral("待处理失败 / 缺失：%1　已忽略：%2（仍保留真实提取状态与已有正文）").arg(snapshot.value("failurePending").toInt()).arg(snapshot.value("ignoredDocuments").toInt());
            lines<<QStringLiteral("最近成功建库：")+snapshot.value("lastBuild").toString();
            const auto pending=snapshot.value("ocrPending").toObject();lines<<QString()<<QStringLiteral("OCR 阶段与剩余工作")<<QStringLiteral("当前 OCR：%1").arg(busy&&stage==QLatin1String("ocr")?QStringLiteral("正在补全"):busy&&stage==QLatin1String("retry")&&property("knowledgeRetryOcr").toBool()?QStringLiteral("重试任务按需补全"):QStringLiteral("未运行"));
            lines<<QStringLiteral("已检测且明确待 OCR：%1 份文档（PDF %2 页、图片 %3 张）").arg(pending.value("documents").toInt()).arg(pending.value("pdfPages").toInt()).arg(pending.value("images").toInt());
            lines<<QStringLiteral("OCR 后仍无文字的 PDF 页：%1；无正文图片：%2 张（可能本身没有文字，不自动当作待办）").arg(pending.value("emptyOcrPages").toInt()).arg(pending.value("imagesWithoutText").toInt());
            lines<<QStringLiteral("这是已提交提取记录中的待处理量，不包含尚未检测的文件；正在处理的文档提交后才会更新。无正文不一定需要 OCR。")<<QStringLiteral("剩余总页数 / 预计耗时：尚不能可靠估算，取决于未检测 PDF 页数、图片复杂度与资源限制。");
            lines<<QString()<<QStringLiteral("待处理失败 / 来源缺失示例（最多 20 项；通过下方按钮处理或查看已忽略项）");
            const auto failures=snapshot.value("failures").toArray();if(failures.isEmpty())lines<<QStringLiteral("暂无记录");for(const auto &item:failures){const auto row=item.toObject();lines<<QStringLiteral("• %1：%2").arg(row.value("name").toString(),row.value("reason").toString());}
            if(!statsMessage.isEmpty())lines<<statsMessage;
        }
        const QString configuredDirectory=m_knowledgeDirectory.trimmed().isEmpty()?QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)+QStringLiteral("/ai-space/knowledge"):m_knowledgeDirectory;
        const QString displayedDb=snapshot.value("path").toString(QDir(configuredDirectory).filePath(QStringLiteral("smart-space-knowledge.sqlite")));
        lines<<QString()<<QStringLiteral("存储与资源")<<QStringLiteral("当前个人库：")+displayedDb<<QStringLiteral("新建库位置：")+m_libraryStorageDirectory<<QStringLiteral("至少保留 %1 GiB · 单库上限 %2 GiB · OCR 单核 %3% · 工作进程地址空间 %4 MiB").arg(m_storageReserveGiB).arg(m_libraryQuotaGiB).arg(m_ocrCpuPercent).arg(m_ocrMemoryMiB);
        const auto saved=snapshot.value("checkpoint").toObject();if(!saved.isEmpty())lines<<QStringLiteral("已完成阶段检查点：%1 · %2").arg(saved.value("stage").toString(),saved.value("completed_at").toString());

        const QString text=lines.join('\n');if(text!=lastText){const int scroll=body->verticalScrollBar()->value();body->setPlainText(text);body->verticalScrollBar()->setValue(scroll);lastText=text;}
    };
    auto request=[&]{
        if(process->state()!=QProcess::NotRunning)return;
        QStringList args{statsWorker};
        const QString currentDirectory=property("knowledgeJobDirectory").toString();
        if(m_libraryJobBusy && !currentDirectory.isEmpty())args<<QStringLiteral("--db")<<QDir(currentDirectory).filePath(QStringLiteral("smart-space-knowledge.sqlite"));
        args<<QStringLiteral("stats")<<QStringLiteral("--details");
        process->start(QStandardPaths::findExecutable(QStringLiteral("python3")),args);refresh->setEnabled(false);
    };
    connect(process,QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished),&dialog,[&](int code,QProcess::ExitStatus){const auto value=QJsonDocument::fromJson(process->readAllStandardOutput()).object();if(code==0 && value.contains("documents")){snapshot=value;sampledAt=QDateTime::currentDateTime();statsMessage.clear();setProperty("knowledgeStatusLoaded",true);}else statsMessage=QStringLiteral("本次统计未完成：%1（已有数据为上次快照）").arg(value.value("error").toString());refresh->setEnabled(true);render();});
    connect(process,&QProcess::errorOccurred,&dialog,[&](QProcess::ProcessError e){if(e==QProcess::FailedToStart){statsMessage=QStringLiteral("统计工具无法启动，请检查安装");refresh->setEnabled(true);render();}});
    connect(failuresButton,&QPushButton::clicked,&dialog,[&]{
        if(m_libraryJobBusy)return;
        const QString db=snapshot.value("path").toString();if(db.isEmpty())return;
        KnowledgeFailuresDialog failures(db,statsWorker,light,m_ocrCpuPercent,m_ocrMemoryMiB,&dialog);
        setProperty("knowledgeFailuresVisible",true);const int action=failures.exec();setProperty("knowledgeFailuresVisible",false);
        if(action==KnowledgeFailuresDialog::Retry){const auto ids=failures.retryIds();const bool ocr=failures.includeOcr();dialog.accept();QTimer::singleShot(0,this,[this,db,ids,ocr]{startKnowledgeRetry(db,ids,ocr);});}
        else request();
    });
    QTimer timeout;timeout.setSingleShot(true);timeout.setInterval(15000);connect(process,&QProcess::started,&timeout,QOverload<>::of(&QTimer::start));connect(&timeout,&QTimer::timeout,process,&QProcess::kill);
    QTimer live;live.setInterval(1000);connect(&live,&QTimer::timeout,&dialog,render);live.start();
    QTimer polling;polling.setInterval(10000);connect(&polling,&QTimer::timeout,&dialog,[&]{if(m_libraryJobBusy)request();});polling.start();connect(refresh,&QPushButton::clicked,&dialog,request);
    setProperty("knowledgeStatusVisible",true);setProperty("knowledgeStatusLoaded",false);render();request();dialog.exec();
    live.stop();polling.stop();timeout.stop();process->disconnect(&dialog);
    if(process->state()!=QProcess::NotRunning){
        process->setParent(this);
        connect(process,QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished),process,&QObject::deleteLater);
        process->kill();
    }
    setProperty("knowledgeStatusVisible",false);
}

void SmartSpaceWidget::showKnowledgeManager()
{
    if(property("knowledgeFailuresVisible").toBool()){m_statusLabel->setText(QStringLiteral("请先关闭失败项处理窗口"));return;}
    if(property("knowledgeManagerVisible").toBool()){m_statusLabel->setText(QStringLiteral("知识库向导已打开"));return;}
    if (m_libraryJobBusy || (m_knowledgeProcess && m_knowledgeProcess->property("knowledgeTask").toString()!=QLatin1String("search"))) {
        smartInformation(this,QStringLiteral("知识库任务正在运行"),QStringLiteral("请先等待完成，或通过暂停按钮保存进度后再管理知识库。"));return;
    }
    KnowledgeManagerDialog dialog(m_libraryStorageDirectory,knowledgeDbPath(),m_roots,
        m_storageReserveGiB,m_libraryQuotaGiB,m_ocrCpuPercent,m_ocrMemoryMiB,m_themeMode==2,knowledgeWorkerPath(),this);
    setProperty("knowledgeManagerVisible",true);
    const int action=dialog.exec();
    setProperty("knowledgeManagerVisible",false);
    if(action==0 || action==KnowledgeManagerDialog::Save) {
        // A mount/version may have changed inside the manager. Refresh the
        // last committed query without executing text still being edited.
        m_entries.erase(std::remove_if(m_entries.begin(),m_entries.end(),[](const SmartSpaceEntry &entry){return entry.libraryOnly;}),m_entries.end());m_libraryOnlyEntryCount=0;
        m_knowledgeScores.clear();m_knowledgeExtractionStates.clear();m_knowledgeAvailability.clear();m_knowledgeSnippets.clear();
        if(!m_previewPath.isEmpty() && !entryForPath(m_previewPath))hidePreview();
        updateResults(m_scopePath.isEmpty());
        if(!m_executedQuery.isEmpty())startKnowledgeSearch(m_executedQuery);
    }
    if(!action)return;
    m_libraryStorageDirectory=dialog.directory();m_storageReserveGiB=dialog.reserveGiB();m_libraryQuotaGiB=dialog.quotaGiB();
    m_ocrCpuPercent=dialog.ocrCpu();m_ocrMemoryMiB=dialog.ocrMemory();m_storageConfirmed=true;
    if(action==KnowledgeManagerDialog::Build || action==KnowledgeManagerDialog::Ocr) m_roots=dialog.roots();
    saveSettings();
    if(action==KnowledgeManagerDialog::Build || action==KnowledgeManagerDialog::Ocr) {
        startGuidedBuild(dialog.includeOcr(),action==KnowledgeManagerDialog::Ocr);return;
    }
    QStringList arguments{QStringLiteral("--db"),knowledgeDbPath()};
    const QStringList storage{QStringLiteral("--directory"),m_libraryStorageDirectory,
        QStringLiteral("--reserve-gib"),QString::number(m_storageReserveGiB),QStringLiteral("--max-gib"),QString::number(m_libraryQuotaGiB)};
    if(action==KnowledgeManagerDialog::Import) {
        const QString file=QFileDialog::getOpenFileName(this,QStringLiteral("导入只读知识库"),m_libraryStorageDirectory,QStringLiteral("AI空间知识库 (*.aikb)"));
        if(file.isEmpty())return;
        arguments<<QStringLiteral("import")<<QStringLiteral("--source")<<file<<QStringLiteral("--mode")<<(dialog.mountOnly()?QStringLiteral("mount"):QStringLiteral("copy"));arguments.append(storage);
    } else if(action==KnowledgeManagerDialog::Export) {
        QString file=QFileDialog::getSaveFileName(this,QStringLiteral("导出已提取的知识（不含原件）"),QDir(m_libraryStorageDirectory).filePath(QStringLiteral("共享知识库.aikb")),QStringLiteral("AI空间知识库 (*.aikb)"));
        if(file.isEmpty())return;
        if(!file.endsWith(QLatin1String(".aikb"),Qt::CaseInsensitive))file+=QStringLiteral(".aikb");
        arguments<<QStringLiteral("export")<<QStringLiteral("--output")<<file<<QStringLiteral("--name")<<QFileInfo(file).completeBaseName();arguments.append(storage);
    } else return;
    if(m_knowledgeProcess) {m_knowledgeProcess->disconnect(this);terminateKnowledgeProcess(true);m_knowledgeProcess->deleteLater();m_knowledgeProcess=nullptr;}
    startKnowledgeTask(QStringLiteral("library"),arguments);
    m_statusLabel->setText(QStringLiteral("正在校验及处理知识库 · 大库可能需要较长时间，可暂停"));
}

void SmartSpaceWidget::startGuidedBuild(bool ocr,bool ocrOnly,const QString &singleStage,const QString &targetDirectory,bool providerApproved)
{
    if(!m_storageConfirmed){showKnowledgeManager();return;}
    if(property("knowledgeManagerVisible").toBool() || property("knowledgeFailuresVisible").toBool() || m_settingsViewVisible){m_statusLabel->setText(QStringLiteral("请先关闭向导、失败项处理或设置页，再启动知识库任务"));return;}
    if(m_providerAuditProcess){m_statusLabel->setText(QStringLiteral("正在检查数据源配置，请稍候"));return;}
    if(!providerApproved && !m_providerConfig.isEmpty() && (QFileInfo::exists(m_providerConfig) || m_providerConfig!=providerConfigPath())){
        auto *audit=new QProcess(this);m_providerAuditProcess=audit;
        auto output=std::make_shared<QByteArray>();
        connect(audit,&QProcess::readyReadStandardOutput,this,[audit,output]{output->append(audit->readAllStandardOutput());if(output->size()>262144)audit->kill();});
        connect(audit,QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished),this,[=](int code,QProcess::ExitStatus){
            if(m_providerAuditProcess==audit)m_providerAuditProcess=nullptr;audit->deleteLater();
            const auto report=QJsonDocument::fromJson(*output).object();
            if(code!=0 || report.value("digest").toString().isEmpty()){smartInformation(this,QStringLiteral("数据源配置不可用"),report.value("error").toString());return;}
            const QString digest=report.value("digest").toString();const auto capabilities=report.value("capabilities").toArray();
            if(!capabilities.isEmpty() && digest!=m_providerApprovedDigest){
                QStringList lines{QStringLiteral("以下数据源将执行程序或联系外部服务。请核对来源后再允许；配置、程序文件或解析路径变化后会再次确认。Command 程序以当前用户权限运行，授权不提供系统沙箱。")};
                for(const auto &value:capabilities){const auto row=value.toObject();lines<<row.value("type").toString()+QStringLiteral(" · ")+row.value("target").toString();if(row.value("sendRootPaths").toBool())lines<<QStringLiteral("此数据源会接收资料目录路径。");}
                if(smartQuestion(this,QStringLiteral("允许使用这些数据源？"),lines.join('\n'),QMessageBox::Yes|QMessageBox::No,QMessageBox::No)!=QMessageBox::Yes)return;
            }
            m_providerApprovedDigest=digest;saveSettings();startGuidedBuild(ocr,ocrOnly,singleStage,targetDirectory,true);
        });
        connect(audit,&QProcess::errorOccurred,this,[this,audit](QProcess::ProcessError error){if(error==QProcess::FailedToStart){if(m_providerAuditProcess==audit)m_providerAuditProcess=nullptr;audit->deleteLater();m_statusLabel->setText(QStringLiteral("无法检查数据源配置"));}});
        audit->start(QStandardPaths::findExecutable(QStringLiteral("python3")),{QFileInfo(knowledgeWorkerPath()).dir().filePath(QStringLiteral("provider_policy.py")),m_providerConfig});
        QTimer::singleShot(5000,audit,[audit]{if(audit->state()!=QProcess::NotRunning)audit->kill();});return;
    }

    if(m_libraryJobBusy){m_statusLabel->setText(QStringLiteral("知识库任务正在运行 · 请先暂停或等待完成"));return;}
    if(m_knowledgeProcess) {m_knowledgeProcess->disconnect(this);terminateKnowledgeProcess(true);m_knowledgeProcess->deleteLater();m_knowledgeProcess=nullptr;}
    const QString target=targetDirectory.isEmpty()?m_libraryStorageDirectory:targetDirectory;
    if((singleStage==QLatin1String("text") || ocrOnly) && (!QFileInfo::exists(QDir(target).filePath(QStringLiteral("catalog/index.json.ui.bin.gz"))) || (ocrOnly && !QFileInfo::exists(QDir(target).filePath(QStringLiteral("smart-space-knowledge.sqlite")))))){
        smartInformation(this,QStringLiteral("请先完成前置步骤"),ocrOnly?QStringLiteral("请先完成①全量扫描和②文本建库，再执行③OCR补全。"):QStringLiteral("请先完成①全量扫描，再执行②文本建库。"));return;
    }
    setProperty("knowledgeJobDirectory",target);
    QStringList args{QStringLiteral("--directory"),target,
        QStringLiteral("--previous-db"),knowledgeDbPath(),
        QStringLiteral("--chunk-size"),QString::number(m_knowledgeChunkSize),QStringLiteral("--overlap"),QString::number(m_knowledgeOverlap),
        QStringLiteral("--roots-json"),QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(m_roots)).toJson(QJsonDocument::Compact)),
        QStringLiteral("--exclude-roots-json"),QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(m_excludedFolders)).toJson(QJsonDocument::Compact)),
        QStringLiteral("--include-extensions-json"),QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(m_indexedExtensions)).toJson(QJsonDocument::Compact)),
        QStringLiteral("--reserve-gib"),QString::number(m_storageReserveGiB),QStringLiteral("--max-gib"),QString::number(m_libraryQuotaGiB),
        QStringLiteral("--ocr-cpu"),QString::number(m_ocrCpuPercent),QStringLiteral("--ocr-memory"),QString::number(m_ocrMemoryMiB)};
    if(!m_providerConfig.isEmpty() && QFileInfo::exists(m_providerConfig))args<<QStringLiteral("--provider-config")<<m_providerConfig<<QStringLiteral("--provider-approval")<<m_providerApprovedDigest;
    if(ocrOnly)args<<QStringLiteral("--ocr-only");else if(ocr)args<<QStringLiteral("--ocr");
    if(singleStage==QLatin1String("manifest"))args<<QStringLiteral("--manifest-only");
    if(singleStage==QLatin1String("text"))args<<QStringLiteral("--text-only");
    startKnowledgeTask(QStringLiteral("pipeline"),args);
}

void SmartSpaceWidget::startKnowledgeBuild(){showKnowledgeManager();}

void SmartSpaceWidget::startKnowledgeRetry(const QString &db,const QJsonArray &ids,bool ocr)
{
    if(ids.isEmpty() || m_libraryJobBusy || m_knowledgeProcess){m_statusLabel->setText(QStringLiteral("请等待当前任务完成后再重试"));return;}
    const QString directory=QFileInfo(db).absolutePath();setProperty("knowledgeJobDirectory",directory);setProperty("knowledgeRetryOcr",ocr);
    QStringList args{QStringLiteral("--directory"),directory,QStringLiteral("--retry-db"),db,QStringLiteral("--roots-json"),QStringLiteral("[]"),QStringLiteral("--retry-documents-json"),QString::fromUtf8(QJsonDocument(ids).toJson(QJsonDocument::Compact)),
        QStringLiteral("--chunk-size"),QString::number(m_knowledgeChunkSize),QStringLiteral("--overlap"),QString::number(m_knowledgeOverlap),QStringLiteral("--reserve-gib"),QString::number(m_storageReserveGiB),QStringLiteral("--max-gib"),QString::number(m_libraryQuotaGiB),QStringLiteral("--ocr-cpu"),QString::number(m_ocrCpuPercent),QStringLiteral("--ocr-memory"),QString::number(m_ocrMemoryMiB)};
    if(ocr)args<<QStringLiteral("--ocr");startKnowledgeTask(QStringLiteral("pipeline"),args);
}

void SmartSpaceWidget::startKnowledgeSearch(const QString &query)
{
    if(query!=m_executedQuery || m_searchUsesRegex)return;
    ++m_knowledgeSearchRevision;
    if(m_knowledgeSearchProcess){m_knowledgeSearchProcess->disconnect(this);m_knowledgeSearchProcess->kill();m_knowledgeSearchProcess->deleteLater();m_knowledgeSearchProcess=nullptr;}
    if(query.trimmed().isEmpty())return;
    const quint64 revision=m_knowledgeSearchRevision;
    auto *process=new QProcess(this);m_knowledgeSearchProcess=process;
    auto output=std::make_shared<QByteArray>();
    connect(process,&QProcess::readyReadStandardOutput,this,[process,output]{
        output->append(process->readAllStandardOutput());
        if(output->size()>4*1024*1024){process->setProperty("oversize",true);process->kill();output->clear();}
    });
    connect(process,QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished),this,[this,process,output,revision,query](int code,QProcess::ExitStatus){
        output->append(process->readAllStandardOutput());
        const auto data=QJsonDocument::fromJson(output->trimmed()).object();process->deleteLater();
        if(m_knowledgeSearchProcess!=process || revision!=m_knowledgeSearchRevision || query!=m_executedQuery || m_searchUsesRegex)return;
        m_knowledgeSearchProcess=nullptr;
        if(code!=0 || data.isEmpty() || !data.value(QStringLiteral("error")).toString().isEmpty()) {
            m_statusLabel->setText(process->property("oversize").toBool()?QStringLiteral("知识库返回内容过多，请使用更具体的关键词"):QStringLiteral("知识库查询未完成 · %1").arg(data.value(QStringLiteral("error")).toString().left(150)));return;
        }
        applyKnowledgeSearchResult(data);
    });
    connect(process,&QProcess::errorOccurred,this,[this,process,revision](QProcess::ProcessError error){
        if(error==QProcess::FailedToStart){if(revision==m_knowledgeSearchRevision)m_statusLabel->setText(QStringLiteral("无法启动知识库查询工具，请检查安装"));if(m_knowledgeSearchProcess==process)m_knowledgeSearchProcess=nullptr;process->deleteLater();}
    });
    const QStringList args{QFileInfo(knowledgeWorkerPath()).dir().filePath(QStringLiteral("knowledge_library.py")),QStringLiteral("--db"),knowledgeDbPath(),QStringLiteral("search"),
        QStringLiteral("--query"),query,QStringLiteral("--all"),QStringLiteral("--limit"),QStringLiteral("50"),QStringLiteral("--budget-ms"),QStringLiteral("2000")};
    process->start(QStandardPaths::findExecutable(QStringLiteral("python3")),args);
    QTimer::singleShot(3000,process,[process]{if(process->state()!=QProcess::NotRunning)process->kill();});
}

void SmartSpaceWidget::applyKnowledgeSearchResult(const QJsonObject &result)
{
    m_entries.erase(std::remove_if(m_entries.begin(),m_entries.end(),[](const SmartSpaceEntry &entry){return entry.libraryOnly;}),m_entries.end());
    m_libraryOnlyEntryCount=0;
    for(auto &entry:m_entries){entry.libraryReference.clear();entry.libraryDb.clear();}
    m_knowledgeScores.clear();m_knowledgeExtractionStates.clear();m_knowledgeAvailability.clear();m_knowledgeSnippets.clear();
    const QJsonArray rows = result.value(QStringLiteral("results")).toArray();
    for (const QJsonValue &value : rows) {
        const QJsonObject row = value.toObject();
        const QString path = row.value(QStringLiteral("path")).toString();
        if (path.isEmpty())
            continue;
        QString resultPath=path;
        const QString libraryRef=row.value(QStringLiteral("reference")).toString();
        const QString sourceDb=row.value(QStringLiteral("source_db")).toString();
        if(!libraryRef.isEmpty() && (sourceDb!=knowledgeDbPath() || !entryForPath(path))) {
            resultPath=libraryRef;
            if(!entryForPath(resultPath)) {
                SmartSpaceEntry entry;entry.libraryOnly=true;entry.path=resultPath;entry.libraryReference=libraryRef;entry.libraryDb=sourceDb;
                entry.libraryName=row.value(QStringLiteral("library_name")).toString();entry.originalPath=path;
                entry.libraryDocumentId=row.value(QStringLiteral("document_id")).toVariant().toLongLong();entry.libraryChunkId=row.value(QStringLiteral("chunk_id")).toVariant().toLongLong();
                entry.name=row.value(QStringLiteral("name")).toString();entry.category=row.value(QStringLiteral("category")).toString();entry.suffix=QFileInfo(entry.name).suffix().toLower();
                entry.root=QStringLiteral("aispace://")+row.value(QStringLiteral("library_id")).toString();entry.parentPath=entry.root;
                entry.content=row.value(QStringLiteral("snippet")).toString();entry.size=row.value(QStringLiteral("size")).toVariant().toLongLong();entry.modified=QDateTime::fromMSecsSinceEpoch(row.value(QStringLiteral("modifiedMs")).toVariant().toLongLong());
                m_entries.append(entry);++m_libraryOnlyEntryCount;
            }
        }
        if(resultPath==path && !libraryRef.isEmpty()) {
            for(auto &entry:m_entries)if(entry.path==path) {
                entry.libraryReference=libraryRef;entry.libraryDb=sourceDb;entry.libraryName=row.value(QStringLiteral("library_name")).toString();entry.originalPath=path;
                entry.libraryDocumentId=row.value(QStringLiteral("document_id")).toVariant().toLongLong();entry.libraryChunkId=row.value(QStringLiteral("chunk_id")).toVariant().toLongLong();break;
            }
        }
        m_knowledgeExtractionStates.insert(resultPath,row.value(QStringLiteral("extractionStatus")).toString());
        m_knowledgeAvailability.insert(resultPath,row.value(QStringLiteral("sourceAvailability")).toString());
        m_knowledgeScores.insert(
            resultPath, row.value(QStringLiteral("score")).toInt());
        m_knowledgeSnippets.insert(
            resultPath, row.value(QStringLiteral("snippet")).toString());
    }
    if(!m_previewPath.isEmpty() && !entryForPath(m_previewPath))hidePreview();
    updateResults(m_scopePath.isEmpty());
    const QJsonArray coverage=result.value(QStringLiteral("libraries")).toArray();int unavailable=0;
    for(const auto &v:coverage)if(v.toObject().value(QStringLiteral("status")).toString()==QLatin1String("unavailable") || v.toObject().value(QStringLiteral("status")).toString()==QLatin1String("error"))++unavailable;
    m_statusLabel->setText(QStringLiteral("知识库召回 %1 项%2%3").arg(m_knowledgeScores.size())
        .arg(result.value(QStringLiteral("timed_out")).toBool()?QStringLiteral(" · 已达时间预算，可缩小关键词后重试"):QString())
        .arg(unavailable?QStringLiteral(" · %1 个库不可用").arg(unavailable):result.value(QStringLiteral("incomplete")).toBool()?QStringLiteral(" · 查询覆盖不完整，部分文字可能超出旧索引的语言范围"):QString()));
}

void SmartSpaceWidget::handleKnowledgeOutput()
{
    if (!m_knowledgeProcess)
        return;
    m_knowledgeOutputBuffer += m_knowledgeProcess->readAllStandardOutput();
    const QByteArray errors = m_knowledgeProcess->readAllStandardError();
    if (!errors.isEmpty()) {
        const int room = qMax(
            0, kKnowledgeErrorLimit - m_knowledgeErrorBuffer.size());
        if (room > 0)
            m_knowledgeErrorBuffer += errors.left(room);
    }
    if (m_knowledgeOutputBuffer.size() > 4 * 1024 * 1024)
        m_knowledgeOutputBuffer =
            m_knowledgeOutputBuffer.right(2 * 1024 * 1024);
    int newline = -1;
    while ((newline = m_knowledgeOutputBuffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_knowledgeOutputBuffer.left(newline).trimmed();
        m_knowledgeOutputBuffer.remove(0, newline + 1);
        QJsonParseError error;
        const QJsonObject object =
            QJsonDocument::fromJson(line, &error).object();
        if (error.error != QJsonParseError::NoError || object.isEmpty())
            continue;
        m_knowledgeLastObject = object;
        const QString task = object.value(QStringLiteral("task")).toString();
        if(task==QLatin1String("pipeline") && !object.value(QStringLiteral("done")).toBool()) {
            if(object.value(QStringLiteral("phase")).toString()==QLatin1String("ready")) {const QString db=object.value(QStringLiteral("db")).toString();if(!db.isEmpty())m_knowledgeDirectory=QFileInfo(db).absolutePath();saveSettings();loadIndexAsync();}
            const QString stage=object.value(QStringLiteral("stage")).toString();
            if(object.value(QStringLiteral("phase")).toString()==QLatin1String("start")){setProperty("knowledgeStage",stage);m_indexProgress->hide();}
            const QString text=stage==QLatin1String("manifest")?QStringLiteral("1/3 全量文件清单（不提取正文）"):stage==QLatin1String("text")?QStringLiteral("2/3 文本正文提取（不运行 OCR）"):stage==QLatin1String("retry")?QStringLiteral("正在重新处理选中文件（沿用资源限制，可暂停）"):QStringLiteral("3/3 受限 OCR 补全（可暂停）");
            m_statusLabel->setText(text);
        }
        if(m_libraryJobBusy && !object.value(QStringLiteral("done")).toBool()) {
            qreal completed=0,total=0;QString format;
            if(task==QLatin1String("build")){completed=object.value(QStringLiteral("processed")).toDouble();total=object.value(QStringLiteral("total")).toDouble();format=property("knowledgeStage").toString()==QLatin1String("ocr")?QStringLiteral("OCR %p%"):QStringLiteral("正文 %p%");}
            else if(task==QLatin1String("import")){completed=object.value(QStringLiteral("bytes")).toDouble();total=object.value(QStringLiteral("totalBytes")).toDouble();format=QStringLiteral("导入校验 %p%");}
            else if(task==QLatin1String("export")){completed=object.value(QStringLiteral("completedPages")).toDouble();total=object.value(QStringLiteral("totalPages")).toDouble();format=QStringLiteral("导出 %p%");}
            else if(task.isEmpty() && (object.contains(QStringLiteral("processed")) || object.contains(QStringLiteral("progress")))){completed=object.value(object.contains(QStringLiteral("processed"))?QStringLiteral("processed"):QStringLiteral("progress")).toDouble();total=object.value(QStringLiteral("total")).toDouble();format=QStringLiteral("文件清单 %p%");}
            if(total>0){m_indexProgress->setRange(0,100);m_indexProgress->setFormat(format);m_indexProgress->setValue(qBound(0,int(completed*100.0/total),100));m_indexProgress->show();}
        }
        if (task == QLatin1String("build") && !object.value(
                QStringLiteral("done")).toBool()) {
            m_statusLabel->setText(QStringLiteral(
                "%5 %1/%2 · 文档 %3 · 片段 %4")
                .arg(object.value(QStringLiteral("processed")).toInt())
                .arg(object.value(QStringLiteral("total")).toInt())
                .arg(object.value(QStringLiteral("documents")).toInt())
                .arg(object.value(QStringLiteral("chunks")).toInt()).arg(property("knowledgeStage").toString()==QLatin1String("retry")?QStringLiteral("失败项重试"):QStringLiteral("知识库建库")));
        }
    }
}

void SmartSpaceWidget::finishKnowledgeTask(int exitCode, int exitStatus)
{
    Q_UNUSED(exitStatus)
    if (!m_knowledgeProcess)
        return;
    handleKnowledgeOutput();
    const QString task =
        m_knowledgeProcess->property("knowledgeTask").toString();
    const bool userPaused =
        m_knowledgeProcess->property("userPaused").toBool();
    const bool timedOut =
        m_knowledgeProcess->property("timedOut").toBool();
    m_knowledgeProcess->deleteLater();
    m_knowledgeProcess = nullptr;
    m_knowledgeUsesProcessGroup = false;
    if(m_libraryJobBusy){m_libraryJobBusy=false;m_indexProgress->hide();m_idleIndexButton->setEnabled(true);m_refreshButton->setEnabled(true);if(m_textIndexButton)m_textIndexButton->setEnabled(true);m_ocrIndexButton->setEnabled(true);m_pauseIndexButton->setEnabled(false);}
    const QJsonObject result = m_knowledgeLastObject;
    const QString error = result.value(QStringLiteral("error")).toString();
    const QString processError = QString::fromUtf8(
        m_knowledgeErrorBuffer).trimmed().left(500);
    setProperty("knowledgeJobOutcome",userPaused?QStringLiteral("已暂停"):exitCode==0?QStringLiteral("已完成"):QStringLiteral("失败 / 已停止"));
    if (userPaused) {
        m_statusLabel->setText(QStringLiteral(
            "知识库任务已暂停 · 已完成内容已保存，下次自动续跑"));
        return;
    }
    if (timedOut) {
        m_statusLabel->setText(QStringLiteral(
            "知识库任务超时 · 已安全结束，可稍后重试"));
        return;
    }
    if (exitCode != 0 || exitStatus != QProcess::NormalExit ||
        !error.isEmpty()) {
        const QString detail = !error.isEmpty()
            ? error
            : !processError.isEmpty()
                ? processError
                : QStringLiteral("退出码 %1").arg(exitCode);
        m_statusLabel->setText(QStringLiteral("知识库任务失败 · %1")
            .arg(detail));
        return;
    }
    if(task==QLatin1String("pipeline")) {
        if(result.value("phase").toString()==QLatin1String("retry-complete")){
            m_statusLabel->setText(QStringLiteral("重试完成 · %1 项 · %2 项仍失败 / 缺失 · 点击查看详情").arg(result.value("retried").toInt()).arg(result.value("retryFailures").toInt()));if(!m_executedQuery.isEmpty())startKnowledgeSearch(m_executedQuery);return;
        }
        if(m_reindexPending && m_indexMode==1){m_reindexPending=false;QTimer::singleShot(2000,this,&SmartSpaceWidget::startIndexing);}
        if(result.value(QStringLiteral("phase")).toString()==QLatin1String("manifest-complete")){m_statusLabel->setText(QStringLiteral("① 扫描完成 · 请继续②文本建库"));return;}
        const QString completedDb=result.value(QStringLiteral("db")).toString();
        if(!completedDb.isEmpty())m_knowledgeDirectory=QFileInfo(completedDb).absolutePath();
        saveSettings();loadIndexAsync();
        const auto states=result.value(QStringLiteral("extractionStates")).toObject();
        m_statusLabel->setText(QStringLiteral("建库完成 · %1 文档 · %2 部分提取 · %3 失败 · 点击查看详情").arg(result.value(QStringLiteral("documents")).toInt()).arg(states.value(QStringLiteral("partial")).toInt()).arg(states.value(QStringLiteral("failed")).toInt()));return;
    }
    if(task==QLatin1String("library")) {
        m_statusLabel->setText(QStringLiteral("知识库导入 / 导出完成 · 外来库只读挂载，无需重新扫描"));
        if(!m_executedQuery.isEmpty())startKnowledgeSearch(m_executedQuery);return;
    }
    if (task == QLatin1String("build")) {
        m_statusLabel->setText(QStringLiteral(
            "知识库建库完成 · %1 个文档 · %2 个片段 · %3 MB")
            .arg(result.value(QStringLiteral("documents")).toInt())
            .arg(result.value(QStringLiteral("chunks")).toInt())
            .arg(result.value(QStringLiteral("dbBytes")).toDouble()
                 / 1024.0 / 1024.0, 0, 'f', 1));
    }
    else if (task == QLatin1String("stats")) {
        m_statusLabel->setText(QStringLiteral(
            "知识库：%1 文档 / %2 片段 / %3 MB")
            .arg(result.value(QStringLiteral("documents")).toInt())
            .arg(result.value(QStringLiteral("chunks")).toInt())
            .arg(result.value(QStringLiteral("dbBytes")).toDouble()
                 / 1024.0 / 1024.0, 0, 'f', 1));
    }
}

QStringList SmartSpaceWidget::defaultRoots() const
{
    QStringList roots;
    // The user's Documents tree can contain hundreds of thousands of files.
    // Start with the small desktop tree and let the user opt into precise
    // document folders from Settings.
    const QString path = QStandardPaths::writableLocation(
        QStandardPaths::DesktopLocation);
    if (!path.isEmpty() && QDir(path).exists())
        roots << path;
    return roots;
}

void SmartSpaceWidget::startIndexing()
{
    if(!m_storageConfirmed){if(qobject_cast<QTimer *>(sender()))return;showKnowledgeManager();return;}
    if(property("knowledgeManagerVisible").toBool() || property("knowledgeFailuresVisible").toBool() || m_settingsViewVisible){if(!qobject_cast<QTimer *>(sender()))m_statusLabel->setText(QStringLiteral("请先关闭向导、失败项处理或设置页再启动增量更新"));return;}
    const QString current=QFileInfo(knowledgeDbPath()).absolutePath();
    startGuidedBuild(false,false,QString(),QFileInfo::exists(knowledgeDbPath())?current:m_libraryStorageDirectory);
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

void SmartSpaceWidget::startIdleFullIndex(){if(!m_storageConfirmed)showKnowledgeManager();else startGuidedBuild(false);}
void SmartSpaceWidget::startOcrIndex(){if(!m_storageConfirmed)showKnowledgeManager();else startGuidedBuild(false,true);}

void SmartSpaceWidget::stopIdleFullIndex()
{
    if(!m_knowledgeProcess)return;
    m_knowledgeProcess->setProperty("userPaused",true);terminateKnowledgeProcess(false);
}

SmartSpaceWidget::IndexSnapshot SmartSpaceWidget::readIndexSnapshot(
    const QString &canonicalPath, const QString &streamFilename, const QString &ocrPath,
    const QString &resumePath, const QStringList &excludedFolders, int maxItems)
{
    IndexSnapshot snapshot;
    auto pathExcluded = [&excludedFolders](const QString &path) {
        for (const QString &folder : excludedFolders)
            if (pathIsInside(path, folder)) return true;
        return false;
    };
    QJsonObject root;
    QVector<SmartSpaceEntry> loaded;
    auto appendEntry = [&loaded, &pathExcluded](const QJsonObject &object) {
        SmartSpaceEntry entry;
        entry.path = normalizedPath(object.value(QStringLiteral("path")).toString());
        entry.root = normalizedPath(object.value(QStringLiteral("root")).toString());
        entry.parentPath = normalizedPath(object.value(QStringLiteral("parentPath")).toString());
        if (entry.parentPath.isEmpty())
            entry.parentPath = normalizedPath(QFileInfo(entry.path).absolutePath());
        entry.name = object.value(QStringLiteral("name")).toString();
        if (entry.name.isEmpty())
            entry.name = QFileInfo(entry.path).fileName();
        entry.suffix = object.value(QStringLiteral("suffix")).toString();
        if (entry.suffix.isEmpty() && !object.value(QStringLiteral("isDir")).toBool())
            entry.suffix = QFileInfo(entry.path).suffix().toLower();
        entry.category = object.value(QStringLiteral("category")).toString();
        entry.content = object.value(QStringLiteral("content")).toString();
        entry.extractor = object.value(QStringLiteral("extractor")).toString();
        entry.ocrStatus = object.value(QStringLiteral("ocrStatus")).toString();
        entry.ocrReason = object.value(QStringLiteral("ocrReason")).toString();
        entry.isDir = object.value(QStringLiteral("isDir")).toBool();
        entry.size = static_cast<qint64>(object.value(QStringLiteral("size")).toDouble());
        entry.modified = QDateTime::fromString(
            object.value(QStringLiteral("modified")).toString(), Qt::ISODateWithMs);
        if (!entry.path.isEmpty() && !pathExcluded(entry.path))
            loaded.append(entry);
    };

    bool loadedFromStream = false;
    QString streamFailure;
    const QByteArray streamPath = QFile::encodeName(streamFilename);
    const QFileInfo streamInfo(streamFilename);
    const QFileInfo canonicalInfo(canonicalPath);
    const bool streamIsCurrent = streamInfo.exists() &&
        (!canonicalInfo.exists() ||
         streamInfo.lastModified() >= canonicalInfo.lastModified());
    if (streamInfo.exists() && !streamIsCurrent)
        streamFailure = QStringLiteral("流式索引旧于主索引");
    if (streamIsCurrent) {
        gzFile archive = gzopen(streamPath.constData(), "rb");
        if (!archive) {
            streamFailure = QStringLiteral("无法打开流式索引");
        } else {
            QByteArray decompressedBuffer(256 * 1024, '\0');
            int bufferOffset = 0;
            int bufferSize = 0;
            auto readExact = [archive, &decompressedBuffer, &bufferOffset,
                              &bufferSize](void *destination, int bytes) {
                char *cursor = static_cast<char *>(destination);
                int remaining = bytes;
                while (remaining > 0) {
                    if (bufferOffset >= bufferSize) {
                        bufferSize = gzread(archive, decompressedBuffer.data(),
                                            decompressedBuffer.size());
                        bufferOffset = 0;
                        if (bufferSize <= 0)
                            return false;
                    }
                    const int count = qMin(remaining,
                                           bufferSize - bufferOffset);
                    std::memcpy(cursor,
                                decompressedBuffer.constData() + bufferOffset,
                                static_cast<size_t>(count));
                    cursor += count;
                    bufferOffset += count;
                    remaining -= count;
                }
                return true;
            };
            auto readU32 = [&readExact](quint32 &value) {
                quint32 encoded = 0;
                if (!readExact(&encoded, sizeof(encoded)))
                    return false;
                value = qFromLittleEndian(encoded);
                return true;
            };
            auto readI64 = [&readExact](qint64 &value) {
                qint64 encoded = 0;
                if (!readExact(&encoded, sizeof(encoded)))
                    return false;
                value = qFromLittleEndian(encoded);
                return true;
            };
            auto readString = [&readExact, &readU32](QString &value) {
                quint32 length = 0;
                if (!readU32(length) || length > 64U * 1024U * 1024U)
                    return false;
                QByteArray encoded;
                encoded.resize(static_cast<int>(length));
                if (length > 0 && !readExact(encoded.data(), encoded.size()))
                    return false;
                value = QString::fromUtf8(encoded);
                return true;
            };

            char magic[8] = {};
            bool streamOk = readExact(magic, sizeof(magic)) &&
                std::memcmp(magic, "UKFIDX1\n", sizeof(magic)) == 0;
            quint32 streamVersion = 0;
            quint32 metadataLength = 0;
            streamOk = streamOk && readU32(streamVersion) &&
                streamVersion == 1 && readU32(metadataLength) &&
                metadataLength <= 4U * 1024U * 1024U;
            QByteArray metadata;
            if (streamOk) {
                metadata.resize(static_cast<int>(metadataLength));
                streamOk = metadataLength == 0 ||
                    readExact(metadata.data(), metadata.size());
            }
            int expectedItems = -1;
            int parsedItems = 0;
            if (streamOk) {
                QJsonParseError metaError;
                const QJsonDocument metaDocument = QJsonDocument::fromJson(
                    metadata, &metaError);
                root = metaDocument.object();
                quint32 encodedItems = 0;
                streamOk = readU32(encodedItems);
                expectedItems = static_cast<int>(encodedItems);
                streamOk = metaError.error == QJsonParseError::NoError &&
                    metaDocument.isObject() && !root.isEmpty() && streamOk &&
                    expectedItems >= 0 &&
                    root.value(QStringLiteral("uiStreamItems")).toInt(-1) ==
                        expectedItems;
                if (!streamOk)
                    streamFailure = QStringLiteral("流式索引元数据无效");
            }
            if (streamOk)
                loaded.reserve(expectedItems);
            while (streamOk && parsedItems < expectedItems) {
                quint8 flags = 0;
                qint64 size = 0;
                qint64 modifiedMs = -1;
                SmartSpaceEntry entry;
                streamOk = readExact(&flags, sizeof(flags)) &&
                    readI64(size) && readI64(modifiedMs) &&
                    readString(entry.path) && readString(entry.root) &&
                    readString(entry.name) && readString(entry.category) &&
                    readString(entry.content) && readString(entry.extractor) &&
                    readString(entry.ocrStatus) && readString(entry.ocrReason);
                if (!streamOk) {
                    streamOk = false;
                    streamFailure = QStringLiteral("流式索引条目无效");
                    break;
                }
                ++parsedItems;
                entry.path = normalizedPath(entry.path);
                entry.root = normalizedPath(entry.root);
                entry.parentPath = normalizedPath(
                    QFileInfo(entry.path).absolutePath());
                if (entry.name.isEmpty())
                    entry.name = QFileInfo(entry.path).fileName();
                entry.isDir = (flags & 1U) != 0;
                if (!entry.isDir)
                    entry.suffix = QFileInfo(entry.path).suffix().toLower();
                entry.size = size;
                if (modifiedMs >= 0)
                    entry.modified = QDateTime::fromMSecsSinceEpoch(
                        modifiedMs, Qt::UTC);
                if (!entry.path.isEmpty() && !pathExcluded(entry.path))
                    loaded.append(entry);
            }
            int zlibError = Z_OK;
            gzerror(archive, &zlibError);
            if (zlibError != Z_OK && zlibError != Z_STREAM_END) {
                streamOk = false;
                streamFailure = QStringLiteral("流式索引压缩数据损坏");
            }
            if (parsedItems != expectedItems) {
                streamOk = false;
                streamFailure = QStringLiteral("流式索引条目数不完整");
            }
            if (gzclose(archive) != Z_OK) {
                streamOk = false;
                streamFailure = QStringLiteral("流式索引解压失败");
            }
            loadedFromStream = streamOk;
            if (!streamOk) {
                root = QJsonObject();
                loaded.clear();
            }
        }
    }

    if (!loadedFromStream) {
        QFile file(canonicalPath);
        if (!file.open(QIODevice::ReadOnly)) {
            snapshot.error = QStringLiteral("无法读取索引文件，请检查文件权限");
            return snapshot;
        }
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            const QString detail = streamFailure.isEmpty()
                ? error.errorString()
                : QStringLiteral("%1；%2").arg(error.errorString(), streamFailure);
            snapshot.error = QStringLiteral("索引加载失败（%1）· 索引文件保持不变").arg(detail);
            return snapshot;
        }
        root = document.object();
        const QJsonArray items = root.value(QStringLiteral("items")).toArray();
        loaded.reserve(items.size());
        for (const QJsonValue &value : items)
            appendEntry(value.toObject());
    }

    const QString generated = root.value(QStringLiteral("generatedAt")).toString();
    const int errorCount = root.value(QStringLiteral("errors")).toArray().size();
    const QJsonObject stats = root.value(QStringLiteral("stats")).toObject();
    const QJsonObject capabilities = root.value(
        QStringLiteral("capabilities")).toObject();
    const int reused = stats.value(QStringLiteral("reused")).toInt();
    int pendingOcr = stats.value(QStringLiteral("ocrPending")).toInt(-1);
    // The published fast-full snapshot keeps the original OCR candidate count.
    // While an OCR backfill is paused, the append-only resume journal contains
    // the newer per-file states, so use its latest state for the status bar.
    QFile ocrResume(ocrPath);
    if (ocrResume.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QHash<QString, bool> pendingByPath;
        bool resumeMatchesSnapshot = false;
        while (!ocrResume.atEnd()) {
            const QByteArray line = ocrResume.readLine().trimmed();
            if (line.isEmpty())
                continue;
            const QJsonDocument checkpoint = QJsonDocument::fromJson(line);
            if (!checkpoint.isObject())
                continue;
            const QJsonObject record = checkpoint.object();
            if (record.value(QStringLiteral("kind")).toString() ==
                QLatin1String("header")) {
                const QJsonObject signature = record.value(
                    QStringLiteral("signature")).toObject();
                resumeMatchesSnapshot = signature.value(
                    QStringLiteral("snapshotGeneratedAt")).toString() ==
                    root.value(QStringLiteral("generatedAt")).toString();
                if (!resumeMatchesSnapshot)
                    break;
                continue;
            }
            if (!resumeMatchesSnapshot)
                continue;
            const QJsonObject item = record.value(QStringLiteral("item")).toObject();
            const QString path = item.value(QStringLiteral("path")).toString();
            if (path.isEmpty())
                continue;
            pendingByPath.insert(
                path, item.value(QStringLiteral("ocrStatus")).toString() ==
                          QLatin1String("pending"));
        }
        if (resumeMatchesSnapshot && !pendingByPath.isEmpty())
            pendingOcr = std::count(pendingByPath.cbegin(),
                                    pendingByPath.cend(), true);
    }
    if (pendingOcr < 0) {
        pendingOcr = 0;
        for (const SmartSpaceEntry &entry : loaded) {
            if (entry.ocrStatus == QLatin1String("pending"))
                ++pendingOcr;
        }
    }
    const bool truncated = stats.value(QStringLiteral("truncated")).toInt() != 0;
    const bool fullRebuild = root.value(QStringLiteral("fullRebuild")).toBool();
    const QString storedIndexMode = root.value(
        QStringLiteral("indexMode")).toString();
    const int snapshotLimit = capabilities.value(
        QStringLiteral("maxItems")).toInt();
    const bool fullResumePending = !fullRebuild && QFileInfo::exists(
        resumePath);
    const QDateTime generatedTime = QDateTime::fromString(generated, Qt::ISODateWithMs);
    const QString displayTime = generatedTime.isValid()
        ? generatedTime.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
        : generated.left(19).replace(QLatin1Char('T'), QLatin1Char(' '));
    const QString snapshotKind = fullRebuild
        ? (storedIndexMode == QLatin1String("fast-full")
            ? QStringLiteral("快速全量已同步")
            : QStringLiteral("全量索引已同步"))
        : (fullResumePending
            ? QStringLiteral("全量待续扫 · 当前展示上次日常索引")
            : QStringLiteral("日常索引已同步"));
    const QString limitState = fullRebuild
        ? QStringLiteral(" · 全量无文件数上限")
        : (truncated
            ? QStringLiteral(" · 已达日常上限 %1").arg(
                  snapshotLimit > 0 ? snapshotLimit : maxItems)
            : QString());
    const QString ocrState = pendingOcr > 0
        ? QStringLiteral(" · 待检测/OCR %1 项").arg(pendingOcr)
        : QString();
    snapshot.statusPrefix = snapshotKind + QStringLiteral(" · ");
    snapshot.statusSuffix = QStringLiteral(" 项%1%2%3%4 · %5")
        .arg(errorCount ? QStringLiteral(" · %1 项跳过").arg(errorCount) : QString())
        .arg(reused ? QStringLiteral(" · 复用 %1 项").arg(reused) : QString())
        .arg(limitState).arg(ocrState).arg(displayTime);
    snapshot.entries = std::move(loaded);
    snapshot.valid = true;
    return snapshot;
}

bool SmartSpaceWidget::applyIndexSnapshot(IndexSnapshot snapshot)
{
    QElapsedTimer elapsed; elapsed.start();
    setProperty("indexLoading", false);
    if (!snapshot.valid) {
        if (!indexBusy()) m_statusLabel->setText(snapshot.error);
        return false;
    }
    // Exclusions may have changed while the initial snapshot was being read.
    auto &entries = snapshot.entries;
    entries.erase(std::remove_if(entries.begin(), entries.end(),
        [this](const SmartSpaceEntry &entry) { return pathExcluded(entry.path); }), entries.end());
    m_entries = std::move(entries);m_libraryOnlyEntryCount=0;
    updateResults(true);
    rebuildWatches();
    if (!indexBusy())
        m_statusLabel->setText(snapshot.statusPrefix + QString::number(m_entries.size()) + snapshot.statusSuffix);
    if (qEnvironmentVariableIsSet("UKUI_FENCES_STARTUP_TRACE"))
        qInfo() << "[AiSpaceStartup] index-applied" << elapsed.elapsed() << "ms";
    setProperty("indexApplyMs",elapsed.elapsed());
    if(!m_executedQuery.isEmpty() && !m_searchUsesRegex)startKnowledgeSearch(m_executedQuery);
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
        ocrResumeIndexPath(), resumeIndexPath(), m_excludedFolders, m_maxItems));
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
    BackgroundTask::run(this, [=] {
        QElapsedTimer elapsed; elapsed.start();
        // Apply the current exclusions on delivery, including folders restored
        // while I/O was pending. Do not freeze the constructor's scope here.
        auto snapshot = readIndexSnapshot(canonical, stream, ocr, resume, {}, maxItems);
        if (qEnvironmentVariableIsSet("UKUI_FENCES_STARTUP_TRACE"))
            qInfo() << "[AiSpaceStartup] index-read" << elapsed.elapsed() << "ms" << snapshot.entries.size() << "entries";
        return snapshot;
    }, [this, revision](const IndexSnapshot &snapshot) {
        if (revision == m_indexLoadRevision) applyIndexSnapshot(snapshot);
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
    if(m_indexMode!=1)return;

    QStringList paths;
    QSet<QString> seenPaths;
    for (const QString &root : m_roots) {
        if (QDir(root).exists() && !pathExcluded(root) &&
            !seenPaths.contains(root)) {
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
    setProperty("knowledgeMigrationPending",pending);
    auto *label=findChild<QLabel *>("knowledgeMigrationStatus");
    if(!label && pending) {
        label=new QLabel(QStringLiteral("旧知识库正在后台导入 · 文件检索可正常使用"),m_contentContainer);
        label->setObjectName("knowledgeMigrationStatus");label->setWordWrap(true);
        m_contentContainer->layout()->addWidget(label);
    }
    if(label)label->setVisible(pending);
}

void SmartSpaceWidget::scheduleReindex()
{
    if (m_indexMode != 1 || !m_automaticIndexingReady)
        return;
    if (m_libraryJobBusy || property("knowledgeManagerVisible").toBool() || m_settingsViewVisible) {
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
    m_pinButton->setText(m_alwaysOnTop
        ? QStringLiteral("已置顶") : QStringLiteral("置顶"));
    m_pinButton->setToolTip(m_alwaysOnTop
        ? QStringLiteral("取消置顶，作为普通应用窗口显示")
        : QStringLiteral("让AI空间悬浮在所有应用上方"));
}

bool SmartSpaceWidget::entryMatches(const SmartSpaceEntry &entry) const
{
    if (pathExcluded(entry.path) || (!entry.isDir && !extensionVisible(entry)))
        return false;
    if (!m_executedQuery.isEmpty()) {
        if (m_searchUsesRegex) {
            const bool matches = m_searchExpression.match(entry.name).hasMatch()
                || m_searchExpression.match(entry.path).hasMatch()
                || m_searchExpression.match(entry.content).hasMatch();
            if (!matches)
                return false;
        } else {
            for (const QString &term : m_searchTerms) {
                const bool matches = entry.name.contains(term, Qt::CaseInsensitive)
                    || entry.path.contains(term, Qt::CaseInsensitive)
                    || entry.content.contains(term, Qt::CaseInsensitive);
                if (!matches)
                    return false;
            }
        }
    }

    if (!categoryMatches(entry))
        return false;
    return true;
}

bool SmartSpaceWidget::fileMatchesCurrentResults(
    const SmartSpaceEntry &entry, bool respectScope) const
{
    if (entry.isDir)
        return false;
    const bool knowledgeMatch =
        !m_executedQuery.isEmpty() &&
        m_knowledgeScores.contains(entry.path);
    const bool visibleKnowledgeMatch = knowledgeMatch &&
        !pathExcluded(entry.path) && extensionVisible(entry) &&
        categoryMatches(entry);
    if (!entryMatches(entry) && !visibleKnowledgeMatch)
        return false;
    if (respectScope && !pathInsideScope(entry.path))
        return false;
    return true;
}

bool SmartSpaceWidget::commitSearchQuery(const QString &query, bool showError)
{
    const QString normalized = query.trimmed();
    const bool regexMode = normalized.startsWith(
        QStringLiteral("re:"), Qt::CaseInsensitive);
    QRegularExpression expression;
    QStringList terms;
    if (regexMode) {
        const QString pattern = normalized.mid(3).trimmed();
        expression = QRegularExpression(
            pattern, QRegularExpression::CaseInsensitiveOption |
                     QRegularExpression::UseUnicodePropertiesOption);
        if (pattern.isEmpty() || !expression.isValid()) {
            if (showError && m_statusLabel) {
                m_statusLabel->setText(pattern.isEmpty()
                    ? QStringLiteral("正则表达式为空，请在 re: 后输入规则")
                    : QStringLiteral("正则表达式无效：%1")
                        .arg(expression.errorString()));
            }
            return false;
        }
    } else if (!normalized.isEmpty()) {
        terms = normalized.split(QRegularExpression(QStringLiteral("\\s+")),
                                 QString::SkipEmptyParts);
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
    if(m_knowledgeSearchProcess){m_knowledgeSearchProcess->disconnect(this);m_knowledgeSearchProcess->kill();m_knowledgeSearchProcess->deleteLater();m_knowledgeSearchProcess=nullptr;}
    hidePreview();
    m_entries.erase(std::remove_if(m_entries.begin(),m_entries.end(),[](const SmartSpaceEntry &entry){return entry.libraryOnly;}),m_entries.end());
    m_libraryOnlyEntryCount=0;
    for(auto &entry:m_entries){entry.libraryReference.clear();entry.libraryDb.clear();}
    m_knowledgeScores.clear();m_knowledgeExtractionStates.clear();m_knowledgeAvailability.clear();
    m_knowledgeSnippets.clear();
    updateResults(true);
    if (!m_searchUsesRegex && !m_executedQuery.isEmpty())
        startKnowledgeSearch(m_executedQuery);
}

bool SmartSpaceWidget::categoryMatches(const SmartSpaceEntry &entry) const
{
    return m_selectedCategories.isEmpty() ||
        m_selectedCategories.contains(entry.category);
}

QString SmartSpaceWidget::entryToolTip(const SmartSpaceEntry &entry) const
{
    const QString body = relevantSnippetHtml(entry, 3, 42, 86);
    const QString ocr = entry.ocrStatus == QLatin1String("pending")
        ? QStringLiteral("<br><span style='color:#d97706'><b>待检测/OCR</b> · "
                         "会先检测 PDF 文本层，必要时才运行 OCR</span>")
        : QString();
    return QStringLiteral(
        "<qt><div style='min-width:320px;max-width:460px'>"
        "<b>%1</b>%2<br><br>%3</div></qt>")
        .arg(entry.name.toHtmlEscaped(), ocr, body);
}

QString SmartSpaceWidget::relevantSnippetHtml(const SmartSpaceEntry &entry,
                                               int maxContexts,
                                               int before, int after) const
{
    const QString content = entry.content.simplified();
    QString snippetQuery = m_executedQuery;
    if (m_searchUsesRegex && snippetQuery.startsWith(
            QStringLiteral("re:"), Qt::CaseInsensitive))
        snippetQuery = snippetQuery.mid(3);
    const QStringList terms = localQueryTerms(snippetQuery);

    struct Hit { int start; int length; };
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
        return left.start == right.start
            ? left.length > right.length : left.start < right.start;
    });

    auto highlighted = [this, &terms](const QString &value) {
        QString html;
        int offset = 0;
        while (offset < value.size()) {
            int next = -1;
            int length = 0;
            for (const QString &term : terms) {
                const int at = value.indexOf(term, offset, Qt::CaseInsensitive);
                if (at >= 0 && (next < 0 || at < next ||
                                (at == next && term.size() > length))) {
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
                .arg(m_accentColor.name(),
                     value.mid(next, length).toHtmlEscaped());
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
    m_statusLabel->setText(QStringLiteral(
        "已从左侧隐藏“%1” · 文件仍可搜索 · 可在设置中恢复")
        .arg(displayName));
}

void SmartSpaceWidget::excludeFolderFromIndex(const QString &path)
{
    const QString clean = normalizedPath(path);
    if (clean.isEmpty() || pathExcluded(clean))
        return;
    if (qEnvironmentVariableIntValue("UKUI_FENCES_TEST_CONFIRM_EXCLUDE") != 1 &&
        smartQuestion(this, QStringLiteral("取消索引文件夹"),
            QStringLiteral("将从当前结果移除“%1”及其全部子项，后续增量、快速全量和 OCR 都会跳过。\n\n"
                           "文件本身不会删除，可在设置 → 索引范围中恢复。是否继续？")
                .arg(clean),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    m_excludedFolders << clean;
    m_hiddenFolders.removeAll(clean);
    if(m_libraryJobBusy)stopIdleFullIndex();
    m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(),
        [this](const SmartSpaceEntry &entry) { return pathExcluded(entry.path); }),
        m_entries.end());
    if (!m_scopePath.isEmpty() && pathIsInside(m_scopePath, clean))
        m_scopePath.clear();
    saveSettings();
    rebuildWatches();
    updateResults(true);
    m_statusLabel->setText(QStringLiteral(
        "已排除索引“%1” · 点击底部“增量”后更新文件清单 · 设置中可恢复")
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
    auto nameMatchRank = [this, &foldedQuery,
                          &foldedTerms](const SmartSpaceEntry &entry) {
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
            [this](const RelevanceMetric &left,
                   const RelevanceMetric &right) {
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
                return QString::localeAwareCompare(
                    m_entries.at(left.index).name,
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

QWidget *SmartSpaceWidget::createFileCard(const SmartSpaceEntry &entry,
                                          QListWidgetItem *item)
{
    auto *card = new SmartFileCard([this, path = entry.path, item] {
        if (item)
            m_fileList->setCurrentItem(item);
        openPath(path);
    }, m_fileList);
    card->setObjectName(QStringLiteral("fileResultCard"));
    card->setAttribute(Qt::WA_Hover, true);
    const QString toolTip = entryToolTip(entry);
    auto *row = new QHBoxLayout(card);
    static const int iconExtents[] = {14, 16, 18, 20, 25, 30, 34, 38};
    static const int cardHeights[] = {24, 28, 33, 38, 50, 64, 78, 94};
    static const int previewExtents[] = {20, 22, 24, 26, 28, 32, 32, 32};
    const int densityIndex = qBound(
        0, m_resultDensity - kMinResultDensity, kResultDensityLevels - 1);
    const bool compact = m_resultDensity < 0;
    const bool ultraCompact = m_resultDensity <= -2;
    const int iconExtent = iconExtents[densityIndex];
    const int cardHeight = cardHeights[densityIndex];
    row->setContentsMargins(m_resultDensity <= -3 ? 4 : ultraCompact ? 6 : 9,
                            m_resultDensity <= -3 ? 0 : compact ? 2 : 5,
                            m_resultDensity <= -3 ? 4 : 8,
                            m_resultDensity <= -3 ? 0 : compact ? 2 : 5);
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
    if(m_knowledgeAvailability.value(entry.path)==QLatin1String("unseen"))metadata<<QStringLiteral("本轮未发现来源 · 保留库内正文");
    const QString knowledgeState=m_knowledgeExtractionStates.value(entry.path);
    if(knowledgeState==QLatin1String("partial"))metadata<<QStringLiteral("正文部分提取");
    else if(knowledgeState==QLatin1String("failed") || knowledgeState==QLatin1String("missing"))metadata<<QStringLiteral("提取未完成 · 使用已存快照");
    auto *meta = new QLabel(metadata.join(QStringLiteral(" · ")), card);
    meta->setProperty("cardMeta", true);
    meta->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    meta->setVisible(m_resultDensity >= 0);
    auto *metadataRow=new QHBoxLayout;metadataRow->setContentsMargins(0,0,0,0);metadataRow->setSpacing(6);
    metadataRow->addWidget(meta);
    const QString parentName=entry.libraryOnly?entry.libraryName:QFileInfo(entry.path).dir().dirName();
    if (!parentName.isEmpty() && !ultraCompact) {
        auto *folderBadge=new QLabel(card);folderBadge->setProperty("folderBadge",true);
        QFont badgeFont=font();badgeFont.setPixelSize(10);folderBadge->setFont(badgeFont);
        folderBadge->setText(QFontMetrics(badgeFont).elidedText(parentName,Qt::ElideRight,164));
        folderBadge->setMaximumWidth(180);
        folderBadge->setToolTip(entry.libraryOnly?QStringLiteral("知识库副本 · %1").arg(entry.originalPath):QFileInfo(entry.path).absolutePath());
        metadataRow->insertWidget(0,folderBadge);
    }

    metadataRow->addStretch(1);texts->addLayout(metadataRow);

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
        auto *match = new QLabel(
            ultraCompact
                ? QStringLiteral("%1%").arg(matchPercent)
                : QStringLiteral("匹配 %1%").arg(matchPercent),
            card);
        match->setProperty("matchScore", true);
        match->setProperty("matchLevel",
                           matchPercent >= 85 ? QStringLiteral("high")
                                              : QStringLiteral("medium"));
        match->setToolTip(QStringLiteral(
            "本地相关度 %1%：由文件名、路径和正文命中综合换算")
            .arg(matchPercent));
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
    preview->setFixedSize(52,qMax(22,previewExtent));
    connect(preview, &QToolButton::clicked, this,
            [this, path = entry.path] { showPreviewForPath(path); });
    auto *actionSlot=new QWidget(card);
    actionSlot->setFixedSize(preview->size());
    auto *actionLayout=new QHBoxLayout(actionSlot);actionLayout->setContentsMargins(0,0,0,0);actionLayout->addWidget(preview);
    row->addWidget(actionSlot,0,Qt::AlignVCenter);
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

void SmartSpaceWidget::showPreviewForPath(const QString &path)
{
    const SmartSpaceEntry *entry = entryForPath(path);
    if (!entry || entry->isDir || !m_previewPanel)
        return;
    QToolTip::hideText();
    if (m_previewProcess && m_previewProcess->state() != QProcess::NotRunning) {
        m_previewProcess->disconnect(this);m_previewProcess->kill();m_previewProcess->deleteLater();m_previewProcess=nullptr;
    }
    m_pendingPreviewImage.clear();
    m_previewPath = path;
    m_previewVisible = true;
    updateResponsiveLayout();
    m_previewTitle->setText(entry->name);
    m_previewTitle->setToolTip(entry->name);
    QStringList metadata;
    metadata << categoryTitle(entry->category);
    const QString size = formattedFileSize(entry->size);
    const QString modified = formattedModified(entry->modified);
    if (!size.isEmpty())
        metadata << size;
    if (!modified.isEmpty())
        metadata << modified;
    m_previewMeta->setText(metadata.join(QStringLiteral(" · ")));
    m_previewOpenButton->setEnabled(!entry->libraryOnly && QFileInfo::exists(entry->path));
    if(!entry->libraryReference.isEmpty()) {
        m_previewMeta->setText(entry->libraryOnly?QStringLiteral("知识库副本 · %1 · 不含原文件").arg(entry->libraryName):QStringLiteral("个人知识库正文 · 按命中位置读取"));
        const QString state=m_knowledgeExtractionStates.value(entry->path);
        if(state==QLatin1String("partial") || state==QLatin1String("failed") || state==QLatin1String("missing"))m_previewMeta->setText(m_previewMeta->text()+QStringLiteral(" · 提取未完整，请结合来源核对"));
        if(m_knowledgeAvailability.value(entry->path)==QLatin1String("unseen"))m_previewMeta->setText(m_previewMeta->text()+QStringLiteral(" · 本轮未发现来源，内容为已存快照"));
        readLibraryPreview(*entry);return;
    }
    m_previewImage->clear();

    if (entry->category == QLatin1String("image") &&
        QFileInfo::exists(entry->path)) {
        showPreviewImage(entry->path);
    } else if (entry->category == QLatin1String("pdf") &&
               QFileInfo::exists(entry->path)) {
        startPdfPreview(*entry);
    } else {
        m_previewStack->setCurrentWidget(m_previewText);
        const QString query = m_executedQuery;
        if (!query.isEmpty()) {
            m_previewText->setHtml(QStringLiteral(
                "<h3>%1</h3><p>%2</p>")
                .arg(QStringLiteral("最相关的索引片段"),
                     relevantSnippetHtml(*entry, 3, 56, 120)));
        } else if (!entry->content.trimmed().isEmpty()) {
            QString body = entry->content.left(20000).toHtmlEscaped();
            body.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
            if (entry->content.size() > 20000)
                body.append(QStringLiteral("<br>…"));
            m_previewText->setHtml(body);
        } else {
            m_previewText->setHtml(QStringLiteral(
                "<p>当前索引没有可预览的正文。</p>"
                "<p>可直接打开原文件。</p>"));
        }
    }
}

void SmartSpaceWidget::readLibraryPreview(const SmartSpaceEntry &entry,int offset)
{
    if(m_libraryPreviewProcess)m_libraryPreviewProcess->kill();
    setProperty("libraryPreviewLoaded",false);
    m_previewStack->setCurrentWidget(m_previewText);m_previewText->setHtml(QStringLiteral("正在读取库内正文…"));
    auto *process=new QProcess(this);m_libraryPreviewProcess=process;
    const QString expected=entry.path;
    connect(process,&QProcess::errorOccurred,this,[this,process,expected](QProcess::ProcessError error){
        if(error==QProcess::FailedToStart){if(m_libraryPreviewProcess==process && m_previewPath==expected)m_previewText->setPlainText(QStringLiteral("无法启动独立知识库检索工具，请检查 Python3 与安装文件。"));process->deleteLater();}
    });
    connect(process,QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished),this,[this,process,expected](int code,QProcess::ExitStatus){
        const auto data=QJsonDocument::fromJson(process->readAllStandardOutput().trimmed()).object();process->deleteLater();
        if(m_libraryPreviewProcess!=process || !m_previewVisible || m_previewPath!=expected)return;
        if(code!=0 || !data.value(QStringLiteral("error")).toString().isEmpty()) {m_previewText->setPlainText(QStringLiteral("无法读取知识库正文：%1").arg(data.value(QStringLiteral("error")).toString()));return;}
        QString html;
        if(data.value(QStringLiteral("results")).toArray().isEmpty())html=QStringLiteral("<p>当前库只有该文档的元数据，没有可用正文。</p><p>需要在资料来源机器补充文本提取或 OCR，再导出新版本。此处不能恢复缺失的原件。</p>");
        for(const auto &v:data.value(QStringLiteral("results")).toArray()) {
            const auto row=v.toObject();QString text=row.value(QStringLiteral("content")).toString().toHtmlEscaped();text.replace(QLatin1Char('\n'),QStringLiteral("<br>"));
            html+=QStringLiteral("<h4>%1</h4><p>%2</p>").arg(row.value(QStringLiteral("locator")).toString().toHtmlEscaped(),text);
        }
        if(data.value(QStringLiteral("hasMore")).toBool())html+=QStringLiteral("<p><a href=\"aispace-next:%1\">继续读取后续片段 →</a></p>").arg(data.value(QStringLiteral("nextOffset")).toInt());
        m_previewText->setHtml(html);setProperty("libraryPreviewLoaded",true);
    });
    QStringList args{QFileInfo(knowledgeWorkerPath()).dir().filePath(QStringLiteral("knowledge_library.py")),QStringLiteral("--db"),entry.libraryDb,QStringLiteral("fetch"),QStringLiteral("--reference"),entry.libraryReference,QStringLiteral("--limit"),QStringLiteral("8")};
    if(offset>=0)args<<QStringLiteral("--offset")<<QString::number(offset);
    process->start(QStandardPaths::findExecutable(QStringLiteral("python3")),args);
    QTimer::singleShot(5000,process,[process]{if(process->state()!=QProcess::NotRunning)process->kill();});
}

void SmartSpaceWidget::hidePreview()
{
    // Invalidate the result before stopping the process. finished() is handled
    // asynchronously and must not revive the preview during retraction.
    m_previewVisible = false;
    setProperty("libraryPreviewLoaded",false);
    if(m_libraryPreviewProcess)m_libraryPreviewProcess->kill();
    m_previewPath.clear();
    m_pendingPreviewImage.clear();
    if (m_previewProcess && m_previewProcess->state() != QProcess::NotRunning)
        m_previewProcess->kill();
    if (m_previewImage)
        m_previewImage->clear();
    if (m_previewImage)
        m_previewImage->setMinimumSize(QSize(0, 0));
    if (m_previewPanel)
        m_previewPanel->hide();
    updateResponsiveLayout();
}

void SmartSpaceWidget::showPreviewImage(const QString &imagePath)
{
    QImageReader reader(imagePath);
    reader.setAutoTransform(true);
    const QSize original = reader.size();
    if (original.isValid()) {
        const int previewWidth = m_previewPanel
            ? qMax(240, m_previewPanel->width() - 40) : 800;
        const QSize bounded = original.scaled(QSize(previewWidth, 1600),
                                              Qt::KeepAspectRatio);
        if (bounded.width() < original.width() || bounded.height() < original.height())
            reader.setScaledSize(bounded);
    }
    const QImage image = reader.read();
    if (image.isNull()) {
        const SmartSpaceEntry *entry = entryForPath(m_previewPath);
        m_previewStack->setCurrentWidget(m_previewText);
        m_previewText->setHtml(entry
            ? relevantSnippetHtml(*entry, 3, 56, 120)
            : QStringLiteral("预览图片加载失败。"));
        return;
    }
    m_previewImage->setPixmap(QPixmap::fromImage(image));
    m_previewImage->setMinimumSize(image.size());
    m_previewStack->setCurrentIndex(1);
}

void SmartSpaceWidget::startPdfPreview(const SmartSpaceEntry &entry)
{
    const QString pdftoppm = QStandardPaths::findExecutable(
        QStringLiteral("pdftoppm"));
    if (pdftoppm.isEmpty()) {
        m_previewStack->setCurrentWidget(m_previewText);
        m_previewText->setHtml(relevantSnippetHtml(entry, 3, 56, 120));
        return;
    }

    const QByteArray identity = QStringLiteral("%1|%2|%3")
        .arg(entry.path).arg(entry.size)
        .arg(entry.modified.toMSecsSinceEpoch()).toUtf8();
    const QString key = QString::fromLatin1(
        QCryptographicHash::hash(identity, QCryptographicHash::Sha1).toHex());
    const QString cacheDir = QStandardPaths::writableLocation(
        QStandardPaths::CacheLocation) + QStringLiteral("/smart-space/previews");
    QDir().mkpath(cacheDir);
    m_pendingPreviewImage = cacheDir + QLatin1Char('/') + key + QStringLiteral(".png");
    if (QFileInfo::exists(m_pendingPreviewImage)) {
        showPreviewImage(m_pendingPreviewImage);
        prunePreviewCache();
        return;
    }

    m_previewStack->setCurrentWidget(m_previewText);
    m_previewText->setHtml(QStringLiteral(
        "<p>正在低优先级生成 PDF 首页预览…</p>"));
    QString prefix = m_pendingPreviewImage;
    prefix.chop(4);
    QStringList renderArguments{
        QStringLiteral("-f"), QStringLiteral("1"),
        QStringLiteral("-l"), QStringLiteral("1"),
        QStringLiteral("-singlefile"), QStringLiteral("-r"),
        QStringLiteral("120"), QStringLiteral("-png"),
        entry.path, prefix
    };
    auto *process=new QProcess(this);m_previewProcess=process;process->setProcessChannelMode(QProcess::MergedChannels);
    const QString requestPath=m_previewPath,image=m_pendingPreviewImage;
    auto finish=[this,process,requestPath,image](bool success){
        if(m_previewProcess==process){m_previewProcess=nullptr;
            if(m_previewVisible && m_previewPath==requestPath){
                if(success && QFileInfo::exists(image)){showPreviewImage(image);prunePreviewCache();}
                else{const auto *entry=entryForPath(requestPath);m_previewStack->setCurrentWidget(m_previewText);m_previewText->setHtml(entry?relevantSnippetHtml(*entry,3,56,120):QStringLiteral("预览未完成，可重试。"));}
            }
        }process->deleteLater();
    };
    connect(process,QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished),this,[finish](int code,QProcess::ExitStatus status){finish(code==0 && status==QProcess::NormalExit);});
    connect(process,&QProcess::errorOccurred,this,[finish](QProcess::ProcessError error){if(error==QProcess::FailedToStart)finish(false);});
    QTimer::singleShot(15000,process,[process]{if(process->state()!=QProcess::NotRunning)process->kill();});
    const QString nice = QStandardPaths::findExecutable(QStringLiteral("nice"));
    const QString ionice = QStandardPaths::findExecutable(QStringLiteral("ionice"));
    if (!ionice.isEmpty()) {
        QStringList arguments{QStringLiteral("-c"), QStringLiteral("3")};
        if (!nice.isEmpty())
            arguments << nice << QStringLiteral("-n") << QStringLiteral("15");
        arguments << pdftoppm;
        arguments.append(renderArguments);
        m_previewProcess->start(ionice, arguments);
    } else if (!nice.isEmpty()) {
        QStringList arguments{QStringLiteral("-n"), QStringLiteral("15"), pdftoppm};
        arguments.append(renderArguments);
        m_previewProcess->start(nice, arguments);
    } else {
        m_previewProcess->start(pdftoppm, renderArguments);
    }
}

void SmartSpaceWidget::prunePreviewCache()
{
    const QString cacheDir = QStandardPaths::writableLocation(
        QStandardPaths::CacheLocation) + QStringLiteral("/smart-space/previews");
    QDir dir(cacheDir);
    const QFileInfoList previews = dir.entryInfoList(
        {QStringLiteral("*.png")}, QDir::Files, QDir::Time);
    for (int index = 3; index < previews.size(); ++index)
        QFile::remove(previews.at(index).absoluteFilePath());
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
    const QString pageKey = QStringLiteral("%1\n%2\n%3\n%4\n%5\n%6\n%7\n%8")
        .arg(m_executedQuery,
             categories.join(QLatin1Char(',')),
             visibleExtensions.join(QLatin1Char(',')),
             excludedFolders.join(QLatin1Char('\n')),
             m_scopePath,
             QString::number(m_resultSortMode),
 QString(),
             QString::number(m_entries.size()));
    if (pageKey != m_resultPageKey) {
        m_resultPageKey = pageKey;
        m_visibleResultLimit = kInitialResultsPageSize;
    }

    m_fileList->setSpacing(m_resultDensity <= -3 ? 0
                           : m_resultDensity <= -2 ? 1
                           : m_resultDensity < 0 ? 2 : 4);
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
        loadMore->setText(QStringLiteral(
            "加载更多 %1 个 · 剩余 %2")
            .arg(nextBatch).arg(remaining));
        loadMore->setToolTip(QStringLiteral(
            "首次显示 32 项，之后每次增加 100 项，避免大量结果一次创建控件"));
        m_fileList->setItemWidget(moreItem, loadMore);
        connect(loadMore, &QToolButton::clicked, this, [this, visible] {
            m_visibleResultLimit += kResultsPageSize;
            updateResults(false);
            if (visible < m_fileList->count()) {
                if (QListWidgetItem *firstNew = m_fileList->item(visible))
                    m_fileList->scrollToItem(
                        firstNew, QAbstractItemView::PositionAtCenter);
            }
        });
    }
    if (visible == 0) {
        auto *empty = new QListWidgetItem(
            QIcon::fromTheme(QStringLiteral("edit-find")),
            m_executedQuery.isEmpty()?QStringLiteral("输入关键词检索知识库，或从向导建立个人库")
                                      :QStringLiteral("没有匹配的文件或知识 · 请查看检索状态"),
            m_fileList);
        empty->setFlags(Qt::NoItemFlags);
    }

    QString scope = m_scopePath.isEmpty()
        ? QStringLiteral("全部目录")
        : QFileInfo(m_scopePath).fileName();
    if (scope.isEmpty())
        scope = m_scopePath;
    if (m_folderTitleLabel)
        m_folderTitleLabel->setText(QStringLiteral("文件夹拾取"));
    if (m_fileTitleLabel)
        m_fileTitleLabel->setText(QStringLiteral("匹配结果"));
    if (m_fileCountLabel)
        m_fileCountLabel->setText(QStringLiteral("%1 项").arg(
            QLocale::system().toString(matches.size())));
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
        m_folderCountLabel->setText(QStringLiteral("%1 项").arg(
            QLocale::system().toString(folderCount)));
    }
    m_statusLabel->setText(QStringLiteral("%1%2 · 匹配 %3 个文件%4")
        .arg(scope)
        .arg(QString())
        .arg(matches.size())
        .arg(matches.size() > visible
            ? QStringLiteral(" · 显示前 %1 个").arg(visible) : QString()));
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
    const int densityIndex = qBound(
        0, m_resultDensity - kMinResultDensity, kResultDensityLevels - 1);
    const int rowHeight = rowHeights[densityIndex];
    for (int level = 0; level < m_folderLevelWidgets.size(); ++level) {
        const QList<QListWidget *> lists =
            m_folderLevelWidgets.at(level)->findChildren<QListWidget *>();
        for (QListWidget *list : lists) {
            list->setIconSize(QSize(iconSizes[densityIndex],
                                    iconSizes[densityIndex]));
            list->setSpacing(m_resultDensity <= -3 ? 0
                             : m_resultDensity <= -1 ? 1
                             : m_resultDensity >= 1 ? 4 : 2);
            for (int row = 0; row < list->count(); ++row)
                list->item(row)->setSizeHint(QSize(0, rowHeight));
            if (level > 0) {
                const int visibleRows = qBound(2, list->count(), 5);
                list->setMaximumHeight(qMin(320,
                    visibleRows * rowHeight + 12));
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
    QHash<QString,QHash<QString,QString>> groupByParent;
    QStringList configuredRoots;
    for(const auto &root:m_roots)configuredRoots<<normalizedPath(root);
    for(const auto &entry:m_entries) {
        if(!fileMatchesCurrentResults(entry,false))continue;
        const QString parent=entry.path.left(entry.path.lastIndexOf(QLatin1Char('/')));
        auto &parents=groupByParent[entry.root];
        auto cached=parents.constFind(parent);
        QString key;
        if(cached!=parents.constEnd())key=cached.value();
        else {
            QStringList roots;
            if(!entry.root.isEmpty())roots<<normalizedPath(entry.root);
            for(const auto &root:configuredRoots)if(!roots.contains(root))roots<<root;
            for(const auto &root:roots) {
                const QString prefix=root==QStringLiteral("/")?root:root+QLatin1Char('/');
                if(!entry.path.startsWith(prefix))continue;
                const int separator=entry.path.indexOf(QLatin1Char('/'),prefix.size());
                if(separator<0)break;
                const QString candidate=entry.path.left(separator);
                if(!folderHidden(candidate))key=candidate;
                break;
            }
            parents.insert(parent,key);
        }
        if(key.isEmpty())continue;
        const auto found=folderIndexes.constFind(key);
        if(found==folderIndexes.constEnd()) {
            folderIndexes.insert(key,folders.size());
            folders.append({key,QFileInfo(key).fileName(),1});
        } else ++folders[found.value()].count;
    }
    setProperty("navigationGroups",folders.size());
    std::sort(folders.begin(), folders.end(), [](const FolderResult &a,
                                                  const FolderResult &b) {
        return QString::localeAwareCompare(a.name, b.name) < 0;
    });
    for (const FolderResult &folder : folders) {
        auto *item = new QListWidgetItem(
            QIcon::fromTheme(QStringLiteral("folder")),
            folder.name, list);
        item->setData(kFolderCountRole,folder.count);
        item->setData(PathRole, normalizedPath(folder.path));
        item->setToolTip(folder.path);
    }
    if (folders.isEmpty()) {
        auto *empty = new QListWidgetItem(
            QIcon::fromTheme(QStringLiteral("folder")),
            m_entries.isEmpty() ? QStringLiteral("本地目录为空 · 已导入的知识库可直接搜索")
                                : QStringLiteral("当前条件下没有子文件夹"), list);
        empty->setFlags(Qt::NoItemFlags);
    }
    connect(list, &QListWidget::itemClicked,
            this, [this](QListWidgetItem *item) { folderActivated(item, 0); });
    connect(list, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem *item) {
                openPath(item->data(PathRole).toString());
            });
    connect(list, &QListWidget::customContextMenuRequested,
            this, [this, list](const QPoint &pos) { showEntryMenu(list, pos); });
    frame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_folderLevelsLayout->insertWidget(
        m_folderLevelsLayout->count() - 1, frame, 1);
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
        if (!fileMatchesCurrentResults(entry, false) ||
            !pathIsInside(entry.path, cleanParent))
            continue;
        const QString relative = QDir(cleanParent).relativeFilePath(entry.path);
        const int separator = relative.indexOf(QLatin1Char('/'));
        if (separator <= 0)
            continue;
        const QString key = normalizedPath(
            QDir(cleanParent).filePath(relative.left(separator)));
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
    std::sort(folders.begin(), folders.end(), [](const FolderResult &a,
                                                  const FolderResult &b) {
        return QString::localeAwareCompare(a.name, b.name) < 0;
    });
    if (folders.isEmpty())
        return;

    auto *frame = new QFrame(m_folderLevelsContainer);
    auto *layout = new QVBoxLayout(frame);
    layout->setContentsMargins(8, 7, 0, 0);
    layout->setSpacing(4);
    QString title = QFileInfo(parentPath).fileName();
    if (title.isEmpty()) title = parentPath;
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
        auto *item = new QListWidgetItem(
            QIcon::fromTheme(QStringLiteral("folder")),
            folder.name, list);
        item->setData(kFolderCountRole,folder.count);
        item->setData(PathRole, folder.path);
        item->setToolTip(folder.path);
    }
    connect(list, &QListWidget::itemClicked,
            this, [this, level](QListWidgetItem *item) {
                folderActivated(item, level);
            });
    connect(list, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem *item) {
                openPath(item->data(PathRole).toString());
            });
    connect(list, &QListWidget::customContextMenuRequested,
            this, [this, list](const QPoint &pos) { showEntryMenu(list, pos); });
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

int SmartSpaceWidget::indexedEntryCount() const {return m_entries.size()-m_libraryOnlyEntryCount;}
int SmartSpaceWidget::resultCount() const {int count=0;if(m_fileList)for(int row=0;row<m_fileList->count();++row)if(!m_fileList->item(row)->data(PathRole).toString().isEmpty())++count;return count;}
void SmartSpaceWidget::searchFor(const QString &query) { m_searchEdit->setText(query);executeSearch(); }

void SmartSpaceWidget::openFileItem(QListWidgetItem *item)
{
    if (item)
        openPath(item->data(PathRole).toString());
}

void SmartSpaceWidget::openPath(const QString &path)
{
    if(path.startsWith(QLatin1String("aispace://"))){showPreviewForPath(path);return;}
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
    if(path.startsWith(QLatin1String("aispace://"))) {
        auto *read=menu.addAction(QStringLiteral("阅读库内正文"));auto *copy=menu.addAction(QStringLiteral("复制知识引用"));
        connect(read,&QAction::triggered,this,[this,path]{showPreviewForPath(path);});
        connect(copy,&QAction::triggered,this,[path]{QApplication::clipboard()->setText(path);});
        menu.exec(list->viewport()->mapToGlobal(position));return;
    }
    QAction *open = menu.addAction(QIcon::fromTheme(QStringLiteral("document-open")),
                                   QFileInfo(path).isDir()
                                       ? QStringLiteral("用文件管理器打开")
                                       : QStringLiteral("打开"));
    connect(open, &QAction::triggered, this, [this, path] { openPath(path); });
    if (QFileInfo(path).isDir()) {
        menu.addSeparator();
        QAction *hideFolder = menu.addAction(
            QIcon::fromTheme(QStringLiteral("view-hidden")),
            QStringLiteral("取消显示（仅隐藏左栏）"));
        hideFolder->setToolTip(QStringLiteral("不影响索引和右侧搜索结果"));
        QAction *excludeFolder = menu.addAction(
            QIcon::fromTheme(QStringLiteral("edit-delete")),
            QStringLiteral("取消索引此文件夹…"));
        excludeFolder->setToolTip(QStringLiteral(
            "从索引中排除此文件夹及全部子项，不删除本地文件"));
        connect(hideFolder, &QAction::triggered, this,
                [this, path] { hideFolderFromNavigation(path); });
        connect(excludeFolder, &QAction::triggered, this,
                [this, path] { excludeFolderFromIndex(path); });
    } else {
        QAction *containing = menu.addAction(
            QIcon::fromTheme(QStringLiteral("folder-open")),
            QStringLiteral("打开所在文件夹"));
        connect(containing, &QAction::triggered, this, [this, path] {
            openPath(QFileInfo(path).absolutePath());
        });
    }
    menu.exec(list->viewport()->mapToGlobal(position));
}

void SmartSpaceWidget::showSettingsDialog()
{
    setSettingsViewVisible(true);
}

QWidget *SmartSpaceWidget::createSettingsPage(QWidget *parent)
{
    auto *form = new QWidget(parent);
    form->setObjectName(QStringLiteral("smartSettingsPage"));
    QWidget &dialog = *form;
    connect(this, &QObject::destroyed, form, &QObject::deleteLater);
    dialog.setWindowTitle(QStringLiteral("AI空间设置"));
    QColor buttonHover = m_accentColor;
    buttonHover.setAlpha(28);
    QColor accentSoft = m_accentColor;
    accentSoft.setAlpha(m_themeMode == 2 ? 26 : 48);
    const QColor field = m_themeMode == 2 ? QColor(255,255,255,58) : QColor(15,23,42,58);
    dialog.setStyleSheet(QStringLiteral(R"(
        QWidget#smartSettingsPage { background: transparent; color: %1; }
        QLabel#settingsPageTitle { color: %1; font-size: 18px; font-weight: 700; }
        QLabel[settingsSubtitle="true"] { color: %3; font-size: 12px; }
        QLabel[settingsHint="true"] { color: %3; background: %8;
                                      border: 1px solid %2; border-radius: 10px;
                                      padding: 10px 12px; }
        QTabWidget::pane { background: %5; border: 1px solid %2;
                           border-radius: 14px; top: 10px; }
        QTabWidget::tab-bar { left: 0px; }
        QTabBar { background: transparent; }
        QTabBar::tab { color: %3; background: transparent; padding: 7px 16px;
                       margin-right: 6px; border: 1px solid transparent;
                       border-radius: 16px; font-weight: 500; }
        QTabBar::tab:hover { color: %1; background: %4; }
        QTabBar::tab:selected { color: %6; background: %7; border-color: %7;
                                font-weight: 600; }
        QScrollArea#settingsPageScroll, QWidget#settingsPage {
            background: transparent; border: none;
        }
        QLabel[settingsSectionTitle="true"] { color: %1; font-size: 14px; font-weight: 600; }
        QListWidget { background: %8; border: 1px solid %2; border-radius: 10px;
                      padding: 4px; }
        QPushButton { color: %1; background: %5; border: 1px solid %2;
                      border-radius: 9px; padding: 7px 15px; }
        QPushButton[settingsGroupAction="true"] {
            padding: 4px 12px; min-height: 22px;
        }
        QPushButton:hover { background: %4; border-color: %9; }
        QPushButton#primarySettingsButton { color: white; background: #4f46e5;
                                            border-color: #4f46e5; font-weight: 600;
                                            padding: 8px 20px; }
        QPushButton#primarySettingsButton:hover { background: #4338ca; border-color: #4338ca; }
        QPushButton#settingsBackButton { padding: 8px 16px; }
        QCheckBox, QLabel { color: %1; }
        QCheckBox { spacing: 8px; }
        QLineEdit, QComboBox, QSpinBox, QPlainTextEdit { background: %8; }
        QToolButton[themeCard="true"] {
            color: %1; background: %8; border: 2px solid %2; border-radius: 14px;
            padding: 10px; font-weight: 600;
        }
        QToolButton[themeCard="true"]:hover { border-color: %6; }
        QToolButton[themeCard="true"]:checked { border-color: %6; background: %7; }
    )").arg(cssColor(m_textColor),
             cssColor(m_borderColor), cssColor(m_mutedColor),
             cssColor(buttonHover), cssColor(m_cardColor),
             cssColor(m_accentColor), cssColor(accentSoft), cssColor(field))
        .arg(cssColor(m_accentColor.darker(112))));
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(22, 16, 22, 16);
    layout->setSpacing(10);
    auto *pageHeader = new QHBoxLayout;
    pageHeader->setSpacing(10);
    auto *titleColumn = new QVBoxLayout;
    titleColumn->setSpacing(2);
    auto *pageTitle = new QLabel(QStringLiteral("偏好设置"), &dialog);
    pageTitle->setObjectName(QStringLiteral("settingsPageTitle"));
    titleColumn->addWidget(pageTitle);
    auto *settingsSubtitle = new QLabel(QStringLiteral(
        "索引范围、更新策略、外观和本地 Skill 配置均保存在本机"), &dialog);
    settingsSubtitle->setProperty("settingsSubtitle", true);
    titleColumn->addWidget(settingsSubtitle);
    pageHeader->addLayout(titleColumn, 1);
    auto *backButton = new QPushButton(QStringLiteral("返回检索"), &dialog);
    backButton->setObjectName(QStringLiteral("settingsBackButton"));
    backButton->setCursor(Qt::PointingHandCursor);
    auto *saveButton = new QPushButton(QStringLiteral("保存设置"), &dialog);
    saveButton->setObjectName(QStringLiteral("primarySettingsButton"));
    saveButton->setCursor(Qt::PointingHandCursor);
    saveButton->setMinimumWidth(104);
    pageHeader->addWidget(backButton, 0, Qt::AlignVCenter);
    pageHeader->addWidget(saveButton, 0, Qt::AlignVCenter);
    layout->addLayout(pageHeader);
    connect(backButton, &QPushButton::clicked, this, [this] {
        QTimer::singleShot(0, this, [this] { setSettingsViewVisible(false); });
    });
    auto *tabs = new QTabWidget(&dialog);
    tabs->setDocumentMode(false);
    tabs->tabBar()->setDrawBase(false);
    tabs->tabBar()->setExpanding(false);
    tabs->tabBar()->setCursor(Qt::PointingHandCursor);
    layout->addWidget(tabs, 1);

    auto *indexTab = new QScrollArea(tabs);
    indexTab->setObjectName(QStringLiteral("settingsPageScroll"));
    indexTab->setWidgetResizable(true);
    indexTab->setFrameShape(QFrame::NoFrame);
    indexTab->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *indexPage = new QWidget(indexTab);
    indexPage->setObjectName(QStringLiteral("settingsPage"));
    indexPage->setMinimumHeight(570);
    auto *indexLayout = new QVBoxLayout(indexPage);
    indexLayout->setContentsMargins(12, 12, 12, 12);
    indexLayout->setSpacing(8);
    auto *addRoot = new QPushButton(QStringLiteral("添加…"), indexPage);
    auto *removeRoot = new QPushButton(QStringLiteral("移除"), indexPage);
    addRoot->setProperty("settingsGroupAction", true);
    removeRoot->setProperty("settingsGroupAction", true);
    auto *rootHeadingRow = new QHBoxLayout;
    rootHeadingRow->setContentsMargins(0, 0, 0, 0);
    rootHeadingRow->setSpacing(6);
    auto *rootHeading = new QLabel(QStringLiteral("索引目录"), indexPage);
    rootHeading->setProperty("settingsSectionTitle", true);
    rootHeadingRow->addWidget(rootHeading);
    rootHeadingRow->addStretch(1);
    rootHeadingRow->addWidget(addRoot);
    rootHeadingRow->addWidget(removeRoot);
    indexLayout->addLayout(rootHeadingRow);
    auto *rootDescription = new QLabel(
        QStringLiteral("只选择需要搜索的目录；范围越精确，索引越快、资源越低。"), indexPage);
    rootDescription->setProperty("settingsSubtitle", true);
    indexLayout->addWidget(rootDescription);
    auto *rootList = new QListWidget(indexPage);
    rootList->addItems(m_roots);
    rootList->setFixedHeight(82);
    indexLayout->addWidget(rootList);

    auto *addExclude = new QPushButton(QStringLiteral("添加排除…"), indexPage);
    auto *restoreExclude = new QPushButton(QStringLiteral("恢复索引"), indexPage);
    addExclude->setProperty("settingsGroupAction", true);
    restoreExclude->setProperty("settingsGroupAction", true);
    auto *excludeHeadingRow = new QHBoxLayout;
    excludeHeadingRow->setContentsMargins(0, 0, 0, 0);
    excludeHeadingRow->setSpacing(6);
    auto *excludeHeading = new QLabel(QStringLiteral("排除文件夹（不进入索引）"), indexPage);
    excludeHeading->setProperty("settingsSectionTitle", true);
    excludeHeadingRow->addWidget(excludeHeading);
    excludeHeadingRow->addStretch(1);
    excludeHeadingRow->addWidget(addExclude);
    excludeHeadingRow->addWidget(restoreExclude);
    indexLayout->addLayout(excludeHeadingRow);
    auto *excludeList = new QListWidget(indexPage);
    excludeList->addItems(m_excludedFolders);
    excludeList->setFixedHeight(66);
    excludeList->setToolTip(QStringLiteral("增量、快速全量和 OCR 补全都会跳过这些文件夹及子项"));
    indexLayout->addWidget(excludeList);
    auto *restoreHidden = new QPushButton(QStringLiteral("恢复显示"), indexPage);
    restoreHidden->setProperty("settingsGroupAction", true);
    auto *hiddenHeadingRow = new QHBoxLayout;
    hiddenHeadingRow->setContentsMargins(0, 0, 0, 0);
    hiddenHeadingRow->setSpacing(6);
    auto *hiddenHeading = new QLabel(QStringLiteral("左栏隐藏文件夹（仍可搜索）"), indexPage);
    hiddenHeading->setProperty("settingsSectionTitle", true);
    hiddenHeadingRow->addWidget(hiddenHeading);
    hiddenHeadingRow->addStretch(1);
    hiddenHeadingRow->addWidget(restoreHidden);
    indexLayout->addLayout(hiddenHeadingRow);
    auto *hiddenList = new QListWidget(indexPage);
    hiddenList->addItems(m_hiddenFolders);
    hiddenList->setFixedHeight(54);
    indexLayout->addWidget(hiddenList);
    auto *resourceHint=new QLabel(QStringLiteral("文本与OCR资源设置请使用分阶段建库向导。①扫描不提取正文；②提取文本；③按CPU和内存限额补OCR。"),indexPage);
    resourceHint->setWordWrap(true);
    resourceHint->setProperty("settingsHint", true);
    indexLayout->addWidget(resourceHint);
    indexLayout->addStretch(1);
    indexTab->setWidget(indexPage);
    tabs->addTab(indexTab, QStringLiteral("索引范围"));

    auto *formatTab = new QWidget(tabs);
    auto *formatLayout = new QVBoxLayout(formatTab);
    formatLayout->setContentsMargins(14, 14, 14, 14);
    formatLayout->setSpacing(8);
    auto formatItemText = [](QListWidgetItem *item) {
        const QString label = item->data(Qt::UserRole + 1).toString();
        return QStringLiteral("%1  %2")
            .arg(item->checkState() == Qt::Checked
                    ? QStringLiteral("✔") : QStringLiteral("○"),
                 label);
    };
    auto createFormatList = [&formatTab, formatItemText](const QStringList &selected) {
        auto *list = new QListWidget(formatTab);
        list->setViewMode(QListView::IconMode);
        list->setResizeMode(QListView::Adjust);
        list->setMovement(QListView::Static);
        list->setWrapping(true);
        list->setGridSize(QSize(138, 31));
        list->setSpacing(2);
        const bool all = selected.isEmpty();
        for (const FormatOption &option : formatOptions()) {
            const QString id = QString::fromLatin1(option.id);
            const QString label =
                id == QLatin1String("__other__")
                    ? QString::fromUtf8(option.label)
                    : QStringLiteral("%1  .%2")
                        .arg(QString::fromLatin1(option.label), id);
            auto *item = new QListWidgetItem(list);
            item->setData(Qt::UserRole, id);
            item->setData(Qt::UserRole + 1, label);
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            item->setCheckState(all || selected.contains(id)
                ? Qt::Checked : Qt::Unchecked);
            item->setText(formatItemText(item));
        }
        QObject::connect(list, &QListWidget::itemChanged, list,
                         [formatItemText](QListWidgetItem *item) {
            const QString text = formatItemText(item);
            if (item->text() != text)
                item->setText(text);
        });
        QObject::connect(list, &QListWidget::itemPressed, list,
                         [](QListWidgetItem *item) {
            if (!item)
                return;
            item->setCheckState(item->checkState() == Qt::Checked
                ? Qt::Unchecked : Qt::Checked);
        });
        return list;
    };
    auto *indexFormatHeading = new QLabel(QStringLiteral("索引格式白名单"), formatTab);
    indexFormatHeading->setStyleSheet(QStringLiteral("font-weight: 600;"));
    formatLayout->addWidget(indexFormatHeading);
    auto *indexFormatHint = new QLabel(QStringLiteral(
        "只有勾选的文件才会进入索引；文件夹本身始终保留以支持向下钻取。"), formatTab);
    indexFormatHint->setProperty("settingsSubtitle", true);
    indexFormatHint->setWordWrap(true);
    formatLayout->addWidget(indexFormatHint);
    auto *indexFormatList = createFormatList(m_indexedExtensions);
    indexFormatList->setMinimumHeight(125);
    indexFormatList->setMaximumHeight(145);
    formatLayout->addWidget(indexFormatList, 1);
    auto *indexFormatButtons = new QHBoxLayout;
    auto *indexSelectAll = new QPushButton(QStringLiteral("全选"), formatTab);
    auto *indexSelectCommon = new QPushButton(QStringLiteral("仅常用办公格式"), formatTab);
    auto *indexClear = new QPushButton(QStringLiteral("清空"), formatTab);
    indexFormatButtons->addWidget(indexSelectAll);
    indexFormatButtons->addWidget(indexSelectCommon);
    indexFormatButtons->addWidget(indexClear);
    indexFormatButtons->addStretch(1);
    formatLayout->addLayout(indexFormatButtons);

    auto *visibleFormatHeading = new QLabel(QStringLiteral("匹配文件显示格式"), formatTab);
    visibleFormatHeading->setStyleSheet(QStringLiteral("font-weight: 600;"));
    formatLayout->addWidget(visibleFormatHeading);
    auto *visibleFormatHint = new QLabel(QStringLiteral(
        "仅控制右侧匹配文件的显示范围，不会删除已有索引。"), formatTab);
    visibleFormatHint->setProperty("settingsSubtitle", true);
    visibleFormatHint->setWordWrap(true);
    formatLayout->addWidget(visibleFormatHint);
    auto *visibleFormatList = createFormatList(m_visibleExtensions);
    visibleFormatList->setMinimumHeight(115);
    visibleFormatList->setMaximumHeight(135);
    formatLayout->addWidget(visibleFormatList, 1);
    auto *visibleFormatButtons = new QHBoxLayout;
    auto *visibleSelectAll = new QPushButton(QStringLiteral("全选"), formatTab);
    auto *visibleSelectIndexed = new QPushButton(QStringLiteral("与索引格式同步"), formatTab);
    auto *visibleClear = new QPushButton(QStringLiteral("清空"), formatTab);
    visibleFormatButtons->addWidget(visibleSelectAll);
    visibleFormatButtons->addWidget(visibleSelectIndexed);
    visibleFormatButtons->addWidget(visibleClear);
    visibleFormatButtons->addStretch(1);
    formatLayout->addLayout(visibleFormatButtons);
    tabs->addTab(formatTab, QStringLiteral("文件格式"));

    auto *updateTab = new QWidget(tabs);
    auto *updateLayout = new QVBoxLayout(updateTab);
    updateLayout->setContentsMargins(14, 14, 14, 14);
    updateLayout->setSpacing(12);
    auto *modeRow = new QHBoxLayout;
    auto *indexMode = new QComboBox(updateTab);
    indexMode->addItem(QStringLiteral("仅手动（推荐，打开时不扫描）"), 0);
    indexMode->addItem(QStringLiteral("文件变化后增量更新"), 1);
    indexMode->addItem(QStringLiteral("定时增量更新"), 2);
    indexMode->setCurrentIndex(indexMode->findData(m_indexMode));
    auto *interval = new QSpinBox(updateTab);
    interval->setRange(5, 1440);
    interval->setValue(m_indexIntervalMinutes);
    interval->setSuffix(QStringLiteral(" 分钟"));
    interval->setEnabled(m_indexMode == 2);
    modeRow->addWidget(new QLabel(QStringLiteral("更新方式："), updateTab));
    modeRow->addWidget(indexMode, 1);
    modeRow->addWidget(interval);
    updateLayout->addLayout(modeRow);
    connect(indexMode, QOverload<int>::of(&QComboBox::currentIndexChanged),
            interval, [indexMode, interval](int) {
        interval->setEnabled(indexMode->currentData().toInt() == 2);
    });

    auto *autoStart = new QCheckBox(QStringLiteral("登录后启动AI空间"),updateTab);
    autoStart->setChecked(m_autoStart);
    updateLayout->addWidget(autoStart);
    auto *syncKylin = new QCheckBox(
        QStringLiteral("同步目录到麒麟文件索引服务（如果已安装）"), updateTab);
    syncKylin->setChecked(true);
    updateLayout->addWidget(syncKylin);
    auto *indexNow = new QCheckBox(
        QStringLiteral("保存设置后立即更新一次索引"), updateTab);
    indexNow->setChecked(false);
    updateLayout->addWidget(indexNow);
    QString snapshotText = QStringLiteral("当前没有索引快照。");
    QFile snapshot(indexPath());
    if (snapshot.open(QIODevice::ReadOnly)) {
        const QJsonObject object = QJsonDocument::fromJson(snapshot.readAll()).object();
        snapshotText = QStringLiteral("已保存快照：%1 项 · %2")
            .arg(object.value(QStringLiteral("items")).toArray().size())
            .arg(object.value(QStringLiteral("generatedAt")).toString()
                .left(19).replace(QLatin1Char('T'), QLatin1Char(' ')));
    }
    auto *snapshotLabel = new QLabel(snapshotText, updateTab);
    snapshotLabel->setWordWrap(true);
    updateLayout->addWidget(snapshotLabel);
    auto *saveHint = new QLabel(QStringLiteral(
        "索引器每次生成新快照后再原子替换旧文件；"
        "AI空间规则和设置单独持久化。关闭组件不会丢失已有索引。"), updateTab);
    saveHint->setWordWrap(true);
    saveHint->setProperty("settingsHint", true);
    updateLayout->addWidget(saveHint);
    auto *idleHint = new QLabel(QStringLiteral(
        "“快全量”和“增量”都不启动 OCR。"
        "“OCR”只补全已标记的扫描 PDF/图片，低优先级逐文件处理并支持暂停续扫。"), updateTab);
    idleHint->setWordWrap(true);
    idleHint->setProperty("settingsHint", true);
    updateLayout->addWidget(idleHint);
    updateLayout->addStretch(1);
    tabs->addTab(updateTab, QStringLiteral("更新策略"));

    auto *appearanceTab = new QWidget(tabs);
    auto *appearanceLayout = new QVBoxLayout(appearanceTab);
    appearanceLayout->setContentsMargins(18, 18, 18, 18);
    appearanceLayout->setSpacing(12);
    auto *appearanceTitle = new QLabel(QStringLiteral("界面主题"),
                                       appearanceTab);
    appearanceTitle->setProperty("settingsSectionTitle", true);
    appearanceLayout->addWidget(appearanceTitle);
    auto *appearanceHint = new QLabel(QStringLiteral(
        "选择AI空间的整体视觉风格。两种主题都使用纯色界面，清晰稳定、不额外占用 GPU。"),
        appearanceTab);
    appearanceHint->setProperty("settingsSubtitle", true);
    appearanceHint->setWordWrap(true);
    appearanceLayout->addWidget(appearanceHint);
    // Hidden combo keeps the save logic simple; the cards drive it.
    auto *themeMode = new QComboBox(appearanceTab);
    themeMode->addItem(QStringLiteral("浅色雅致"), 2);
    themeMode->addItem(QStringLiteral("深邃暗夜"), 1);
    themeMode->setCurrentIndex(qMax(0, themeMode->findData(m_themeMode == 2 ? 2 : 1)));
    themeMode->hide();
    auto *themeCards = new QHBoxLayout;
    themeCards->setSpacing(14);
    auto *themeGroup = new QButtonGroup(appearanceTab);
    themeGroup->setExclusive(true);
    auto makeThemeCard = [&](int mode, const QString &title,
                             const QColor &canvas, const QColor &card,
                             const QColor &accent, const QColor &ink) {
        const QSize previewSize(168, 96);
        const qreal dpr = devicePixelRatioF();
        QPixmap preview(previewSize * dpr);
        preview.setDevicePixelRatio(dpr);
        preview.fill(Qt::transparent);
        {
            QPainter p(&preview);
            p.setRenderHint(QPainter::Antialiasing);
            QPainterPath frame;
            frame.addRoundedRect(QRectF(0.5, 0.5, 167, 95), 9, 9);
            p.fillPath(frame, canvas);
            p.save();
            p.setClipPath(frame);
            p.fillRect(QRectF(0, 0, 168, 18), card);
            p.restore();
            p.setPen(Qt::NoPen);
            p.setBrush(accent);
            p.drawRoundedRect(QRectF(8, 5, 8, 8), 2.5, 2.5);
            QColor muted = ink; muted.setAlpha(90);
            p.setBrush(muted);
            p.drawRoundedRect(QRectF(20, 7, 34, 4), 2, 2);
            p.setBrush(card);
            p.drawRoundedRect(QRectF(8, 26, 48, 62), 6, 6);
            p.drawRoundedRect(QRectF(62, 26, 98, 62), 6, 6);
            p.setBrush(accent);
            p.drawRoundedRect(QRectF(70, 34, 56, 6), 3, 3);
            p.setBrush(muted);
            for (int i = 0; i < 3; ++i) {
                p.drawRoundedRect(QRectF(70, 48 + i * 12, 80 - i * 14, 5), 2.5, 2.5);
                p.drawRoundedRect(QRectF(14, 34 + i * 12, 34, 5), 2.5, 2.5);
            }
            p.setPen(QPen(QColor(0, 0, 0, 30), 1));
            p.setBrush(Qt::NoBrush);
            p.drawPath(frame);
        }
        auto *button = new QToolButton(appearanceTab);
        button->setProperty("themeCard", true);
        button->setCheckable(true);
        button->setChecked((m_themeMode == 2 ? 2 : 1) == mode);
        button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        button->setIcon(QIcon(preview));
        button->setIconSize(previewSize);
        button->setText(title);
        button->setCursor(Qt::PointingHandCursor);
        button->setMinimumSize(200, 150);
        themeGroup->addButton(button, mode);
        themeCards->addWidget(button);
        connect(button, &QToolButton::clicked, themeMode, [themeMode, mode] {
            themeMode->setCurrentIndex(themeMode->findData(mode));
        });
    };
    makeThemeCard(2, QStringLiteral("浅色雅致"),
                  QColor(QStringLiteral("#f4f6fa")), QColor(QStringLiteral("#ffffff")),
                  QColor(QStringLiteral("#2563eb")), QColor(QStringLiteral("#0f172a")));
    makeThemeCard(1, QStringLiteral("深邃暗夜"),
                  QColor(QStringLiteral("#0b1120")), QColor(QStringLiteral("#131c31")),
                  QColor(QStringLiteral("#3b82f6")), QColor(QStringLiteral("#f1f5f9")));
    themeCards->addStretch(1);
    appearanceLayout->addLayout(themeCards);
    auto *appearanceDetail = new QLabel(QStringLiteral(
        "提示：标题栏上的太阳/月亮按钮可随时一键切换主题；保存设置后下次启动沿用当前主题。"),
        appearanceTab);
    appearanceDetail->setProperty("settingsHint", true);
    appearanceDetail->setWordWrap(true);
    appearanceLayout->addWidget(appearanceDetail);
    appearanceLayout->addStretch(1);
    tabs->addTab(appearanceTab, QStringLiteral("外观"));
    auto *knowledgeTab = new QWidget(tabs);
    auto *knowledgeLayout = new QVBoxLayout(knowledgeTab);
    knowledgeLayout->setContentsMargins(14, 14, 14, 14);
    knowledgeLayout->setSpacing(10);
    auto *managerButton=new QPushButton(QStringLiteral("存储设置 / 导入导出 / 分阶段建库…"),knowledgeTab);
    knowledgeLayout->addWidget(managerButton);connect(managerButton,&QPushButton::clicked,this,&SmartSpaceWidget::showKnowledgeManager);
    auto *knowledgePathRow = new QHBoxLayout;
    auto *knowledgePathEdit = new QLineEdit(
        m_knowledgeDirectory, knowledgeTab);
    auto *browseKnowledge = new QPushButton(
        QStringLiteral("选择位置…"), knowledgeTab);
    knowledgePathRow->addWidget(
        new QLabel(QStringLiteral("保存位置："), knowledgeTab));
    knowledgePathRow->addWidget(knowledgePathEdit, 1);
    knowledgePathRow->addWidget(browseKnowledge);
    knowledgeLayout->addLayout(knowledgePathRow);
    knowledgePathEdit->setReadOnly(true);
    browseKnowledge->setText(QStringLiteral("存储向导…"));
    connect(browseKnowledge,&QPushButton::clicked,&dialog,[this,knowledgePathEdit]{showKnowledgeManager();knowledgePathEdit->setText(m_knowledgeDirectory);});
    auto *chunkRow = new QHBoxLayout;
    auto *knowledgeChunkSize = new QSpinBox(knowledgeTab);
    knowledgeChunkSize->setRange(600, 3000);
    knowledgeChunkSize->setSingleStep(100);
    knowledgeChunkSize->setValue(m_knowledgeChunkSize);
    knowledgeChunkSize->setSuffix(QStringLiteral(" 字"));
    auto *knowledgeOverlap = new QSpinBox(knowledgeTab);
    knowledgeOverlap->setRange(0, 600);
    knowledgeOverlap->setSingleStep(20);
    knowledgeOverlap->setValue(m_knowledgeOverlap);
    knowledgeOverlap->setSuffix(QStringLiteral(" 字"));
    chunkRow->addWidget(new QLabel(QStringLiteral("片段长度："), knowledgeTab));
    chunkRow->addWidget(knowledgeChunkSize);
    chunkRow->addSpacing(18);
    chunkRow->addWidget(new QLabel(QStringLiteral("上下文重叠："), knowledgeTab));
    chunkRow->addWidget(knowledgeOverlap);
    chunkRow->addStretch(1);
    knowledgeLayout->addLayout(chunkRow);
    connect(knowledgeChunkSize, QOverload<int>::of(&QSpinBox::valueChanged),
            knowledgeOverlap, [knowledgeOverlap](int value) {
        knowledgeOverlap->setMaximum(value / 3);
    });
    auto *buildKnowledgeNow = new QCheckBox(
        QStringLiteral("保存后立即快速建库（仅本地处理，可断点重跑）"),
        knowledgeTab);
    knowledgeLayout->addWidget(buildKnowledgeNow);
    QString knowledgeStatus = QStringLiteral("尚未建立知识库");
    const QString existingKnowledgeDb = knowledgeDbPath();
    if (QFileInfo::exists(existingKnowledgeDb)) {
        knowledgeStatus = QStringLiteral("现有知识库：%1 MB · %2")
            .arg(QFileInfo(existingKnowledgeDb).size()
                 / 1024.0 / 1024.0, 0, 'f', 1)
            .arg(existingKnowledgeDb);
    }
    auto *knowledgeHint = new QLabel(
        knowledgeStatus + QStringLiteral(
            "\n快速建库不会调用网络，也不会修改 OCR/全文索引；"
            "仅保存本地正文片段和检索元数据，暂停后会从未完成处继续。"),
        knowledgeTab);
    knowledgeHint->setWordWrap(true);
    knowledgeHint->setProperty("settingsHint", true);
    knowledgeLayout->addWidget(knowledgeHint);
    knowledgeLayout->addStretch(1);
    tabs->addTab(knowledgeTab, QStringLiteral("知识库"));

    auto *skillTab = new QWidget(tabs);
    auto *skillLayout = new QVBoxLayout(skillTab);
    skillLayout->setContentsMargins(14, 14, 14, 14);
    skillLayout->setSpacing(10);
    auto *skillTitle = new QLabel(QStringLiteral(
        "将本地索引能力提供给 Codex、Antigravity 等 Agent"), skillTab);
    skillTitle->setProperty("settingsSectionTitle", true);
    skillLayout->addWidget(skillTitle);
    auto *skillHint = new QLabel(QStringLiteral(
        "Skill 只负责调用本地索引和组织研究流程，模型由外部 Agent 自行决定。"
        "复制路径可直接配置其他 Agent；导出会把完整 Skill 目录复制到指定位置。"),
        skillTab);
    skillHint->setWordWrap(true);
    skillHint->setProperty("settingsHint", true);
    skillLayout->addWidget(skillHint);
    auto *skillPath = new QLineEdit(skillDirectoryPath(), skillTab);
    skillPath->setReadOnly(true);
    skillPath->setToolTip(skillPath->text());
    auto *copySkillPath = new QPushButton(QStringLiteral("复制 Skill 路径"), skillTab);
    auto *exportSkill = new QPushButton(QStringLiteral("导出 Skill"), skillTab);
    auto *skillActionRow = new QHBoxLayout;
    skillActionRow->addWidget(skillPath, 1);
    skillActionRow->addWidget(copySkillPath);
    skillActionRow->addWidget(exportSkill);
    skillLayout->addLayout(skillActionRow);
    auto *skillStatus = new QLabel(skillTab);
    skillStatus->setWordWrap(true);
    skillStatus->setProperty("settingsSubtitle", true);
    skillLayout->addWidget(skillStatus);
    skillLayout->addStretch(1);
    connect(copySkillPath, &QPushButton::clicked, &dialog,
            [skillPath, skillStatus] {
        const QString path = skillPath->text().trimmed();
        if (path.isEmpty() || !QDir(path).exists()) {
            skillStatus->setText(QStringLiteral("Skill 目录不存在，请先完成索引 Skill 安装。"));
            return;
        }
        QApplication::clipboard()->setText(path);
        skillStatus->setText(QStringLiteral("Skill 路径已复制到剪贴板。"));
    });
    connect(exportSkill, &QPushButton::clicked, &dialog,
            [this, &dialog, skillPath, skillStatus] {
        const QString source = skillPath->text().trimmed();
        if (source.isEmpty() || !QDir(source).exists()) {
            skillStatus->setText(QStringLiteral("Skill 目录不存在，无法导出。"));
            return;
        }
        const QString destination = QFileDialog::getExistingDirectory(
            dialog.window(), QStringLiteral("选择 Skill 导出目录"));
        if (destination.isEmpty())
            return;
        const QString target = QDir(destination).filePath(
            QStringLiteral("ai-space-index-query"));
        if (QDir::cleanPath(source) == QDir::cleanPath(target)) {
            skillStatus->setText(QStringLiteral("目标目录就是当前 Skill，无需导出。"));
            return;
        }
        if (QDir(target).exists()) {
            const auto answer = smartQuestion(
                &dialog, QStringLiteral("确认覆盖"),
                QStringLiteral("目标目录已存在，是否覆盖其中同名文件？"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (answer != QMessageBox::Yes)
                return;
        }
        QString error;
        if (!copyDirectoryTree(source, target, &error)) {
            skillStatus->setText(QStringLiteral("Skill 导出失败：%1").arg(error));
            return;
        }
        const QString runtime=QDir(target).filePath(QStringLiteral("scripts/knowledge_library.py"));
        if(QFileInfo::exists(runtime))QFile::remove(runtime);
        if(!QFile::copy(QFileInfo(knowledgeWorkerPath()).dir().filePath(QStringLiteral("knowledge_library.py")),runtime)) {
            skillStatus->setText(QStringLiteral("无法导出独立检索运行时，请检查目标目录权限"));return;
        }
        skillStatus->setText(QStringLiteral("Skill 已导出到：%1").arg(target));
    });
    tabs->addTab(skillTab, QStringLiteral("Skill"));

    auto *providerTab = new QWidget(tabs);
    auto *providerLayout = new QVBoxLayout(providerTab);
    providerLayout->setContentsMargins(14, 14, 14, 14);
    providerLayout->setSpacing(10);
    auto *providerRow = new QHBoxLayout;
    auto *providerEdit = new QLineEdit(m_providerConfig, providerTab);
    providerEdit->setPlaceholderText(QStringLiteral("可选：Command / HTTP / D-Bus Provider JSON"));
    auto *browseProvider = new QPushButton(QStringLiteral("选择…"), providerTab);
    auto *editProvider = new QPushButton(QStringLiteral("编辑 JSON…"), providerTab);
    providerRow->addWidget(new QLabel(QStringLiteral("外部数据源："), providerTab));
    providerRow->addWidget(providerEdit, 1);
    providerRow->addWidget(browseProvider);
    providerRow->addWidget(editProvider);
    providerLayout->addLayout(providerRow);
    auto *hint = new QLabel(
        QStringLiteral("支持：PDF、DOCX、PPTX、XLSX、ODF、文本、图片 OCR；"
                       "ET/XLS 优先只读解析，DOC/PPT/WPS/DPS 使用兼容文本提取。"),
        providerTab);
    hint->setWordWrap(true);
    hint->setProperty("settingsHint", true);
    providerLayout->addWidget(hint);
    providerLayout->addStretch(1);
    tabs->addTab(providerTab, QStringLiteral("数据源"));

    connect(addRoot, &QPushButton::clicked, &dialog, [&dialog, rootList] {
        const QString path = QFileDialog::getExistingDirectory(
            dialog.window(), QStringLiteral("选择索引目录"));
        if (!path.isEmpty()) {
            const QList<QListWidgetItem *> found = rootList->findItems(path, Qt::MatchExactly);
            if (found.isEmpty())
                rootList->addItem(path);
        }
    });
    connect(removeRoot, &QPushButton::clicked, &dialog, [rootList] {
        qDeleteAll(rootList->selectedItems());
    });
    connect(addExclude, &QPushButton::clicked, &dialog,
            [&dialog, rootList, excludeList] {
        const QString path = QFileDialog::getExistingDirectory(
            dialog.window(), QStringLiteral("选择要排除的文件夹"));
        if (path.isEmpty())
            return;
        const QString clean = normalizedPath(path);
        bool insideRoot = false;
        bool equalsRoot = false;
        for (int row = 0; row < rootList->count(); ++row) {
            const QString root = normalizedPath(rootList->item(row)->text());
            insideRoot = insideRoot || pathIsInside(clean, root);
            equalsRoot = equalsRoot || clean == root;
        }
        if (!insideRoot || equalsRoot) {
            smartInformation(&dialog, QStringLiteral("不能添加"),
                equalsRoot
                    ? QStringLiteral("如需取消整个根目录，请从“索引目录”中移除。")
                    : QStringLiteral("排除文件夹必须位于已配置的索引目录内。"));
            return;
        }
        if (excludeList->findItems(clean, Qt::MatchExactly).isEmpty())
            excludeList->addItem(clean);
    });
    connect(restoreExclude, &QPushButton::clicked, &dialog, [excludeList] {
        qDeleteAll(excludeList->selectedItems());
    });
    connect(restoreHidden, &QPushButton::clicked, &dialog, [hiddenList] {
        qDeleteAll(hiddenList->selectedItems());
    });
    auto setFormatChecks = [](QListWidget *list, Qt::CheckState state) {
        for (int row = 0; row < list->count(); ++row)
            list->item(row)->setCheckState(state);
    };
    connect(indexSelectAll, &QPushButton::clicked, &dialog,
            [indexFormatList, setFormatChecks] {
        setFormatChecks(indexFormatList, Qt::Checked);
    });
    connect(indexClear, &QPushButton::clicked, &dialog,
            [indexFormatList, setFormatChecks] {
        setFormatChecks(indexFormatList, Qt::Unchecked);
    });
    connect(indexSelectCommon, &QPushButton::clicked, &dialog,
            [indexFormatList] {
        const QSet<QString> common{
            QStringLiteral("pdf"), QStringLiteral("docx"), QStringLiteral("doc"),
            QStringLiteral("wps"), QStringLiteral("pptx"), QStringLiteral("ppt"),
            QStringLiteral("dps"), QStringLiteral("xlsx"), QStringLiteral("xls"),
            QStringLiteral("et"), QStringLiteral("txt"), QStringLiteral("md"),
            QStringLiteral("csv"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
            QStringLiteral("png")
        };
        for (int row = 0; row < indexFormatList->count(); ++row) {
            QListWidgetItem *item = indexFormatList->item(row);
            item->setCheckState(common.contains(item->data(Qt::UserRole).toString())
                ? Qt::Checked : Qt::Unchecked);
        }
    });
    connect(visibleSelectAll, &QPushButton::clicked, &dialog,
            [visibleFormatList, setFormatChecks] {
        setFormatChecks(visibleFormatList, Qt::Checked);
    });
    connect(visibleClear, &QPushButton::clicked, &dialog,
            [visibleFormatList, setFormatChecks] {
        setFormatChecks(visibleFormatList, Qt::Unchecked);
    });
    connect(visibleSelectIndexed, &QPushButton::clicked, &dialog,
            [indexFormatList, visibleFormatList] {
        for (int row = 0; row < visibleFormatList->count(); ++row)
            visibleFormatList->item(row)->setCheckState(
                indexFormatList->item(row)->checkState());
    });
    connect(browseProvider, &QPushButton::clicked, &dialog, [&dialog, providerEdit] {
        const QString path = QFileDialog::getOpenFileName(
            dialog.window(), QStringLiteral("选择 Provider 配置"),
            QFileInfo(providerEdit->text()).absolutePath(), QStringLiteral("JSON (*.json)"));
        if (!path.isEmpty()) providerEdit->setText(path);
    });
    connect(editProvider, &QPushButton::clicked, &dialog, [this, &dialog, providerEdit] {
        QString path = providerEdit->text().trimmed();
        if (path.isEmpty()) path = providerConfigPath();
        QFile source(path);
        QByteArray value;
        if (source.open(QIODevice::ReadOnly)) value = source.readAll();
        if (value.trimmed().isEmpty()) {
            value = QByteArray(R"({
  "providers": [
    {
      "name": "本地命令示例",
      "enabled": false,
      "type": "command",
      "program": "/path/to/provider",
      "arguments": [],
      "timeout": 30,
      "limit": 5000
    },
    {
      "name": "HTTP API 示例",
      "enabled": false,
      "type": "http",
      "url": "http://127.0.0.1:8080/search",
      "tokenEnv": "UKUI_FENCES_PROVIDER_TOKEN"
    },
    {
      "name": "D-Bus 示例",
      "enabled": false,
      "type": "dbus",
      "service": "org.example.Search",
      "objectPath": "/org/example/Search",
      "method": "org.example.Search.queryJson"
    }
  ]
})");
        }
        QDialog editor(&dialog);
        editor.setWindowTitle(QStringLiteral("Provider JSON 配置"));
        editor.resize(720, 540);
        auto *editorLayout = new QVBoxLayout(&editor);
        auto *textEdit = new QPlainTextEdit(QString::fromUtf8(value), &editor);
        textEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
        editorLayout->addWidget(textEdit, 1);
        auto *editorButtons = new QDialogButtonBox(
            QDialogButtonBox::Save | QDialogButtonBox::Cancel, &editor);
        editorLayout->addWidget(editorButtons);
        connect(editorButtons, &QDialogButtonBox::rejected, &editor, &QDialog::reject);
        connect(editorButtons, &QDialogButtonBox::accepted, &editor,
                [&editor, textEdit, path, providerEdit] {
            QJsonParseError parseError;
            const QByteArray bytes = textEdit->toPlainText().toUtf8();
            const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                smartWarning(&editor, QStringLiteral("JSON 无效"),
                                     parseError.errorString());
                return;
            }
            QDir().mkpath(QFileInfo(path).absolutePath());
            QSaveFile output(path);
            if (!output.open(QIODevice::WriteOnly) ||
                output.write(document.toJson(QJsonDocument::Indented)) < 0 ||
                !output.commit()) {
                smartWarning(&editor, QStringLiteral("保存失败"), path);
                return;
            }
            providerEdit->setText(path);
            editor.accept();
        });
        editor.exec();
    });

    connect(saveButton, &QPushButton::clicked, &dialog, [=, &dialog] {
    if(m_libraryJobBusy){smartInformation(this,QStringLiteral("任务正在运行"),QStringLiteral("请先暂停建库，再保存资料和索引设置。"));return;}
    QStringList roots;
    for (int i = 0; i < rootList->count(); ++i) {
        const QString path = rootList->item(i)->text();
        if (QDir(path).exists() && !roots.contains(path))
            roots << path;
    }
    if (roots.isEmpty()) {
        smartInformation(this, QStringLiteral("AI空间"),
                                 QStringLiteral("至少需要保留一个可访问的索引目录。"));
        return;
    }
    QStringList excludedFolders;
    for (int row = 0; row < excludeList->count(); ++row) {
        const QString path = normalizedPath(excludeList->item(row)->text());
        bool valid = false;
        for (const QString &root : roots) {
            if (path != normalizedPath(root) && pathIsInside(path, root)) {
                valid = true;
                break;
            }
        }
        if (valid && !excludedFolders.contains(path))
            excludedFolders << path;
    }
    std::sort(excludedFolders.begin(), excludedFolders.end(),
              [](const QString &left, const QString &right) {
        return left.size() < right.size();
    });
    QStringList compactExcluded;
    for (const QString &path : excludedFolders) {
        bool alreadyCovered = false;
        for (const QString &parent : compactExcluded)
            alreadyCovered = alreadyCovered || pathIsInside(path, parent);
        if (!alreadyCovered)
            compactExcluded << path;
    }
    excludedFolders = compactExcluded;
    QStringList hiddenFolders;
    for (int row = 0; row < hiddenList->count(); ++row) {
        const QString path = normalizedPath(hiddenList->item(row)->text());
        if (!path.isEmpty() && !hiddenFolders.contains(path))
            hiddenFolders << path;
    }
    auto checkedFormats = [](QListWidget *list, bool *noneSelected) {
        QStringList result;
        for (int row = 0; row < list->count(); ++row) {
            QListWidgetItem *item = list->item(row);
            if (item->checkState() == Qt::Checked)
                result << item->data(Qt::UserRole).toString();
        }
        *noneSelected = result.isEmpty();
        if (result.size() == list->count())
            result.clear(); // Empty is the backward-compatible "all formats" value.
        return result;
    };
    bool noIndexFormats = false;
    bool noVisibleFormats = false;
    const QStringList indexedExtensions = checkedFormats(
        indexFormatList, &noIndexFormats);
    const QStringList visibleExtensions = checkedFormats(
        visibleFormatList, &noVisibleFormats);
    if (noIndexFormats || noVisibleFormats) {
        smartInformation(this, QStringLiteral("AI空间"),
            noIndexFormats
                ? QStringLiteral("索引格式至少需要保留一项。")
                : QStringLiteral("匹配文件显示格式至少需要保留一项。"));
        return;
    }
    const bool excludeChanged = excludedFolders != m_excludedFolders;
    const bool hiddenChanged = hiddenFolders != m_hiddenFolders;
    const bool visibleFormatsChanged = visibleExtensions != m_visibleExtensions;
    const bool indexInputsChanged = roots != m_roots ||
        excludeChanged || indexedExtensions != m_indexedExtensions ||
        providerEdit->text().trimmed() != m_providerConfig;
    m_roots = roots;
    m_excludedFolders = excludedFolders;
    m_hiddenFolders = hiddenFolders;
    m_indexedExtensions = indexedExtensions;
    m_visibleExtensions = visibleExtensions;
    m_indexMode = indexMode->currentData().toInt();
    m_indexIntervalMinutes = interval->value();
    m_themeMode = themeMode->currentData().toInt();

    m_customOpacity = 100;
    m_customColorsEnabled = false;
    setAutoStart(autoStart->isChecked());
    m_knowledgeChunkSize = knowledgeChunkSize->value();
    m_knowledgeOverlap = qMin(
        knowledgeOverlap->value(), m_knowledgeChunkSize / 3);
    if(m_providerConfig!=providerEdit->text().trimmed())m_providerApprovedDigest.clear();
    m_providerConfig = providerEdit->text().trimmed();
    if(m_libraryJobBusy)stopIdleFullIndex();
    m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(),
        [this](const SmartSpaceEntry &entry) { return pathExcluded(entry.path); }),
        m_entries.end());
    configureIndexScheduling();
    applyTheme();
    saveSettings();
    if (syncKylin->isChecked()) {
        QDBusInterface indexService(
            QStringLiteral("com.ukui.search.fileindex.service"),
            QStringLiteral("/org/ukui/search/fileindex"),
            QStringLiteral("org.ukui.search.fileindex"),
            QDBusConnection::sessionBus());
        if (indexService.isValid()) {
            for (const QString &root : m_roots)
                indexService.asyncCall(QStringLiteral("appendSearchDir"), root);
        }
    }
    rebuildWatches();
    if (excludeChanged || visibleFormatsChanged || hiddenChanged)
        updateResults(true);
    if (indexNow->isChecked()) {
        QTimer::singleShot(0,this,[this]{setSettingsViewVisible(false);startIndexing();});
    } else if (indexInputsChanged) {
        m_statusLabel->setText(QStringLiteral(
            "索引设置已保存 · 未自动扫描，点击底部“增量”后生效"));
    }
    if (buildKnowledgeNow->isChecked())
        QTimer::singleShot(0,this,[this]{setSettingsViewVisible(false);startKnowledgeBuild();});
    indexNow->setChecked(false);
    buildKnowledgeNow->setChecked(false);
    dialog.setProperty("settingsDirty", false);
    QTimer::singleShot(0, this, [this] { setSettingsViewVisible(false); });
    });
    styleModernCombos(form);
    return form;
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
    QPainterPath path;path.addRoundedRect(QRectF(rect()).adjusted(.5,.5,-.5,-.5),radius,radius);
    const QColor shell=isActiveWindow()?m_surfaceColor:QColor(m_themeMode==2?"#f8fafc":"#0f172a");
    painter.fillPath(path,shell);
    if (!maxed) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(m_borderColor,1));
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
    QAction *pin = menu.addAction(
        m_alwaysOnTop ? QStringLiteral("取消置顶") : QStringLiteral("置顶"));
    menu.addSeparator();
    QAction *refresh = menu.addAction(smartMenuIcon(QStringLiteral("refresh")),QStringLiteral("增量更新 · 扫描并复用正文"));
    QAction *settings = menu.addAction(smartMenuIcon(QStringLiteral("settings")),
                                       QStringLiteral("AI空间设置"));
    QAction *idleIndex = menu.addAction(smartMenuIcon(QStringLiteral("fast-full")),
        QStringLiteral("① 全量扫描"));
    QAction *textIndex=menu.addAction(smartMenuIcon(QStringLiteral("refresh")),QStringLiteral("② 文本建库"));textIndex->setEnabled(!m_libraryJobBusy);
    QAction *ocrIndex = menu.addAction(smartMenuIcon(QStringLiteral("ocr")),
        QStringLiteral("③ OCR补全"));
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
        startGuidedBuild(false,false,QStringLiteral("manifest"));
    else if (selected == textIndex)
        startGuidedBuild(false,false,QStringLiteral("text"));
    else if (selected == ocrIndex)
        startOcrIndex();
}

bool SmartSpaceWidget::eventFilter(QObject *watched, QEvent *event)
{
    if(watched==m_statusLabel && event->type()==QEvent::MouseButtonRelease && static_cast<QMouseEvent *>(event)->button()==Qt::LeftButton){showKnowledgeStatus();return true;}
    if(watched==m_statusLabel && event->type()==QEvent::KeyPress){auto *key=static_cast<QKeyEvent *>(event);if(key->key()==Qt::Key_Return || key->key()==Qt::Key_Space){showKnowledgeStatus();return true;}}
    auto *title = qobject_cast<QLabel *>(watched);
    if (title && title->property("cardName").toBool()) {
        if (event->type() == QEvent::Leave) {
            QToolTip::hideText();
        } else if (event->type() == QEvent::ToolTip ||
                   event->type() == QEvent::MouseMove) {
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
        if ((event->type() == QEvent::Enter || event->type() == QEvent::ToolTip)
            && m_indexProgress && m_indexProgress->isVisible()) {
            QToolTip::showText(
                m_indexProgress->mapToGlobal(
                    QPoint(m_indexProgress->width() / 2,
                           m_indexProgress->height() + 4)),
                m_indexProgress->toolTip(), m_indexProgress);
            return event->type() == QEvent::ToolTip;
        }
        if (event->type() == QEvent::Leave)
            QToolTip::hideText();
    }
    return QWidget::eventFilter(watched, event);
}

namespace {
constexpr int kResizeLeft = 1;
constexpr int kResizeRight = 2;
constexpr int kResizeTop = 4;
constexpr int kResizeBottom = 8;
constexpr int kResizeBand = 6;
}

int SmartSpaceWidget::hitResizeEdges(const QPoint &pos) const
{
    if (isWindowMaximized() || !rect().contains(pos))
        return 0;
    int edges = 0;
    if (pos.x() <= kResizeBand) edges |= kResizeLeft;
    else if (pos.x() >= width() - kResizeBand - 1) edges |= kResizeRight;
    if (pos.y() <= kResizeBand - 2) edges |= kResizeTop;
    else if (pos.y() >= height() - kResizeBand - 1) edges |= kResizeBottom;
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
    if (event->button() == Qt::LeftButton &&
        insideTitleBar(this, m_actionRail, event->pos()) &&
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
            ? m_normalGeometry : QRect(pos(), QSize(kDefaultWidth, kDefaultHeight));
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
    if (auto *bar=qobject_cast<AiSpace::WindowTitleBarWidget *>(m_actionRail))
        bar->setAppearance(m_themeMode==2,isActiveWindow(),isWindowMaximized());
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
bool SmartSpaceWidget::event(QEvent *event) {
    const bool repaint=event->type()==QEvent::WindowActivate || event->type()==QEvent::WindowDeactivate;
    const bool result=QWidget::event(event);
    if (event->type()==QEvent::WindowStateChange) {
        const bool nativeMaximized=isMaximized();
        if (nativeMaximized && !m_isMaximized) {
            m_normalGeometry=normalGeometry();
            m_isMaximized=true;
        } else if (!nativeMaximized && !isMinimized()) m_isMaximized=false;
        updateWindowStateButtons();updateRoundedMask();
    }
    if (repaint && m_actionRail) {updateWindowStateButtons();update();}
    return result;
}
