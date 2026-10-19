#pragma once
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariant>
#include <QList>
#include <functional>
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
    Q_SCRIPTABLE void quit();
    Q_SCRIPTABLE QString status() const;
    Q_SCRIPTABLE void search(const QString &query);
    Q_SCRIPTABLE void setAlwaysOnTop(bool value);
    Q_SCRIPTABLE void setAutoStart(bool value);
    Q_SCRIPTABLE void hideToEdge();
    Q_SCRIPTABLE void revealFromEdge();
    Q_SCRIPTABLE void moveWindow(int x,int y);
    Q_SCRIPTABLE void resizeWindow(int width,int height);
    Q_SCRIPTABLE void setDensity(int value);
    Q_SCRIPTABLE void setGlassOptics(bool value);
    Q_SCRIPTABLE void startFullIndex();
    Q_SCRIPTABLE void hideFolder(const QString &path);
    Q_SCRIPTABLE void excludeFolder(const QString &path);
signals:
    void stateChanged(const QString &json);
private:
    void notify();
    void applyWindowMode();
    void refreshWallpaper();
    void watchWallpaper();
    void perform(std::function<void(SmartSpaceWidget*)> action);
    QPointer<SmartSpaceWidget> m_widget;
    QList<std::function<void(SmartSpaceWidget*)>> m_pending;
    QSystemTrayIcon *m_tray=nullptr;
    QPointer<QWidget> m_bootstrap;
    QFileSystemWatcher *m_wallpaperWatcher=nullptr;
    bool m_wallpaperLoading=false,m_wallpaperPending=false;
    bool m_requestedVisible=true;
};
