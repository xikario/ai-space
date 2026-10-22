#include "SmartSpaceWidget.h"
#include "KnowledgeManagerDialog.h"
#include "KnowledgeFailuresDialog.h"
#include "KnowledgeJobController.h"
#include "UiMessages.h"
#include <QPaintEvent>
#include <QDBusPendingCall>
#include <QProcessEnvironment>
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
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <algorithm>
using namespace AiSpaceUi;
namespace { constexpr int kKnowledgeErrorLimit=64*1024; }

void SmartSpaceWidget::startKnowledgeTask(
        const QString &task, const QStringList &arguments)
{
    if(m_uiState.failuresVisible){m_statusLabel->setText(QStringLiteral("请先关闭失败项处理窗口"));return;}
    if(m_uiState.migrationPending) {
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
        m_libraryJobBusy = true;emit busyChanged();
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
            if(m_libraryJobBusy){m_libraryJobBusy=false;emit busyChanged();m_indexProgress->hide();m_idleIndexButton->setEnabled(true);m_refreshButton->setEnabled(true);if(m_textIndexButton)m_textIndexButton->setEnabled(true);m_ocrIndexButton->setEnabled(true);m_pauseIndexButton->setEnabled(false);}
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
        m_dialogJobBusy=failures.backgroundBusy();emit busyChanged();
        connect(&failures,&KnowledgeFailuresDialog::backgroundBusyChanged,this,[this](bool busy){m_dialogJobBusy=busy;emit busyChanged();});
        m_uiState.failuresVisible=true;setProperty("knowledgeFailuresVisible",true);const int action=failures.exec();m_dialogJobBusy=false;emit busyChanged();m_uiState.failuresVisible=false;setProperty("knowledgeFailuresVisible",false);
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
    if(m_uiState.failuresVisible){m_statusLabel->setText(QStringLiteral("请先关闭失败项处理窗口"));return;}
    if(m_uiState.managerVisible){m_statusLabel->setText(QStringLiteral("知识库向导已打开"));return;}
    if (m_libraryJobBusy || (m_knowledgeProcess && m_knowledgeProcess->property("knowledgeTask").toString()!=QLatin1String("search"))) {
        smartInformation(this,QStringLiteral("知识库任务正在运行"),QStringLiteral("请先等待完成，或通过暂停按钮保存进度后再管理知识库。"));return;
    }
    KnowledgeManagerDialog dialog(m_libraryStorageDirectory,knowledgeDbPath(),m_roots,
        m_storageReserveGiB,m_libraryQuotaGiB,m_ocrCpuPercent,m_ocrMemoryMiB,m_ocrLanguages,m_themeMode==2,knowledgeWorkerPath(),this);
    m_dialogJobBusy=dialog.backgroundBusy();emit busyChanged();
    connect(&dialog,&KnowledgeManagerDialog::backgroundBusyChanged,this,[this](bool busy){m_dialogJobBusy=busy;emit busyChanged();});
    m_uiState.managerVisible=true;setProperty("knowledgeManagerVisible",true);
    const int action=dialog.exec();
    m_uiState.managerVisible=false;setProperty("knowledgeManagerVisible",false);
    m_dialogJobBusy=false;emit busyChanged();
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
    m_ocrCpuPercent=dialog.ocrCpu();m_ocrMemoryMiB=dialog.ocrMemory();m_ocrLanguages=dialog.ocrLanguages();m_storageConfirmed=true;
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
    if(m_uiState.managerVisible || m_uiState.failuresVisible || m_settingsViewVisible){m_statusLabel->setText(QStringLiteral("请先关闭向导、失败项处理或设置页，再启动知识库任务"));return;}
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
        QStringLiteral("--ocr-cpu"),QString::number(m_ocrCpuPercent),QStringLiteral("--ocr-memory"),QString::number(m_ocrMemoryMiB),QStringLiteral("--ocr-languages"),m_ocrLanguages};
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
        QStringLiteral("--chunk-size"),QString::number(m_knowledgeChunkSize),QStringLiteral("--overlap"),QString::number(m_knowledgeOverlap),QStringLiteral("--reserve-gib"),QString::number(m_storageReserveGiB),QStringLiteral("--max-gib"),QString::number(m_libraryQuotaGiB),QStringLiteral("--ocr-cpu"),QString::number(m_ocrCpuPercent),QStringLiteral("--ocr-memory"),QString::number(m_ocrMemoryMiB),QStringLiteral("--ocr-languages"),m_ocrLanguages};
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
    if(m_libraryJobBusy){m_libraryJobBusy=false;emit busyChanged();m_indexProgress->hide();m_idleIndexButton->setEnabled(true);m_refreshButton->setEnabled(true);if(m_textIndexButton)m_textIndexButton->setEnabled(true);m_ocrIndexButton->setEnabled(true);m_pauseIndexButton->setEnabled(false);}
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
    if(m_uiState.managerVisible || m_uiState.failuresVisible || m_settingsViewVisible){if(!qobject_cast<QTimer *>(sender()))m_statusLabel->setText(QStringLiteral("请先关闭向导、失败项处理或设置页再启动增量更新"));return;}
    const QString current=QFileInfo(knowledgeDbPath()).absolutePath();
    startGuidedBuild(false,false,QString(),QFileInfo::exists(knowledgeDbPath())?current:m_libraryStorageDirectory);
}
