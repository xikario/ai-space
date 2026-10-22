#include <QScreen>
#include "AiSpaceController.h"
#include "AppConfig.h"
#include "SmartSpaceWidget.h"
#include "Migration.h"
#include "BackgroundTask.h"
#include <QApplication>
#include <QPushButton>
#include <memory>
#include <QDialog>
#include <QMessageBox>
#include <QSystemTrayIcon>
#include <QLabel>
#include <QVBoxLayout>
#include <QFileInfo>
#include <QMenu>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSettings>
#include <QFileSystemWatcher>
#include <QStandardPaths>
#include <QDir>
#include <QSaveFile>
#include <QTimer>
#include <QSignalBlocker>

static void showControllerWarning(QWidget *parent, const QString &title, const QString &text)
{
    QMessageBox box(QMessageBox::Warning, title, text, QMessageBox::Ok, parent);
    box.setWindowTitle(title);
    box.setStyleSheet(QStringLiteral(
        "QMessageBox { background: #ffffff; color: #0f172a; }"
        "QMessageBox QLabel { color: #0f172a; font-size: 13px; }"
        "QMessageBox QPushButton { background: #2563eb; color: #ffffff; border: none;"
        " border-radius: 6px; padding: 6px 18px; min-width: 64px; font-weight: 600; }"));
    box.exec();
}

static bool writeAutoStart(bool enabled)
{
    const QString path = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) +
                         "/autostart/ai-space.desktop";
    if (!enabled)
        return !QFileInfo::exists(path) || QFile::remove(path);
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    // Exec quoting, then Desktop Entry string escaping. No shell is invoked.
    QString executable = QCoreApplication::applicationFilePath();
    executable.replace('\\', "\\\\").replace('"', "\\\"").replace('`', "\\`").replace('$', "\\$");
    executable.replace('\\', "\\\\").replace('%', "%%");
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    const auto entry = ("[Desktop Entry]\nType=Application\nName=AI空间\nExec=\"" + executable +
                        "\" --autostart\nIcon=ai-space\nTerminal=false\n")
                           .toUtf8();
    return file.write(entry) == entry.size() && file.commit();
}

AiSpaceController::AiSpaceController(QObject *parent) : QObject(parent)
{
}
AiSpaceController::~AiSpaceController()
{
    delete m_widget.data();
    delete m_bootstrap.data();
}
void AiSpaceController::initialize(bool visible, bool settings)
{
    m_startup.start();
    m_requestedVisible = visible;
    if (visible && !QSettings().value("migration/legacyV1", false).toBool() &&
        !QSettings().value("migration/legacyPreferencesV1", false).toBool() &&
        !qEnvironmentVariableIsSet("AI_SPACE_TEST_NO_BOOTSTRAP")) {
        auto *dialog = new QDialog;
        dialog->setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
        dialog->setWindowTitle(QStringLiteral("AI空间"));
        dialog->setWindowIcon(qApp->windowIcon());
        dialog->setStyleSheet(QStringLiteral(
            "QDialog { background: #ffffff; border: 1px solid #cbd5e1; border-radius: 12px; }"
            "QLabel { color: #0f172a; font-size: 14px; font-weight: 500; }"));
        auto *layout = new QVBoxLayout(dialog);
        layout->setContentsMargins(24, 24, 24, 24);
        layout->addWidget(new QLabel(QStringLiteral("正在导入界面偏好与文件快照…"), dialog));
        dialog->resize(460, 140);
        dialog->show();
        m_bootstrap = dialog;
        // Esc only dismisses this temporary progress window; migration continues.
    }
    if (settings)
        m_pending.append(
            [](SmartSpaceWidget *w) { QTimer::singleShot(0, w, &SmartSpaceWidget::showSettings); });
    BackgroundTask::run(
        this, [] { return migrateLegacyData(false); },
        [this](const MigrationResult &result) {
            auto *widget = new SmartSpaceWidget;
            m_widget = widget;
            // Production never duplicates a multi-GB legacy library at login.
            // Legacy-copy fault tests explicitly opt in with a disposable HOME.
            const bool testCopy = qEnvironmentVariableIntValue("AI_SPACE_TEST_LEGACY_COPY") == 1 &&
                                  qEnvironmentVariableIsSet("AI_SPACE_TEST_NO_BOOTSTRAP");
            m_migrationPending = result.knowledgePending && testCopy;
            widget->setKnowledgeMigrationPending(m_migrationPending);
            connect(widget, &SmartSpaceWidget::indexLoaded, this, [this] {
                m_indexReadyMs = m_startup.elapsed();
                notify();
            });
            widget->setWindowTitle(QStringLiteral("AI空间"));
            widget->setWindowIcon(qApp->windowIcon());
            connect(widget, &SmartSpaceWidget::closeRequested, this,
                    &AiSpaceController::requestClose);
            connect(widget, &SmartSpaceWidget::alwaysOnTopChanged, this, [this] {
                applyWindowMode();
                notify();
            });
            connect(widget, &SmartSpaceWidget::geometryChanged, this, &AiSpaceController::notify);
            connect(widget, &SmartSpaceWidget::autoStartChanged, this, [this, widget](bool on) {
                if (!writeAutoStart(on)) {
                    const QSignalBlocker blocker(widget);
                    widget->setAutoStart(!on);
                    QTimer::singleShot(0, widget, [widget] {
                        showControllerWarning(
                            widget, QStringLiteral("登录启动"),
                            QStringLiteral("无法更新登录启动文件，请检查用户配置目录的权限。"));
                    });
                }
                notify();
            });
            connect(widget, &SmartSpaceWidget::busyChanged, this, &AiSpaceController::notify);
            applyWindowMode();
            if (m_bootstrap) {
                m_bootstrap->hide();
                m_bootstrap->deleteLater();
            }
            if (m_requestedVisible)
                showWindow();
            else
                widget->showMinimized();
            m_uiReadyMs = m_startup.elapsed();
            for (auto &action : m_pending)
                action(widget);
            m_pending.clear();
            m_tray = new QSystemTrayIcon(qApp->windowIcon(), this);
            auto *menu = new QMenu(widget);
            menu->addAction(QStringLiteral("打开AI空间"), this, &AiSpaceController::showWindow);
            menu->addAction(QStringLiteral("设置"), this, &AiSpaceController::showSettings);
            menu->addSeparator();
            menu->addAction(QStringLiteral("退出AI空间"), this, &AiSpaceController::quit);
            m_tray->setContextMenu(menu);
            m_tray->setToolTip(QStringLiteral("AI空间"));
            m_tray->show();
            connect(m_tray, &QSystemTrayIcon::activated, this,
                    [this](QSystemTrayIcon::ActivationReason r) {
                        if (r == QSystemTrayIcon::Trigger)
                            showWindow();
                    });
            if (!result.ok)
                QTimer::singleShot(0, widget, [widget, error = result.error] {
                    showControllerWarning(widget, QStringLiteral("导入旧数据"), error);
                });
            notify();
            if (result.ok && m_migrationPending) {
                BackgroundTask::run(
                    this, [] { return migrateLegacyData(); },
                    [this](const MigrationResult &migration) {
                        m_migrationPending = false;
                        if (m_widget)
                            m_widget->setKnowledgeMigrationPending(false);
                        notify();
                        if (!migration.ok && m_widget) {
                            const auto widget = m_widget;
                            QTimer::singleShot(0, widget, [widget, error = migration.error] {
                                if (widget)
                                    showControllerWarning(widget, QStringLiteral("导入旧知识库"),
                                                          error);
                            });
                        }
                    });
            }
        });
}
void AiSpaceController::showKnowledgeStatus()
{
    perform([](SmartSpaceWidget *widget) {
        QTimer::singleShot(0, widget, &SmartSpaceWidget::showKnowledgeStatus);
    });
}
void AiSpaceController::showKnowledgeManager()
{
    perform([](SmartSpaceWidget *widget) {
        QTimer::singleShot(0, widget, &SmartSpaceWidget::showKnowledgeManager);
    });
}
void AiSpaceController::perform(std::function<void(SmartSpaceWidget *)> action)
{
    if (m_widget)
        action(m_widget);
    else if (m_pending.size() < 64)
        m_pending.append(std::move(action));
    else
        qWarning("AI Space startup action queue is full; request rejected");
}
void AiSpaceController::applyWindowMode()
{
    if (!m_widget)
        return;
    const bool visible = m_widget->isVisible();
    const bool minimized = m_widget->isMinimized();
    const QPoint p = m_widget->pos();
    m_widget->setWindowFlags(
        Qt::Window | Qt::FramelessWindowHint | Qt::WindowMinimizeButtonHint |
        Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint |
        (m_widget->alwaysOnTop() ? Qt::WindowStaysOnTopHint : Qt::WindowFlags()));
    m_widget->recreateNativeSurface(true);
    m_widget->move(p);
    if (visible) {
        if (minimized)
            m_widget->showMinimized();
        else
            m_widget->show();
    }
}
void AiSpaceController::notify()
{
    emit stateChanged(status());
}
QString AiSpaceController::status() const
{
    QJsonObject o{{"application", "ai-space"},
                  {"version", QStringLiteral(AI_SPACE_VERSION)},
                  {"statusVersion", 1},
                  {"ready", bool(m_widget)},
                  {"pid", QCoreApplication::applicationPid()}};
    o.insert("migrationPending", m_migrationPending);
    o.insert("uiReadyMs", m_uiReadyMs);
    o.insert("indexReadyMs", m_indexReadyMs);
    if (m_widget) {
        o.insert("autoStart", m_widget->autoStart());
        o.insert("visible", m_widget->isVisible() && !m_widget->isMinimized());
        o.insert("indexBusy", m_widget->indexBusy());
        o.insert("alwaysOnTop", m_widget->alwaysOnTop());
        o.insert("x", m_widget->x());
        o.insert("y", m_widget->y());
        o.insert("width", m_widget->width());
        o.insert("height", m_widget->height());
        o.insert("indexedEntries", m_widget->indexedEntryCount());
        o.insert("results", m_widget->resultCount());
        o.insert("navigationGroups", m_widget->property("navigationGroups").toInt());
        o.insert("windowId", QString::number(m_widget->internalWinId()));
        o.insert("previewVisible", m_widget->property("previewVisible").toBool());
        o.insert("previewDocked", m_widget->property("previewDocked").toBool());
        o.insert("indexLoading", m_widget->property("indexLoading").toBool());
        o.insert("indexApplyMs", m_widget->property("indexApplyMs").toInt());
        o.insert("maximizable", bool(m_widget->windowFlags() & Qt::WindowMaximizeButtonHint));
        o.insert("maximized", m_widget->isWindowMaximized());
        o.insert("minimized", m_widget->isMinimized());
        o.insert("knowledgeManagerVisible", m_widget->property("knowledgeManagerVisible").toBool());
        o.insert("knowledgeWizardStep", m_widget->property("knowledgeWizardStep").toInt());
        o.insert("knowledgeStatusVisible", m_widget->property("knowledgeStatusVisible").toBool());
        o.insert("knowledgeStatusLoaded", m_widget->property("knowledgeStatusLoaded").toBool());
#ifdef AI_SPACE_TEST_CONTROL
        if (auto *button =
                m_widget->findChild<QPushButton *>(QStringLiteral("knowledgePruneAbsent")))
            o.insert("knowledgePruneEnabled", button->isEnabled());
        if (auto *prune = m_widget->findChild<QWidget *>(QStringLiteral("knowledgePruneDialog"))) {
            o.insert("knowledgePruneVisible", prune->isVisible());
            if (auto *apply =
                    prune->findChild<QPushButton *>(QStringLiteral("knowledgePruneApplyButton")))
                o.insert("knowledgePruneReady", apply->isEnabled());
        }
#endif
        o.insert("knowledgeFailuresVisible",
                 m_widget->property("knowledgeFailuresVisible").toBool());
        if (auto *failures =
                m_widget->findChild<QWidget *>(QStringLiteral("knowledgeFailuresDialog"))) {
            o.insert("failureListLoaded", failures->property("failureListLoaded").toBool());
            o.insert("failureListIgnored", failures->property("failureListIgnored").toBool());
            o.insert("failureListCategory", failures->property("failureListCategory").toString());
            o.insert("failureListTotal", failures->property("failureListTotal").toInt());
            o.insert("failureListOffset", failures->property("failureListOffset").toInt());
            o.insert("failureSelectedCount", failures->property("failureSelectedCount").toInt());
            o.insert("failureAllSelected", failures->property("failureAllSelected").toBool());
        }
        o.insert("libraryPreviewLoaded", m_widget->property("libraryPreviewLoaded").toBool());
        o.insert("settingsVisible", m_widget->settingsViewVisible());
    }
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}
void AiSpaceController::showWindow()
{
    m_requestedVisible = true;
    if (m_widget) {
        if (m_widget->isMinimized()) {
            if (m_widget->isWindowMaximized())
                m_widget->showMaximized();
            else
                m_widget->showNormal();
        }
        if (m_widget->isWindowMaximized())
            m_widget->show();
        else
            m_widget->showNormal();
        m_widget->raise();
        m_widget->activateWindow();
        notify();
    }
}
// Deprecated hide entry points now use the normal taskbar minimize path.
void AiSpaceController::hideWindow()
{
    m_requestedVisible = false;
    if (m_widget)
        m_widget->minimizeWindow();
    notify();
}
void AiSpaceController::toggleWindow()
{
    if (m_widget && m_widget->isVisible() && !m_widget->isMinimized())
        hideWindow();
    else
        showWindow();
}
void AiSpaceController::showSettings()
{
    showWindow();
    perform([](SmartSpaceWidget *w) { QTimer::singleShot(0, w, &SmartSpaceWidget::showSettings); });
}
void AiSpaceController::requestClose()
{
    if (!m_widget || !m_widget->indexBusy()) {
        quit();
        return;
    }
    QMessageBox box(QMessageBox::Question, QStringLiteral("建库任务正在运行"),
                    QStringLiteral("可以保留后台任务，或先暂停并保存已完成内容后退出。"),
                    QMessageBox::NoButton, m_widget);
    box.setWindowIcon(qApp->windowIcon());
    auto *background = box.addButton(QStringLiteral("最小化并继续"), QMessageBox::AcceptRole);
    auto *stop = box.addButton(QStringLiteral("暂停并退出"), QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() == background)
        hideWindow();
    else if (box.clickedButton() == stop)
        quit();
}
void AiSpaceController::quit()
{
    if (m_quitting)
        return;
    if (!m_widget || !m_widget->indexBusy()) {
        qApp->quit();
        return;
    }
    m_quitting = true;
    m_widget->pauseKnowledgeWork();
    auto elapsed = std::make_shared<QElapsedTimer>();
    elapsed->start();
    auto *timer = new QTimer(this);
    timer->setInterval(100);
    connect(timer, &QTimer::timeout, this, [this, timer, elapsed] {
        if (!m_widget || !m_widget->indexBusy() || elapsed->elapsed() > 5000) {
            timer->stop();
            qApp->quit();
        }
    });
    timer->start();
}
void AiSpaceController::setAlwaysOnTop(bool v)
{
    perform([v](SmartSpaceWidget *w) { w->setAlwaysOnTop(v); });
}
void AiSpaceController::setAutoStart(bool v)
{
    perform([v](SmartSpaceWidget *w) { w->setAutoStart(v); });
}
void AiSpaceController::moveWindow(int x, int y)
{
    perform([=](SmartSpaceWidget *w) {
        const QRect bounds = QGuiApplication::primaryScreen()->availableGeometry();
        w->move(qBound(bounds.left() - w->width() + 80, x, bounds.right() - 80),
                qBound(bounds.top(), y, bounds.bottom() - 60));
    });
}
void AiSpaceController::resizeWindow(int x, int y)
{
    perform([=](SmartSpaceWidget *w) { w->resize(qBound(620, x, 2400), qBound(360, y, 1600)); });
}
void AiSpaceController::minimizeWindow()
{
    perform([this](SmartSpaceWidget *w) {
        w->minimizeWindow();
        notify();
    });
}
void AiSpaceController::maximizeWindow()
{
    perform([this](SmartSpaceWidget *w) {
        w->setWindowMaximized(true);
        notify();
    });
}
void AiSpaceController::restoreWindow()
{
    perform([this](SmartSpaceWidget *w) {
        w->setWindowMaximized(false);
        notify();
    });
}
void AiSpaceController::toggleMaximize()
{
    perform([this](SmartSpaceWidget *w) {
        w->toggleMaximized();
        notify();
    });
}
void AiSpaceController::setDensity(int v)
{
    perform([v](SmartSpaceWidget *w) { w->setResultDensity(v); });
}
void AiSpaceController::pauseKnowledgeTask()
{
    perform([](SmartSpaceWidget *w) { w->pauseKnowledgeWork(); });
}
void AiSpaceController::startFullIndex()
{
    perform([](SmartSpaceWidget *w) {
        QTimer::singleShot(0, w, &SmartSpaceWidget::startFastFullIndex);
    });
}
void AiSpaceController::hideFolder(const QString &p)
{
    perform([p](SmartSpaceWidget *w) { w->hideFolder(p); });
}
void AiSpaceController::excludeFolder(const QString &p)
{
    perform(
        [p](SmartSpaceWidget *w) { QTimer::singleShot(0, w, [w, p] { w->excludeFolder(p); }); });
}
void AiSpaceController::search(const QString &q)
{
    showWindow();
    perform([q](SmartSpaceWidget *w) { w->searchFor(q); });
}
