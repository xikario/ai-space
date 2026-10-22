#include "WindowCornerFallback.h"
#include "KnowledgeManagerDialog.h"
#include "JsonWorkerRequest.h"
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QApplication>
#include <QCheckBox>
#include <QFrame>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QStandardPaths>
#include <QPushButton>
#include <QSpinBox>
#include <QStorageInfo>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

KnowledgeManagerDialog::KnowledgeManagerDialog(const QString &directory,const QString &personalDb,
    const QStringList &roots,int reserve,int quota,int cpu,int memory,const QString &languages,bool light,const QString &worker,QWidget *parent)
    : QDialog(parent,Qt::Dialog|Qt::FramelessWindowHint)
{
    setCursor(Qt::ArrowCursor);
    installWindowCornerFallback(this,18);
    setAttribute(Qt::WA_TranslucentBackground);setAutoFillBackground(false);
    setObjectName("knowledgeManager");setWindowTitle(QStringLiteral("知识库管理与建库向导"));
    setWindowIcon(qApp->windowIcon());resize(750,640);
    const QString bg=light?"#f1f5f9":"#0b1120",fg=light?"#0f172a":"#e2e8f0",field=light?"#ffffff":"#111827",border=light?"#cbd5e1":"#334155";
    setStyleSheet(QString("QDialog#knowledgeManager{background:transparent;border:0;} QFrame#knowledgeSurface{background:%1;color:%2;border:1px solid %4;border-radius:18px;} QLabel,QCheckBox{color:%2;background:transparent;} QLineEdit,QSpinBox{background:%3;color:%2;border:1px solid %4;border-radius:6px;padding:7px;} QPushButton{background:%3;color:%2;border:1px solid %4;border-radius:6px;padding:8px 12px;} QPushButton:hover{border-color:#6366f1;}").arg(bg,fg,field,border));
    auto *outerLayout=new QVBoxLayout(this);outerLayout->setContentsMargins(0,0,0,0);
    auto *surface=new QFrame(this);surface->setObjectName("knowledgeSurface");outerLayout->addWidget(surface);
    auto *frameLayout=new QVBoxLayout(surface);frameLayout->setContentsMargins(22,16,22,18);frameLayout->setSpacing(12);
    auto *title=new QHBoxLayout;auto *heading=new QLabel(QStringLiteral("知识库 · 存储与导入"));heading->setStyleSheet("font-size:18px;font-weight:600;");title->addWidget(heading);title->addStretch();
    auto *minimize=new QPushButton(QStringLiteral("−"));auto *close=new QPushButton(QStringLiteral("×"));minimize->setFixedWidth(40);close->setFixedWidth(40);title->addWidget(minimize);title->addWidget(close);frameLayout->addLayout(title);
    minimize->setObjectName("knowledgeMinimizeButton");close->setObjectName("knowledgeCloseButton");
    minimize->setFocusPolicy(Qt::StrongFocus);close->setFocusPolicy(Qt::StrongFocus);
    connect(minimize,&QPushButton::clicked,this,[this]{showMinimized();});
    connect(close,&QPushButton::clicked,this,&QDialog::reject);
    auto *tabs=new QTabWidget(this);frameLayout->addWidget(tabs,1);
    auto *guide=new QWidget;auto *guideLayout=new QVBoxLayout(guide);tabs->addTab(guide,QStringLiteral("分阶段建库"));
    auto *stepTitle=new QLabel;stepTitle->setObjectName("knowledgeWizardStep");stepTitle->setStyleSheet("font-size:18px;font-weight:600;padding:10px 0;");guideLayout->addWidget(stepTitle);
    auto *pages=new QStackedWidget;pages->setObjectName("knowledgeWizardPages");guideLayout->addWidget(pages,1);
    auto page=[&](){auto *scroll=new QScrollArea;scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);auto *body=new QWidget;auto *box=new QVBoxLayout(body);box->setSpacing(16);scroll->setWidget(body);pages->addWidget(scroll);return box;};
    auto note=[](QVBoxLayout *box,const QString &text){auto *label=new QLabel(text);label->setWordWrap(true);box->addWidget(label);};
    auto *prepare=page();
    note(prepare,QStringLiteral("开始前先准备好资料目录和专用存储位置。大知识库可能达到数十 GB；请优先选择空间充足的数据盘，接好移动硬盘，并保持资料可读取。"));
    note(prepare,QStringLiteral("已有知识库可直接检索，不必重新建库。接收别人的 .aikb 文件，请使用右侧“导入 / 导出”页。"));
    auto *form=new QFormLayout;prepare->addLayout(form);
    auto *storageRow=new QHBoxLayout;m_directory=new QLineEdit(directory);m_directory->setObjectName("knowledgeStorageDirectory");auto *browse=new QPushButton(QStringLiteral("选择位置…"));storageRow->addWidget(m_directory,1);storageRow->addWidget(browse);form->addRow(QStringLiteral("知识库存储位置"),storageRow);
    connect(browse,&QPushButton::clicked,this,[this]{const QString path=QFileDialog::getExistingDirectory(this,QStringLiteral("选择知识库存储盘"),m_directory->text());if(!path.isEmpty())m_directory->setText(path);});
    m_space=new QLabel;m_space->setWordWrap(true);form->addRow(QStringLiteral("目标磁盘"),m_space);connect(m_directory,&QLineEdit::textChanged,this,[this]{updateSpace();});
    auto spin=[this](int low,int high,int value,const QString &suffix){auto *s=new QSpinBox(this);s->setRange(low,high);s->setValue(value);s->setSuffix(suffix);return s;};
    m_reserve=spin(1,1024,reserve," GiB");m_quota=spin(1,4096,quota," GiB");form->addRow(QStringLiteral("磁盘至少保留"),m_reserve);form->addRow(QStringLiteral("数据库上限（含 WAL）"),m_quota);
    auto *rootRow=new QHBoxLayout;m_roots=new QPlainTextEdit(roots.join("\n"));m_roots->setMaximumHeight(110);m_roots->setObjectName("knowledgeSourceRoots");auto *addRoot=new QPushButton(QStringLiteral("添加资料目录…"));rootRow->addWidget(m_roots,1);rootRow->addWidget(addRoot);form->addRow(QStringLiteral("资料目录（每行一个）"),rootRow);
    connect(addRoot,&QPushButton::clicked,this,[this]{auto path=QFileDialog::getExistingDirectory(this,QStringLiteral("选择要建库的资料目录"));if(!path.isEmpty()){auto paths=this->roots();if(!paths.contains(path))paths.append(path);m_roots->setPlainText(paths.join("\n"));}});
    m_memory=spin(512,8192,memory," MiB");form->addRow(QStringLiteral("各阶段工作进程地址空间上限"),m_memory);
    note(prepare,QStringLiteral("此内存预算用于扫描、文本提取和 OCR；最低 512 MiB，大资料集建议 1024 MiB 或更多。单库上限包含数据库和 WAL；清单、上一代快照、页缓存及临时文件另占空间，但同样受磁盘保留值约束。"));
    note(prepare,QStringLiteral("现有个人库：%1\n选择新位置不会立即搬运或删除旧库；新位置的文本建库成功后再切换。旧库保留为只读来源。").arg(QFileInfo::exists(personalDb)?personalDb:QStringLiteral("尚未建立")));prepare->addStretch();
    auto *scan=page();note(scan,QStringLiteral("① 全量扫描 · 先弄清楚有哪些资料"));note(scan,QStringLiteral("遍历所选资料目录，记录文件名称、类型、位置和修改时间。此阶段不提取正文，也不运行 OCR。\n\n产出：文件清单，供后续文本建库使用。扫描数量在完成前可能变化，因此不会提前显示虚假的总进度。\n\n资料移动、增删后，可在菜单的“增量更新”中重新扫描并复用已有正文。"));scan->addStretch();
    auto *text=page();note(text,QStringLiteral("② 文本建库 · 先让可读取的正文可用"));note(text,QStringLiteral("按第 1 步的清单提取文本、Office / WPS 文档和 PDF 文本层。已有且未变化的提取结果会复用。\n\n产出：可以搜索和预览的正文知识库；此阶段完成即可使用，无需等待 OCR。\n\n扫描 PDF 与图片中尚未识别的内容会标记为待补全。提取失败和没有正文分别记录，不会当作已完成。"));text->addStretch();
    auto *ocrPage=page();note(ocrPage,QStringLiteral("③ OCR 补全 · 最后处理图片与扫描 PDF"));note(ocrPage,QStringLiteral("OCR 比文本提取更耗资源，建议空闲时执行。可以先完成前两步，以后再点“③ OCR补全”。暂停会保留已提交的文档和受限的页缓存。"));
    m_ocr=new QCheckBox(QStringLiteral("本次文本建库完成后，继续 OCR（默认关闭）"));ocrPage->addWidget(m_ocr);
    auto *resources=new QFormLayout;m_cpu=spin(5,100,cpu,QStringLiteral("% 单核"));resources->addRow(QStringLiteral("OCR CPU 上限"),m_cpu);m_languages=new QLineEdit(languages);m_languages->setObjectName("knowledgeOcrLanguages");resources->addRow(QStringLiteral("OCR 语言（用 + 连接）"),m_languages);ocrPage->addLayout(resources);
    note(ocrPage,QStringLiteral("单任务、单线程、低优先级；系统内存不足或磁盘低于保留值时停止。OCR 页缓存最多 512 MiB。剩余数量和提取失败可在“当前状态详情”中查看；未检测的页数、剩余耗时会明确显示为未知。"));ocrPage->addStretch();
    auto *navigation=new QHBoxLayout;auto *previous=new QPushButton(QStringLiteral("上一步"));auto *next=new QPushButton(QStringLiteral("下一步"));auto *start=new QPushButton(QStringLiteral("按顺序开始 / 续建"));start->setObjectName("knowledgeAction4");previous->setObjectName("knowledgePreviousStep");next->setObjectName("knowledgeNextStep");navigation->addWidget(previous);navigation->addStretch();navigation->addWidget(next);navigation->addWidget(start);guideLayout->addLayout(navigation);
    auto updateStep=[=]{const int i=pages->currentIndex();if(parent)parent->setProperty("knowledgeWizardStep",i);stepTitle->setText(QStringList{QStringLiteral("前置准备 · 资料与存储"),QStringLiteral("第 1 步 / 3 · 全量扫描"),QStringLiteral("第 2 步 / 3 · 文本建库"),QStringLiteral("第 3 步 / 3 · OCR 补全")}.at(i));previous->setEnabled(i>0);next->setVisible(i<3);start->setVisible(i==3);};
    connect(previous,&QPushButton::clicked,this,[=]{pages->setCurrentIndex(pages->currentIndex()-1);updateStep();});
    connect(next,&QPushButton::clicked,this,[=]{if(pages->currentIndex()==0){if(!validate(Build))return;if(this->roots().isEmpty()){m_space->setText(QStringLiteral("请至少选择一个资料目录"));return;}for(const auto &path:this->roots())if(!QFileInfo(path).isDir()){m_space->setText(QStringLiteral("资料目录不存在：%1").arg(path));return;}}pages->setCurrentIndex(pages->currentIndex()+1);updateStep();});
    connect(start,&QPushButton::clicked,this,[this]{if(validate(Build))done(Build);});updateStep();
    auto *manage=new QWidget;auto *layout=new QVBoxLayout(manage);tabs->addTab(manage,QStringLiteral("导入 / 导出"));
    note(layout,QStringLiteral("接收 .aikb 可直接检索，无需执行扫描、文本建库或 OCR。默认原位只读挂载，不额外复制大文件。复制导入和导出前，请在“前置准备”页选择存储位置。"));
    m_mount=new QCheckBox(QStringLiteral("原位只读挂载（源文件需保持原位置）"));m_mount->setChecked(true);layout->addWidget(m_mount);
    auto *mounted=new QListWidget(this);mounted->setObjectName("knowledgeMountedLibraries");mounted->setStyleSheet(QString("QListWidget{background:%1;color:%2;border:1px solid %3;border-radius:6px;}").arg(field,fg,border));
    layout->addWidget(new QLabel(QStringLiteral("共享库与保留版本 · 移除挂载不会删除文件")));layout->addWidget(mounted,1);
    auto *mountActions=new QHBoxLayout;auto *remove=new QPushButton(QStringLiteral("移除所选挂载"));auto *enable=new QPushButton(QStringLiteral("启用所选版本"));mountActions->addWidget(enable);mountActions->addWidget(remove);layout->addLayout(mountActions);
    m_message=new QLabel; m_message->setWordWrap(true);layout->addWidget(m_message);
    const QString libraryScript=QFileInfo(worker).dir().filePath(QStringLiteral("knowledge_library.py"));
    auto load=[this,mounted,libraryScript]{
        if(m_managementBusy)return;
        m_managementBusy=true;emit backgroundBusyChanged(true);
        m_message->setText(QStringLiteral("正在读取挂载列表…"));
        JsonWorkerRequest::run(this,libraryScript,{QStringLiteral("libraries")},[this,mounted](const JsonWorkerRequest::Result &result){
            m_managementBusy=false;emit backgroundBusyChanged(false);
            if(!result.success){m_message->setText(result.error);return;}
            mounted->clear();
            const auto data=result.data;
            QStringList warnings;for(const auto &warning:data.value("registryWarnings").toArray())warnings<<warning.toString();
            m_message->setText(warnings.isEmpty()?QString():warnings.join(QStringLiteral("；"))+QStringLiteral("。可点击“修复注册表”，原文件将先备份。"));
            for(const auto &value:data.value("libraries").toArray()){
                const auto row=value.toObject();if(!row.value("read_only").toBool())continue;
                auto *item=new QListWidgetItem(row.value("name").toString()+(row.value("enabled").toBool(true)?QStringLiteral(" · 使用中 · "):QStringLiteral(" · 保留版本 · "))+row.value("path").toString(),mounted);
                item->setToolTip(item->text());item->setData(Qt::UserRole,row);
            }
        });
    };
    auto mutate=[this,libraryScript,load](const QStringList &arguments){
        if(m_managementBusy)return;
        m_managementBusy=true;emit backgroundBusyChanged(true);
        m_message->setText(QStringLiteral("正在保存挂载设置…"));
        JsonWorkerRequest::run(this,libraryScript,arguments,[this,load](const JsonWorkerRequest::Result &result){
            m_managementBusy=false;emit backgroundBusyChanged(false);
            if(!result.success){m_message->setText(result.error);return;}
            const QString backup=result.data.value("backup").toString();
            if(!backup.isEmpty())m_space->setText(QStringLiteral("注册表已修复，原文件备份：%1").arg(backup));
            load();
        });
    };
    connect(remove,&QPushButton::clicked,this,[mounted,mutate]{
        if(!mounted->currentItem())return;
        const auto row=mounted->currentItem()->data(Qt::UserRole).toJsonObject();
        mutate({QStringLiteral("unmount"),QStringLiteral("--library-id"),row.value("library_id").toString(),QStringLiteral("--version"),row.value("version").toString()});
    });
    connect(enable,&QPushButton::clicked,this,[mounted,mutate]{
        if(!mounted->currentItem())return;
        const auto row=mounted->currentItem()->data(Qt::UserRole).toJsonObject();
        mutate({QStringLiteral("enable"),QStringLiteral("--library-id"),row.value("library_id").toString(),QStringLiteral("--version"),row.value("version").toString()});
    });
    auto *repair=new QPushButton(QStringLiteral("修复注册表（保留备份）"));mountActions->addWidget(repair);
    connect(repair,&QPushButton::clicked,this,[mutate]{mutate({QStringLiteral("repair-registry")});});
    load();
    auto *buttons=new QHBoxLayout;layout->addLayout(buttons);
    for(const auto &entry:QList<QPair<QString,int>>{{QStringLiteral("导入 .aikb…"),Import},{QStringLiteral("导出当前库…"),Export}}){auto *button=new QPushButton(entry.first);button->setObjectName(QString("knowledgeAction%1").arg(entry.second));buttons->addWidget(button);connect(button,&QPushButton::clicked,this,[this,action=entry.second]{if(validate(action))done(action);});}
    auto *save=new QPushButton(QStringLiteral("仅保存设置"));frameLayout->addWidget(save);connect(save,&QPushButton::clicked,this,[this]{if(validate(Save))done(Save);});
    tabs->setStyleSheet(QString("QTabWidget::pane{border:0;} QTabBar::tab{padding:10px 18px;color:%2;background:%1;} QTabBar::tab:selected{border-bottom:2px solid #6366f1;} QScrollArea,QScrollArea>QWidget>QWidget{background:%1;}").arg(bg,fg));
    setMinimumSize(650,520);resize(780,680);
    updateSpace();
}
QString KnowledgeManagerDialog::directory()const{return QDir::cleanPath(m_directory->text().trimmed());}
QStringList KnowledgeManagerDialog::roots()const{QStringList result;for(auto path:m_roots->toPlainText().split('\n',QString::SkipEmptyParts)){path=QDir::cleanPath(path.trimmed());if(!result.contains(path))result.append(path);}return result;}
int KnowledgeManagerDialog::reserveGiB()const{return m_reserve->value();}int KnowledgeManagerDialog::quotaGiB()const{return m_quota->value();}
QString KnowledgeManagerDialog::ocrLanguages()const{return m_languages->text().trimmed();}
int KnowledgeManagerDialog::ocrCpu()const{return m_cpu->value();}int KnowledgeManagerDialog::ocrMemory()const{return m_memory->value();}
bool KnowledgeManagerDialog::includeOcr()const{return m_ocr->isChecked();}bool KnowledgeManagerDialog::mountOnly()const{return m_mount->isChecked();}
void KnowledgeManagerDialog::updateSpace(){QFileInfo parent(directory());while(!parent.exists()&&parent.absoluteFilePath()!=QStringLiteral("/"))parent=QFileInfo(parent.absolutePath());QStorageInfo disk(parent.absoluteFilePath());m_space->setText(QStringLiteral("可用 %1 GiB · %2").arg(disk.bytesAvailable()/1073741824.0,0,'f',1).arg(disk.rootPath()));}
bool KnowledgeManagerDialog::validate(int action){if(m_managementBusy){m_message->setText(QStringLiteral("请等待当前挂载操作完成"));m_space->setText(QStringLiteral("正在读取或保存挂载信息，请等待完成后继续"));return false;}if(!QRegularExpression(QStringLiteral("^[A-Za-z0-9_]+(\\+[A-Za-z0-9_]+)*$")).match(ocrLanguages()).hasMatch()){m_space->setText(QStringLiteral("OCR 语言无效，请使用 chi_sim+eng 等语言代码"));return false;}if(action==Export)return true;if(directory().isEmpty()||!QDir::isAbsolutePath(directory())){m_space->setText(QStringLiteral("请选择完整的存储目录路径"));return false;}if(action==Import && mountOnly())return true;QFileInfo parent(directory());while(!parent.exists()&&parent.absoluteFilePath()!=QStringLiteral("/"))parent=QFileInfo(parent.absolutePath());QStorageInfo disk(parent.absoluteFilePath());if(!parent.isDir()||!parent.isWritable()||disk.bytesAvailable()<qint64(reserveGiB())*1073741824){m_space->setText(QStringLiteral("目标不可写或可用空间低于保留值，请更换存储盘"));return false;}return true;}

void KnowledgeManagerDialog::mousePressEvent(QMouseEvent *event){if(event->button()==Qt::LeftButton && event->pos().y()<55){m_dragging=true;m_dragOffset=event->globalPos()-frameGeometry().topLeft();event->accept();return;}QDialog::mousePressEvent(event);}
void KnowledgeManagerDialog::mouseMoveEvent(QMouseEvent *event){if(m_dragging){move(event->globalPos()-m_dragOffset);event->accept();return;}QDialog::mouseMoveEvent(event);}
void KnowledgeManagerDialog::mouseReleaseEvent(QMouseEvent *event){m_dragging=false;QDialog::mouseReleaseEvent(event);}
