#include "SmartSpaceWidget.h"
#include "SpacePresentation.h"
#include "SpaceFormats.h"
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
#include <QtDBus/QDBusInterface>
#include <QtDBus/QDBusConnection>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QSaveFile>
#include <algorithm>
using namespace AiSpaceUi;
namespace
{
bool copyDirectoryTree(const QString &sourcePath, const QString &targetPath, QString *error)
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
    const QFileInfoList entries =
        source.entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden | QDir::System,
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
                *error = QStringLiteral("无法复制文件：%1").arg(entry.fileName());
            return false;
        }
    }
    return true;
}

} // namespace
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
    const QColor field = m_themeMode == 2 ? QColor(255, 255, 255, 58) : QColor(15, 23, 42, 58);
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
    )")
                             .arg(cssColor(m_textColor), cssColor(m_borderColor),
                                  cssColor(m_mutedColor), cssColor(buttonHover),
                                  cssColor(m_cardColor), cssColor(m_accentColor),
                                  cssColor(accentSoft), cssColor(field))
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
    auto *settingsSubtitle = new QLabel(
        QStringLiteral("索引范围、更新策略、外观和本地 Skill 配置均保存在本机"), &dialog);
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
    connect(backButton, &QPushButton::clicked, this,
            [this] { QTimer::singleShot(0, this, [this] { setSettingsViewVisible(false); }); });
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
    auto *resourceHint = new QLabel(QStringLiteral("文本与OCR资源设置请使用分阶段建库向导。①扫描不"
                                                   "提取正文；②提取文本；③按CPU和内存限额补OCR。"),
                                    indexPage);
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
        return QStringLiteral("%1  %2").arg(
            item->checkState() == Qt::Checked ? QStringLiteral("✔") : QStringLiteral("○"), label);
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
                    : QStringLiteral("%1  .%2").arg(QString::fromLatin1(option.label), id);
            auto *item = new QListWidgetItem(list);
            item->setData(Qt::UserRole, id);
            item->setData(Qt::UserRole + 1, label);
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            item->setCheckState(all || selected.contains(id) ? Qt::Checked : Qt::Unchecked);
            item->setText(formatItemText(item));
        }
        QObject::connect(list, &QListWidget::itemChanged, list,
                         [formatItemText](QListWidgetItem *item) {
                             const QString text = formatItemText(item);
                             if (item->text() != text)
                                 item->setText(text);
                         });
        QObject::connect(list, &QListWidget::itemPressed, list, [](QListWidgetItem *item) {
            if (!item)
                return;
            item->setCheckState(item->checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked);
        });
        return list;
    };
    auto *indexFormatHeading = new QLabel(QStringLiteral("索引格式白名单"), formatTab);
    indexFormatHeading->setStyleSheet(QStringLiteral("font-weight: 600;"));
    formatLayout->addWidget(indexFormatHeading);
    auto *indexFormatHint =
        new QLabel(QStringLiteral("只有勾选的文件才会进入索引；文件夹本身始终保留以支持向下钻取。"),
                   formatTab);
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
    auto *visibleFormatHint =
        new QLabel(QStringLiteral("仅控制右侧匹配文件的显示范围，不会删除已有索引。"), formatTab);
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
    connect(indexMode, QOverload<int>::of(&QComboBox::currentIndexChanged), interval,
            [indexMode, interval](int) {
                interval->setEnabled(indexMode->currentData().toInt() == 2);
            });

    auto *autoStart = new QCheckBox(QStringLiteral("登录后启动AI空间"), updateTab);
    autoStart->setChecked(m_autoStart);
    updateLayout->addWidget(autoStart);
    auto *syncKylin =
        new QCheckBox(QStringLiteral("同步目录到麒麟文件索引服务（如果已安装）"), updateTab);
    syncKylin->setChecked(true);
    updateLayout->addWidget(syncKylin);
    auto *indexNow = new QCheckBox(QStringLiteral("保存设置后立即更新一次索引"), updateTab);
    indexNow->setChecked(false);
    updateLayout->addWidget(indexNow);
    QString snapshotText = QStringLiteral("当前没有索引快照。");
    QFile snapshot(indexPath());
    if (snapshot.open(QIODevice::ReadOnly)) {
        const QJsonObject object = QJsonDocument::fromJson(snapshot.readAll()).object();
        snapshotText = QStringLiteral("已保存快照：%1 项 · %2")
                           .arg(object.value(QStringLiteral("items")).toArray().size())
                           .arg(object.value(QStringLiteral("generatedAt"))
                                    .toString()
                                    .left(19)
                                    .replace(QLatin1Char('T'), QLatin1Char(' ')));
    }
    auto *snapshotLabel = new QLabel(snapshotText, updateTab);
    snapshotLabel->setWordWrap(true);
    updateLayout->addWidget(snapshotLabel);
    auto *saveHint =
        new QLabel(QStringLiteral("索引器每次生成新快照后再原子替换旧文件；"
                                  "AI空间规则和设置单独持久化。关闭组件不会丢失已有索引。"),
                   updateTab);
    saveHint->setWordWrap(true);
    saveHint->setProperty("settingsHint", true);
    updateLayout->addWidget(saveHint);
    auto *idleHint = new QLabel(
        QStringLiteral("“快全量”和“增量”都不启动 OCR。"
                       "“OCR”只补全已标记的扫描 PDF/图片，低优先级逐文件处理并支持暂停续扫。"),
        updateTab);
    idleHint->setWordWrap(true);
    idleHint->setProperty("settingsHint", true);
    updateLayout->addWidget(idleHint);
    updateLayout->addStretch(1);
    tabs->addTab(updateTab, QStringLiteral("更新策略"));

    auto *appearanceTab = new QWidget(tabs);
    auto *appearanceLayout = new QVBoxLayout(appearanceTab);
    appearanceLayout->setContentsMargins(18, 18, 18, 18);
    appearanceLayout->setSpacing(12);
    auto *appearanceTitle = new QLabel(QStringLiteral("界面主题"), appearanceTab);
    appearanceTitle->setProperty("settingsSectionTitle", true);
    appearanceLayout->addWidget(appearanceTitle);
    auto *appearanceHint = new QLabel(
        QStringLiteral(
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
    auto makeThemeCard = [&](int mode, const QString &title, const QColor &canvas,
                             const QColor &card, const QColor &accent, const QColor &ink) {
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
            QColor muted = ink;
            muted.setAlpha(90);
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
        connect(button, &QToolButton::clicked, themeMode,
                [themeMode, mode] { themeMode->setCurrentIndex(themeMode->findData(mode)); });
    };
    makeThemeCard(2, QStringLiteral("浅色雅致"), QColor(QStringLiteral("#f4f6fa")),
                  QColor(QStringLiteral("#ffffff")), QColor(QStringLiteral("#2563eb")),
                  QColor(QStringLiteral("#0f172a")));
    makeThemeCard(1, QStringLiteral("深邃暗夜"), QColor(QStringLiteral("#0b1120")),
                  QColor(QStringLiteral("#131c31")), QColor(QStringLiteral("#3b82f6")),
                  QColor(QStringLiteral("#f1f5f9")));
    themeCards->addStretch(1);
    appearanceLayout->addLayout(themeCards);
    auto *appearanceDetail = new QLabel(
        QStringLiteral(
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
    auto *managerButton =
        new QPushButton(QStringLiteral("存储设置 / 导入导出 / 分阶段建库…"), knowledgeTab);
    knowledgeLayout->addWidget(managerButton);
    connect(managerButton, &QPushButton::clicked, this, &SmartSpaceWidget::showKnowledgeManager);
    auto *knowledgePathRow = new QHBoxLayout;
    auto *knowledgePathEdit = new QLineEdit(m_knowledgeDirectory, knowledgeTab);
    auto *browseKnowledge = new QPushButton(QStringLiteral("选择位置…"), knowledgeTab);
    knowledgePathRow->addWidget(new QLabel(QStringLiteral("保存位置："), knowledgeTab));
    knowledgePathRow->addWidget(knowledgePathEdit, 1);
    knowledgePathRow->addWidget(browseKnowledge);
    knowledgeLayout->addLayout(knowledgePathRow);
    knowledgePathEdit->setReadOnly(true);
    browseKnowledge->setText(QStringLiteral("存储向导…"));
    connect(browseKnowledge, &QPushButton::clicked, &dialog, [this, knowledgePathEdit] {
        showKnowledgeManager();
        knowledgePathEdit->setText(m_knowledgeDirectory);
    });
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
    auto *rechunkHint = new QLabel(QStringLiteral("修改片段长度或上下文重叠后，下次建库会重新处理全"
                                                  "部文档；PDF 可能重新渲染，OCR 页缓存可复用。"),
                                   knowledgeTab);
    rechunkHint->setWordWrap(true);
    rechunkHint->setProperty("settingsHint", true);
    knowledgeLayout->addWidget(rechunkHint);
    connect(knowledgeChunkSize, QOverload<int>::of(&QSpinBox::valueChanged), knowledgeOverlap,
            [knowledgeOverlap](int value) { knowledgeOverlap->setMaximum(value / 3); });
    auto *buildKnowledgeNow =
        new QCheckBox(QStringLiteral("保存后立即快速建库（仅本地处理，可断点重跑）"), knowledgeTab);
    knowledgeLayout->addWidget(buildKnowledgeNow);
    QString knowledgeStatus = QStringLiteral("尚未建立知识库");
    const QString existingKnowledgeDb = knowledgeDbPath();
    if (QFileInfo::exists(existingKnowledgeDb)) {
        knowledgeStatus =
            QStringLiteral("现有知识库：%1 MB · %2")
                .arg(QFileInfo(existingKnowledgeDb).size() / 1024.0 / 1024.0, 0, 'f', 1)
                .arg(existingKnowledgeDb);
    }
    const QString configuredKnowledgeDb =
        QDir(m_knowledgeDirectory).filePath(QStringLiteral("smart-space-knowledge.sqlite"));
    if (QFileInfo(existingKnowledgeDb).absoluteFilePath() !=
        QFileInfo(configuredKnowledgeDb).absoluteFilePath())
        knowledgeStatus += QStringLiteral("\n当前仍使用旧位置的知识库；选择新存储盘不会自动复制或删"
                                          "除旧库，可在向导显式建库或导出。 ");
    auto *knowledgeHint =
        new QLabel(knowledgeStatus +
                       QStringLiteral("\n快速建库不会调用网络，也不会修改 OCR/全文索引；"
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
    auto *skillTitle =
        new QLabel(QStringLiteral("将本地索引能力提供给 Codex、Antigravity 等 Agent"), skillTab);
    skillTitle->setProperty("settingsSectionTitle", true);
    skillLayout->addWidget(skillTitle);
    auto *skillHint = new QLabel(
        QStringLiteral("Skill 只负责调用本地索引和组织研究流程，模型由外部 Agent 自行决定。"
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
    connect(copySkillPath, &QPushButton::clicked, &dialog, [skillPath, skillStatus] {
        const QString path = skillPath->text().trimmed();
        if (path.isEmpty() || !QDir(path).exists()) {
            skillStatus->setText(QStringLiteral("Skill 目录不存在，请先完成索引 Skill 安装。"));
            return;
        }
        QApplication::clipboard()->setText(path);
        skillStatus->setText(QStringLiteral("Skill 路径已复制到剪贴板。"));
    });
    connect(exportSkill, &QPushButton::clicked, &dialog, [this, &dialog, skillPath, skillStatus] {
        const QString source = skillPath->text().trimmed();
        if (source.isEmpty() || !QDir(source).exists()) {
            skillStatus->setText(QStringLiteral("Skill 目录不存在，无法导出。"));
            return;
        }
        const QString destination = QFileDialog::getExistingDirectory(
            dialog.window(), QStringLiteral("选择 Skill 导出目录"));
        if (destination.isEmpty())
            return;
        const QString target = QDir(destination).filePath(QStringLiteral("ai-space-index-query"));
        if (QDir::cleanPath(source) == QDir::cleanPath(target)) {
            skillStatus->setText(QStringLiteral("目标目录就是当前 Skill，无需导出。"));
            return;
        }
        if (QDir(target).exists()) {
            const auto answer =
                smartQuestion(&dialog, QStringLiteral("确认覆盖"),
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
        const QString runtime =
            QDir(target).filePath(QStringLiteral("scripts/knowledge_library.py"));
        if (QFileInfo::exists(runtime))
            QFile::remove(runtime);
        if (!QFile::copy(QFileInfo(knowledgeWorkerPath())
                             .dir()
                             .filePath(QStringLiteral("knowledge_library.py")),
                         runtime)) {
            skillStatus->setText(QStringLiteral("无法导出独立检索运行时，请检查目标目录权限"));
            return;
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
    auto *hint =
        new QLabel(QStringLiteral("支持：PDF、DOCX、PPTX、XLSX、ODF、文本、图片 OCR；"
                                  "ET/XLS 优先只读解析，DOC/PPT/WPS/DPS 使用兼容文本提取。"),
                   providerTab);
    hint->setWordWrap(true);
    hint->setProperty("settingsHint", true);
    providerLayout->addWidget(hint);
    providerLayout->addStretch(1);
    tabs->addTab(providerTab, QStringLiteral("数据源"));

    connect(addRoot, &QPushButton::clicked, &dialog, [&dialog, rootList] {
        const QString path =
            QFileDialog::getExistingDirectory(dialog.window(), QStringLiteral("选择索引目录"));
        if (!path.isEmpty()) {
            const QList<QListWidgetItem *> found = rootList->findItems(path, Qt::MatchExactly);
            if (found.isEmpty())
                rootList->addItem(path);
        }
    });
    connect(removeRoot, &QPushButton::clicked, &dialog,
            [rootList] { qDeleteAll(rootList->selectedItems()); });
    connect(addExclude, &QPushButton::clicked, &dialog, [&dialog, rootList, excludeList] {
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
    connect(restoreExclude, &QPushButton::clicked, &dialog,
            [excludeList] { qDeleteAll(excludeList->selectedItems()); });
    connect(restoreHidden, &QPushButton::clicked, &dialog,
            [hiddenList] { qDeleteAll(hiddenList->selectedItems()); });
    auto setFormatChecks = [](QListWidget *list, Qt::CheckState state) {
        for (int row = 0; row < list->count(); ++row)
            list->item(row)->setCheckState(state);
    };
    connect(indexSelectAll, &QPushButton::clicked, &dialog,
            [indexFormatList, setFormatChecks] { setFormatChecks(indexFormatList, Qt::Checked); });
    connect(indexClear, &QPushButton::clicked, &dialog, [indexFormatList, setFormatChecks] {
        setFormatChecks(indexFormatList, Qt::Unchecked);
    });
    connect(indexSelectCommon, &QPushButton::clicked, &dialog, [indexFormatList] {
        const QSet<QString> common{
            QStringLiteral("pdf"), QStringLiteral("docx"), QStringLiteral("doc"),
            QStringLiteral("wps"), QStringLiteral("pptx"), QStringLiteral("ppt"),
            QStringLiteral("dps"), QStringLiteral("xlsx"), QStringLiteral("xls"),
            QStringLiteral("et"),  QStringLiteral("txt"),  QStringLiteral("md"),
            QStringLiteral("csv"), QStringLiteral("jpg"),  QStringLiteral("jpeg"),
            QStringLiteral("png")};
        for (int row = 0; row < indexFormatList->count(); ++row) {
            QListWidgetItem *item = indexFormatList->item(row);
            item->setCheckState(
                common.contains(item->data(Qt::UserRole).toString()) ? Qt::Checked : Qt::Unchecked);
        }
    });
    connect(visibleSelectAll, &QPushButton::clicked, &dialog, [visibleFormatList, setFormatChecks] {
        setFormatChecks(visibleFormatList, Qt::Checked);
    });
    connect(visibleClear, &QPushButton::clicked, &dialog, [visibleFormatList, setFormatChecks] {
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
        if (!path.isEmpty())
            providerEdit->setText(path);
    });
    connect(editProvider, &QPushButton::clicked, &dialog, [this, &dialog, providerEdit] {
        QString path = providerEdit->text().trimmed();
        if (path.isEmpty())
            path = providerConfigPath();
        QFile source(path);
        QByteArray value;
        if (source.open(QIODevice::ReadOnly))
            value = source.readAll();
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
        auto *editorButtons =
            new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &editor);
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
        if (m_libraryJobBusy) {
            smartInformation(this, QStringLiteral("任务正在运行"),
                             QStringLiteral("请先暂停建库，再保存资料和索引设置。"));
            return;
        }
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
        std::sort(
            excludedFolders.begin(), excludedFolders.end(),
            [](const QString &left, const QString &right) { return left.size() < right.size(); });
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
        const QStringList indexedExtensions = checkedFormats(indexFormatList, &noIndexFormats);
        const QStringList visibleExtensions = checkedFormats(visibleFormatList, &noVisibleFormats);
        if (noIndexFormats || noVisibleFormats) {
            smartInformation(this, QStringLiteral("AI空间"),
                             noIndexFormats ? QStringLiteral("索引格式至少需要保留一项。")
                                            : QStringLiteral("匹配文件显示格式至少需要保留一项。"));
            return;
        }
        const bool excludeChanged = excludedFolders != m_excludedFolders;
        const bool hiddenChanged = hiddenFolders != m_hiddenFolders;
        const bool visibleFormatsChanged = visibleExtensions != m_visibleExtensions;
        const bool indexInputsChanged = roots != m_roots || excludeChanged ||
                                        indexedExtensions != m_indexedExtensions ||
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
        m_knowledgeOverlap = qMin(knowledgeOverlap->value(), m_knowledgeChunkSize / 3);
        if (m_providerConfig != providerEdit->text().trimmed())
            m_providerApprovedDigest.clear();
        m_providerConfig = providerEdit->text().trimmed();
        if (m_libraryJobBusy)
            stopIdleFullIndex();
        m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(),
                                       [this](const SmartSpaceEntry &entry) {
                                           return pathExcluded(entry.path);
                                       }),
                        m_entries.end());
        configureIndexScheduling();
        applyTheme();
        saveSettings();
        if (syncKylin->isChecked()) {
            QDBusInterface indexService(QStringLiteral("com.ukui.search.fileindex.service"),
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
            QTimer::singleShot(0, this, [this] {
                setSettingsViewVisible(false);
                startIndexing();
            });
        } else if (indexInputsChanged) {
            m_statusLabel->setText(
                QStringLiteral("索引设置已保存 · 未自动扫描，点击底部“增量”后生效"));
        }
        if (buildKnowledgeNow->isChecked())
            QTimer::singleShot(0, this, [this] {
                setSettingsViewVisible(false);
                startKnowledgeBuild();
            });
        indexNow->setChecked(false);
        buildKnowledgeNow->setChecked(false);
        dialog.setProperty("settingsDirty", false);
        QTimer::singleShot(0, this, [this] { setSettingsViewVisible(false); });
    });
    styleModernCombos(form);
    return form;
}
