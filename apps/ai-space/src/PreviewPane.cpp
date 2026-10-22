#include "SmartSpaceWidget.h"
#include "SpacePresentation.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTextBrowser>
#include <QTimer>
#include <QToolTip>
#include <QtEndian>
#include <algorithm>
#include <cstring>
#include <zlib.h>
using namespace AiSpaceUi;

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
