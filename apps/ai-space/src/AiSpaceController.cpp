#include "AiSpaceController.h"
#include "SmartSpaceWidget.h"
#include "Migration.h"
#include "WallpaperBackdrop.h"
#include "../../../shared/async-work/BackgroundTask.h"
#include "LiquidDialog.h"
#include <QApplication>
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

static bool writeAutoStart(bool enabled)
{
    const QString path=QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)+"/autostart/ai-space.desktop";
    if(!enabled) return !QFileInfo::exists(path) || QFile::remove(path);
    if(!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    // Exec quoting, then Desktop Entry string escaping. No shell is invoked.
    QString executable=QCoreApplication::applicationFilePath();
    executable.replace('\\',"\\\\").replace('"',"\\\"").replace('`',"\\`").replace('$',"\\$");
    executable.replace('\\',"\\\\").replace('%',"%%");
    QSaveFile file(path);
    if(!file.open(QIODevice::WriteOnly)) return false;
    const auto entry=("[Desktop Entry]\nType=Application\nName=AI空间\nExec=\""+executable+"\" --autostart\nIcon=ai-space\nTerminal=false\n").toUtf8();
    return file.write(entry)==entry.size() && file.commit();
}

AiSpaceController::AiSpaceController(QObject *parent):QObject(parent) {}
AiSpaceController::~AiSpaceController() { delete m_widget.data();delete m_bootstrap.data(); }
void AiSpaceController::initialize(bool visible,bool settings)
{
    m_requestedVisible=visible;
    if(visible && !qEnvironmentVariableIsSet("AI_SPACE_TEST_NO_BOOTSTRAP")) {
        auto *dialog=new LiquidDialog::Dialog;
        dialog->setWindowTitle(QStringLiteral("AI空间"));dialog->setWindowIcon(qApp->windowIcon());
        auto *layout=new QVBoxLayout(dialog);
        layout->addWidget(new QLabel(QStringLiteral("正在准备液态界面与旧数据副本…"),dialog));
        dialog->resize(460,160);dialog->show();m_bootstrap=dialog;
        connect(dialog,&QDialog::rejected,this,&AiSpaceController::quit);
    }
    if(settings)m_pending.append([](SmartSpaceWidget *w){QTimer::singleShot(0,w,&SmartSpaceWidget::showSettings);});
    BackgroundTask::run(this,[]{
        const auto migration=migrateLegacyData();
        WallpaperBackdrop wallpaper;wallpaper.reload();
        return qMakePair(migration,wallpaper);
    },[this](const QPair<MigrationResult,WallpaperBackdrop> &result){
        WallpaperBackdrop::shared()=result.second;
        auto *widget=new SmartSpaceWidget;
        m_widget=widget;
        widget->setWindowTitle(QStringLiteral("AI空间"));
        widget->setWindowIcon(qApp->windowIcon());
        connect(widget,&SmartSpaceWidget::closeRequested,this,&AiSpaceController::quit);
        connect(widget,&SmartSpaceWidget::alwaysOnTopChanged,this,[this]{applyWindowMode();notify();});
        connect(widget,&SmartSpaceWidget::geometryChanged,this,&AiSpaceController::notify);
        connect(widget,&SmartSpaceWidget::autoStartChanged,this,[this,widget](bool on){
            if(!writeAutoStart(on)) {
                const QSignalBlocker blocker(widget);
                widget->setAutoStart(!on);
                QTimer::singleShot(0,widget,[widget]{LiquidDialog::warning(widget,QStringLiteral("登录启动"),QStringLiteral("无法更新登录启动文件，请检查用户配置目录的权限。"));});
            }
            notify();
        });
        for(auto *process:widget->findChildren<QProcess*>()) {
            connect(process,&QProcess::started,this,&AiSpaceController::notify);
            connect(process,QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished),this,[this]{notify();});
        }
        applyWindowMode();
        if(m_bootstrap){m_bootstrap->hide();m_bootstrap->deleteLater();}
        if(m_requestedVisible)showWindow();else widget->hide();
        for(auto &action:m_pending)action(widget);
        m_pending.clear();
        m_tray=new QSystemTrayIcon(qApp->windowIcon(),this);
        auto *menu=new QMenu(widget);
        menu->addAction(QStringLiteral("打开AI空间"),this,&AiSpaceController::showWindow);
        menu->addAction(QStringLiteral("设置"),this,&AiSpaceController::showSettings);
        menu->addSeparator();menu->addAction(QStringLiteral("退出AI空间"),this,&AiSpaceController::quit);
        m_tray->setContextMenu(menu);m_tray->setToolTip(QStringLiteral("AI空间"));m_tray->show();
        connect(m_tray,&QSystemTrayIcon::activated,this,[this](QSystemTrayIcon::ActivationReason r){if(r==QSystemTrayIcon::Trigger)showWindow();});
        m_wallpaperWatcher=new QFileSystemWatcher(this);auto *debounce=new QTimer(this);
        debounce->setSingleShot(true);debounce->setInterval(250);watchWallpaper();
        connect(m_wallpaperWatcher,&QFileSystemWatcher::fileChanged,debounce,QOverload<>::of(&QTimer::start));
        connect(m_wallpaperWatcher,&QFileSystemWatcher::directoryChanged,debounce,QOverload<>::of(&QTimer::start));
        connect(debounce,&QTimer::timeout,this,&AiSpaceController::refreshWallpaper);
        if(!result.first.ok)QTimer::singleShot(0,widget,[widget,error=result.first.error]{LiquidDialog::warning(widget,QStringLiteral("导入旧数据"),error);});
        notify();
    });
}
void AiSpaceController::perform(std::function<void(SmartSpaceWidget*)> action) { if(m_widget)action(m_widget);else if(m_pending.size()<64)m_pending.append(std::move(action)); }
void AiSpaceController::applyWindowMode() {
    if(!m_widget)return;const bool visible=m_widget->isVisible();const QPoint p=m_widget->pos();
    m_widget->setWindowFlags(Qt::Window|Qt::FramelessWindowHint|Qt::WindowMinimizeButtonHint|Qt::WindowCloseButtonHint|(m_widget->alwaysOnTop()?Qt::WindowStaysOnTopHint:Qt::WindowFlags()));
    m_widget->recreateNativeSurface(true);m_widget->move(p);if(visible)m_widget->show();
}
void AiSpaceController::notify(){emit stateChanged(status());}
QString AiSpaceController::status() const {
    QJsonObject o{{"application","ai-space"},{"version","0.1.0"},{"ready",bool(m_widget)},{"pid",QCoreApplication::applicationPid()}};
    if(m_widget){o.insert("autoStart",m_widget->autoStart());o.insert("visible",m_widget->isVisible());o.insert("edgeHidden",m_widget->edgeHidden());o.insert("indexBusy",m_widget->indexBusy());o.insert("alwaysOnTop",m_widget->alwaysOnTop());o.insert("glassOptics",m_widget->glassOpticsEnabled());o.insert("x",m_widget->x());o.insert("y",m_widget->y());o.insert("width",m_widget->width());o.insert("height",m_widget->height());
        o.insert("indexedEntries",m_widget->indexedEntryCount());o.insert("results",m_widget->resultCount());
        o.insert("windowId",QString::number(m_widget->internalWinId()));
        o.insert("materialBuilds",m_widget->property("smartGlassMaterialBuilds").toInt());
        o.insert("glassBackend",m_widget->property("smartGlassMaterialBackend").toString());
        o.insert("maximizable",bool(m_widget->windowFlags() & Qt::WindowMaximizeButtonHint));
        o.insert("active",m_widget->edgeTransitionActive());
    }
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}
void AiSpaceController::showWindow(){m_requestedVisible=true;if(m_widget){m_widget->showNormal();m_widget->revealFromEdge();m_widget->raise();m_widget->activateWindow();notify();}}
void AiSpaceController::hideWindow(){m_requestedVisible=false;if(m_widget)m_widget->hide();notify();}
void AiSpaceController::toggleWindow(){if(m_widget && m_widget->isVisible() && !m_widget->edgeHidden())hideWindow();else showWindow();}
void AiSpaceController::showSettings(){showWindow();perform([](SmartSpaceWidget*w){QTimer::singleShot(0,w,&SmartSpaceWidget::showSettings);});}
void AiSpaceController::quit(){qApp->quit();}
void AiSpaceController::setAlwaysOnTop(bool v){perform([v](SmartSpaceWidget*w){w->setAlwaysOnTop(v);});}
void AiSpaceController::setAutoStart(bool v){perform([v](SmartSpaceWidget*w){w->setAutoStart(v);});}
void AiSpaceController::hideToEdge(){perform([](SmartSpaceWidget*w){w->hideToNearestEdge();});}
void AiSpaceController::revealFromEdge(){showWindow();}
void AiSpaceController::moveWindow(int x,int y){perform([=](SmartSpaceWidget*w){w->move(x,y);});}
void AiSpaceController::resizeWindow(int x,int y){perform([=](SmartSpaceWidget*w){w->resize(qBound(620,x,2400),qBound(360,y,1600));});}
void AiSpaceController::setDensity(int v){perform([v](SmartSpaceWidget*w){w->setResultDensity(v);});}
void AiSpaceController::setGlassOptics(bool v){perform([v](SmartSpaceWidget*w){w->setSimpleTheme(3,false);w->setGlassOpticsEnabled(v);});}
void AiSpaceController::startFullIndex(){perform([](SmartSpaceWidget*w){QTimer::singleShot(0,w,&SmartSpaceWidget::startFastFullIndex);});}
void AiSpaceController::hideFolder(const QString&p){perform([p](SmartSpaceWidget*w){w->hideFolder(p);});}
void AiSpaceController::excludeFolder(const QString&p){perform([p](SmartSpaceWidget*w){QTimer::singleShot(0,w,[w,p]{w->excludeFolder(p);});});}
void AiSpaceController::search(const QString&q){showWindow();perform([q](SmartSpaceWidget*w){w->searchFor(q);});}

void AiSpaceController::watchWallpaper() {
    if(!m_wallpaperWatcher)return;
    const QString config=QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    QStringList paths=WallpaperBackdrop::shared().sourceWatchPaths();
    paths<<config+"/kyfences"<<config+"/kylin"<<config+"/dconf";
    const auto files=paths;
    for(const auto &path:files)if(!path.isEmpty())paths<<QFileInfo(path).absolutePath();
    paths.removeDuplicates();
    for(const auto &path:paths)if(QFileInfo::exists(path) && !m_wallpaperWatcher->files().contains(path)
        && !m_wallpaperWatcher->directories().contains(path))m_wallpaperWatcher->addPath(path);
}
void AiSpaceController::refreshWallpaper() {
    if(m_wallpaperLoading){m_wallpaperPending=true;return;}
    m_wallpaperLoading=true;
    const auto previous=WallpaperBackdrop::shared();
    BackgroundTask::run(this,[previous]{auto value=previous;const bool changed=value.reload();return qMakePair(changed,value);},[this](const QPair<bool,WallpaperBackdrop> &result){
        m_wallpaperLoading=false;
        if(m_wallpaperPending){m_wallpaperPending=false;refreshWallpaper();return;}
        WallpaperBackdrop::shared()=result.second;watchWallpaper();
        if(result.first && m_widget)m_widget->invalidateWallpaper();
    });
}
