#pragma once
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariant>
#include <QList>
#include <functional>
#include <QElapsedTimer>
class SmartSpaceWidget;
class QSystemTrayIcon;
class QFileSystemWatcher;
class QWidget;
#ifdef AI_SPACE_TEST_CONTROL
#define AI_SPACE_TEST_SLOT Q_SCRIPTABLE
#else
#define AI_SPACE_TEST_SLOT
#endif

class AiSpaceController final : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.github.xikario.AiSpace")
  public:
    explicit AiSpaceController(QObject *parent = nullptr);
    ~AiSpaceController() override;
    void initialize(bool show, bool settings);
  public slots:
    Q_SCRIPTABLE void showWindow();
    Q_SCRIPTABLE void hideWindow();
    Q_SCRIPTABLE void toggleWindow();
    Q_SCRIPTABLE void showSettings();
    AI_SPACE_TEST_SLOT void showKnowledgeManager();
    AI_SPACE_TEST_SLOT void showKnowledgeStatus();
    Q_SCRIPTABLE void quit();
    Q_SCRIPTABLE QString status() const;
    AI_SPACE_TEST_SLOT void search(const QString &query);
    AI_SPACE_TEST_SLOT void setAlwaysOnTop(bool value);
    AI_SPACE_TEST_SLOT void setAutoStart(bool value);
    AI_SPACE_TEST_SLOT void moveWindow(int x, int y);
    AI_SPACE_TEST_SLOT void resizeWindow(int width, int height);
    AI_SPACE_TEST_SLOT void minimizeWindow();
    AI_SPACE_TEST_SLOT void maximizeWindow();
    AI_SPACE_TEST_SLOT void restoreWindow();
    AI_SPACE_TEST_SLOT void toggleMaximize();
    AI_SPACE_TEST_SLOT void setDensity(int value);
    AI_SPACE_TEST_SLOT void startFullIndex();
    AI_SPACE_TEST_SLOT void pauseKnowledgeTask();
    AI_SPACE_TEST_SLOT void hideFolder(const QString &path);
    AI_SPACE_TEST_SLOT void excludeFolder(const QString &path);
  signals:
    void stateChanged(const QString &json);

  private:
    void requestClose();
    bool m_quitting = false;
    void notify();
    void applyWindowMode();
    void perform(std::function<void(SmartSpaceWidget *)> action);
    QPointer<SmartSpaceWidget> m_widget;
    QList<std::function<void(SmartSpaceWidget *)>> m_pending;
    QSystemTrayIcon *m_tray = nullptr;
    QPointer<QWidget> m_bootstrap;
    bool m_requestedVisible = true;
    bool m_migrationPending = false;
    QElapsedTimer m_startup;
    qint64 m_uiReadyMs = -1, m_indexReadyMs = -1;
};
#undef AI_SPACE_TEST_SLOT
