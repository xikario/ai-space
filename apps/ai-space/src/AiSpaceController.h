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
class AiSpaceController final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface","org.ukui.AiSpace")
public:
    explicit AiSpaceController(QObject *parent=nullptr);
    ~AiSpaceController() override;
    void initialize(bool show, bool settings);
public slots:
    Q_SCRIPTABLE void showWindow();
    Q_SCRIPTABLE void hideWindow();
    Q_SCRIPTABLE void toggleWindow();
    Q_SCRIPTABLE void showSettings();
    Q_SCRIPTABLE void showKnowledgeManager();
    Q_SCRIPTABLE void showKnowledgeStatus();
    Q_SCRIPTABLE void quit();
    Q_SCRIPTABLE QString status() const;
    Q_SCRIPTABLE void search(const QString &query);
    Q_SCRIPTABLE void setAlwaysOnTop(bool value);
    Q_SCRIPTABLE void setAutoStart(bool value);
    Q_SCRIPTABLE void hideToEdge();
    Q_SCRIPTABLE void revealFromEdge();
    Q_SCRIPTABLE void moveWindow(int x,int y);
    Q_SCRIPTABLE void resizeWindow(int width,int height);
    Q_SCRIPTABLE void minimizeWindow();
    Q_SCRIPTABLE void maximizeWindow();
    Q_SCRIPTABLE void restoreWindow();
    Q_SCRIPTABLE void toggleMaximize();
    Q_SCRIPTABLE void setDensity(int value);
    Q_SCRIPTABLE void setGlassOptics(bool value);
    Q_SCRIPTABLE void startFullIndex();
    Q_SCRIPTABLE void pauseKnowledgeTask();
    Q_SCRIPTABLE void hideFolder(const QString &path);
    Q_SCRIPTABLE void excludeFolder(const QString &path);
signals:
    void stateChanged(const QString &json);
private:
    void requestClose();
    bool m_quitting=false;
    void notify();
    void applyWindowMode();
    void perform(std::function<void(SmartSpaceWidget*)> action);
    QPointer<SmartSpaceWidget> m_widget;
    QList<std::function<void(SmartSpaceWidget*)>> m_pending;
    QSystemTrayIcon *m_tray=nullptr;
    QPointer<QWidget> m_bootstrap;
    bool m_requestedVisible=true;
    bool m_migrationPending=false;
    QElapsedTimer m_startup;
    qint64 m_uiReadyMs=-1,m_indexReadyMs=-1;
};
