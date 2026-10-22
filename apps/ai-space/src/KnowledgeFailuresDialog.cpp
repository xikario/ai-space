#include "KnowledgeFailuresDialog.h"
#include "JsonWorkerRequest.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPalette>
#include <QProcess>
#include <QPushButton>
#include <QSignalBlocker>
#include <QShortcut>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <memory>

KnowledgeFailuresDialog::KnowledgeFailuresDialog(const QString &db, const QString &worker,
                                                 bool light, int cpu, int memory, QWidget *parent)
    : QDialog(parent, Qt::Dialog | Qt::WindowTitleHint | Qt::WindowMinimizeButtonHint |
                          Qt::WindowCloseButtonHint),
      m_db(db), m_worker(worker)
{
    setObjectName("knowledgeFailuresDialog");
    setWindowTitle(QStringLiteral("知识库 · 失败项与忽略"));
    setWindowIcon(qApp->windowIcon());
    resize(940, 640);
    const QString bg = light ? "#f1f5f9" : "#111827", fg = light ? "#0f172a" : "#e2e8f0",
                  field = light ? "#ffffff" : "#0b1120", border = light ? "#cbd5e1" : "#334155";
    setStyleSheet(
        QString(
            "QDialog{background:%1;} QLabel,QCheckBox{color:%2;} "
            "QTableWidget,QPlainTextEdit,QComboBox{background:%3;color:%2;border:1px solid "
            "%4;border-radius:6px;} QComboBox QAbstractItemView{background:%3;color:%2;border:1px "
            "solid %4;selection-background-color:#6366f1;selection-color:white;} "
            "QTableWidget::item:selected{background:#6366f1;color:white;} "
            "QHeaderView::section{background:%1;color:%2;border:0;padding:6px;} "
            "QPushButton{padding:7px 10px;color:%2;background:%3;border:1px solid "
            "%4;border-radius:6px;} QPushButton:disabled{color:#64748b;} QComboBox{padding:6px;}")
            .arg(bg, fg, field, border));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);
    auto *hint = new QLabel(QStringLiteral(
        "重试仅重新检查所选文件；忽略保留库内已有正文，跳过后续自动重试。可在“已忽略”中恢复。"));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *top = new QHBoxLayout;
    m_filter = new QComboBox;
    m_filter->setObjectName("failureFilter");
    m_filter->addItem(QStringLiteral("待处理失败 / 来源缺失"), false);
    m_filter->addItem(QStringLiteral("已忽略"), true);
    top->addWidget(m_filter);
    m_category = new QComboBox;
    m_category->setObjectName("failureCategoryFilter");
    m_category->addItem(QStringLiteral("全部原因"), QStringLiteral("all"));
    m_category->setMinimumWidth(210);
    top->addWidget(m_category);
    top->addStretch();
    m_refresh = new QPushButton(QStringLiteral("刷新"));
    top->addWidget(m_refresh);
    layout->addLayout(top);
    m_table = new QTableWidget(0, 3);
    m_table->setObjectName("failureTable");
    m_table->setShowGrid(false);
    m_table->setHorizontalHeaderLabels(
        {QStringLiteral("文件"), QStringLiteral("状态"), QStringLiteral("失败原因")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->hide();
    m_table->setWordWrap(false);
    m_table->setColumnWidth(0, 235);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    layout->addWidget(m_table, 1);
    m_detail = new QPlainTextEdit;
    m_detail->setObjectName("failureReasonDetails");
    m_detail->setReadOnly(true);
    m_detail->setMaximumHeight(110);
    m_detail->setPlaceholderText(
        QStringLiteral("选中文件查看完整路径、原因和最近尝试时间；支持多选。"));
    layout->addWidget(m_detail);
    auto detailPalette = m_detail->palette();
    detailPalette.setColor(QPalette::PlaceholderText, QColor(light ? "#64748b" : "#94a3b8"));
    m_detail->setPalette(detailPalette);
    auto *pages = new QHBoxLayout;
    auto *select = new QPushButton(QStringLiteral("全选本页"));
    pages->addWidget(select);
    m_selectAll = new QPushButton(QStringLiteral("全选筛选结果"));
    m_selectAll->setObjectName("failureSelectAllMatching");
    m_selectAll->setShortcut(QKeySequence("Alt+A"));
    pages->addWidget(m_selectAll);
    m_selection = new QLabel;
    m_selection->setObjectName("failureSelectionCount");
    pages->addWidget(m_selection);
    pages->addStretch();
    m_previous = new QPushButton(QStringLiteral("上一页"));
    m_next = new QPushButton(QStringLiteral("下一页"));
    m_previous->setShortcut(QKeySequence("Alt+Left"));
    m_next->setShortcut(QKeySequence("Alt+Right"));
    m_page = new QLabel;
    pages->addWidget(m_previous);
    pages->addWidget(m_page);
    pages->addWidget(m_next);
    layout->addLayout(pages);
    m_ocr = new QCheckBox(
        QStringLiteral("需要时补 OCR（单核 %1% · 工作进程 %2 MiB）").arg(cpu).arg(memory));
    m_ocr->setObjectName("failureRetryOcr");
    m_ocr->setChecked(true);
    layout->addWidget(m_ocr);
    m_message = new QLabel;
    m_message->setObjectName("failureActionMessage");
    m_message->setWordWrap(true);
    layout->addWidget(m_message);
    auto *actions = new QHBoxLayout;
    m_retry = new QPushButton(QStringLiteral("重新处理选中"));
    m_retry->setObjectName("failureRetryButton");
    m_ignore = new QPushButton(QStringLiteral("忽略选中"));
    m_ignore->setObjectName("failureIgnoreButton");
    m_restore = new QPushButton(QStringLiteral("取消忽略"));
    m_restore->setObjectName("failureRestoreButton");
    actions->addWidget(m_retry);
    actions->addWidget(m_ignore);
    actions->addWidget(m_restore);
    actions->addStretch();
    auto *close = new QPushButton(QStringLiteral("关闭"));
    actions->addWidget(close);
    layout->addLayout(actions);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    auto selectPage = [this] {
        m_allSelected = false;
        m_table->selectAll();
        updateActions();
    };
    connect(select, &QPushButton::clicked, this, selectPage);
    auto *selectPageShortcut = new QShortcut(QKeySequence::SelectAll, m_table);
    selectPageShortcut->setContext(Qt::WidgetShortcut);
    connect(selectPageShortcut, &QShortcut::activated, this, selectPage);
    connect(m_selectAll, &QPushButton::clicked, this, &KnowledgeFailuresDialog::selectAllMatching);
    connect(m_refresh, &QPushButton::clicked, this, [this] {
        m_allSelected = false;
        m_notice.clear();
        load();
    });
    auto resetFilter = [this] {
        m_allSelected = false;
        m_offset = 0;
        m_notice.clear();
        load();
    };
    connect(m_filter, QOverload<int>::of(&QComboBox::currentIndexChanged), this, resetFilter);
    connect(m_category, QOverload<int>::of(&QComboBox::currentIndexChanged), this, resetFilter);
    connect(m_previous, &QPushButton::clicked, this, [this] {
        m_offset = qMax(0, m_offset - 50);
        load();
    });
    connect(m_next, &QPushButton::clicked, this, [this] {
        m_offset += 50;
        load();
    });
    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this] {
        m_allSelected = false;
        const auto selected = m_table->selectionModel()->selectedRows();
        m_detail->clear();
        if (!selected.isEmpty()) {
            const auto row =
                m_table->item(selected.first().row(), 0)->data(Qt::UserRole).toJsonObject();
            m_detail->setPlainText(row.value("path").toString() + QStringLiteral("\n问题分类：") +
                                   row.value("failure_category_label").toString() + "\n" +
                                   row.value("reason").toString() + QStringLiteral("\n最近尝试：") +
                                   row.value("attempted_at").toString());
        }
        updateActions();
    });
    connect(m_ignore, &QPushButton::clicked, this, [this] { changePolicy(true); });
    connect(m_restore, &QPushButton::clicked, this, [this] { changePolicy(false); });
    connect(m_retry, &QPushButton::clicked, this, [this] {
        m_retryIds = selectedIds();
        if (!m_retryIds.isEmpty())
            done(Retry);
    });
    m_retry->setShortcut(QKeySequence("Alt+R"));
    m_ignore->setShortcut(QKeySequence("Alt+I"));
    m_restore->setShortcut(QKeySequence("Alt+U"));
    m_ocr->setShortcut(QKeySequence("Alt+O"));
    connect(new QShortcut(QKeySequence("Alt+1"), this), &QShortcut::activated, this, [this] {
        if (!m_busy)
            m_filter->setCurrentIndex(0);
    });
    connect(new QShortcut(QKeySequence("Alt+2"), this), &QShortcut::activated, this, [this] {
        if (!m_busy)
            m_filter->setCurrentIndex(1);
    });
    connect(new QShortcut(QKeySequence("Alt+C"), this), &QShortcut::activated, this, [this] {
        if (!m_busy)
            m_category->setFocus();
    });
    updateActions();
    load();
}

KnowledgeFailuresDialog::~KnowledgeFailuresDialog() = default;

bool KnowledgeFailuresDialog::includeOcr() const
{
    return m_ocr->isChecked();
}
QJsonArray KnowledgeFailuresDialog::selectedIds() const
{
    if (m_allSelected)
        return m_matchingIds;
    QJsonArray result;
    for (const auto &index : m_table->selectionModel()->selectedRows())
        result.append(
            m_table->item(index.row(), 0)->data(Qt::UserRole).toJsonObject().value("document_id"));
    return result;
}
void KnowledgeFailuresDialog::selectAllMatching()
{
    if (m_busy || m_total <= 0)
        return;
    m_allSelected = !m_allSelected;
    {
        QSignalBlocker block(m_table);
        if (m_allSelected)
            m_table->selectAll();
        else
            m_table->clearSelection();
    }
    updateActions();
}
void KnowledgeFailuresDialog::updateActions()
{
    const bool ignored = m_filter->currentData().toBool();
    const int count = m_allSelected ? m_total : m_table->selectionModel()->selectedRows().size();
    const bool selected = count > 0;
    m_retry->setEnabled(!m_busy && m_writable && selected && count <= 200);
    m_ocr->setEnabled(!m_busy && m_writable);
    m_retry->setToolTip(
        count > 200
            ? QStringLiteral("重新处理每次最多 200 项，请按页选择；批量忽略支持全部筛选结果。")
            : QString());
    m_ignore->setVisible(!ignored);
    m_restore->setVisible(ignored);
    m_ignore->setText(selected ? QStringLiteral("忽略选中（%1）").arg(count)
                               : QStringLiteral("忽略选中"));
    m_restore->setText(selected ? QStringLiteral("取消忽略（%1）").arg(count)
                                : QStringLiteral("取消忽略"));
    // setText replaces a button's shortcut, including an explicitly set one.
    m_ignore->setShortcut(QKeySequence("Alt+I"));
    m_restore->setShortcut(QKeySequence("Alt+U"));
    m_ignore->setEnabled(!m_busy && m_writable && selected);
    m_restore->setEnabled(!m_busy && m_writable && selected);
    m_filter->setEnabled(!m_busy);
    m_category->setEnabled(!m_busy);
    m_table->setEnabled(!m_busy);
    m_refresh->setEnabled(!m_busy);
    m_previous->setEnabled(!m_busy && m_offset > 0);
    m_next->setEnabled(!m_busy && m_offset + 50 < m_total);
    m_selectAll->setEnabled(!m_busy && m_total > 0);
    m_selectAll->setText(m_allSelected ? QStringLiteral("取消跨页全选")
                                       : QStringLiteral("全选筛选结果"));
    m_selectAll->setShortcut(QKeySequence("Alt+A"));
    m_selection->setText(m_allSelected ? QStringLiteral("跨页已选 %1 项").arg(count)
                                       : QStringLiteral("已选 %1 项").arg(count));
    setProperty("failureSelectedCount", count);
    setProperty("failureAllSelected", m_allSelected);
}
void KnowledgeFailuresDialog::load()
{
    QStringList args{QStringLiteral("failures"),
                     QStringLiteral("--offset"),
                     QString::number(m_offset),
                     QStringLiteral("--limit"),
                     QStringLiteral("50"),
                     QStringLiteral("--category"),
                     m_category->currentData().toString()};
    if (m_filter->currentData().toBool())
        args << QStringLiteral("--ignored");
    start(args);
}
void KnowledgeFailuresDialog::changePolicy(bool ignore)
{
    if (m_allSelected) {
        start({QStringLiteral("failure-policy"), QStringLiteral("--all-matching"),
               QStringLiteral("--category"), m_category->currentData().toString(),
               QStringLiteral("--expected-selection"), m_selectionToken,
               QStringLiteral("--expected-count"), QString::number(m_total),
               QStringLiteral("--action"),
               ignore ? QStringLiteral("ignore") : QStringLiteral("restore")},
              true);
        return;
    }
    const auto ids = selectedIds();
    if (ids.isEmpty())
        return;
    start({QStringLiteral("failure-policy"), QStringLiteral("--document-ids-json"),
           QString::fromUtf8(QJsonDocument(ids).toJson(QJsonDocument::Compact)),
           QStringLiteral("--action"),
           ignore ? QStringLiteral("ignore") : QStringLiteral("restore")},
          true);
}
void KnowledgeFailuresDialog::start(const QStringList &arguments, bool mutation)
{
    if (m_busy)
        return;
    m_busy = true;
    emit backgroundBusyChanged(true);
    setProperty("failureListLoaded", false);
    setProperty("failureListIgnored", m_filter->currentData().toBool());
    setProperty("failureListCategory", m_category->currentData().toString());
    updateActions();
    m_message->setText(mutation ? QStringLiteral("正在保存处理方式…")
                                : QStringLiteral("正在读取失败记录…"));
    QStringList args{QStringLiteral("--db"), m_db};
    args.append(arguments);
    m_request = JsonWorkerRequest::run(
        this, m_worker, args, [this, mutation](const JsonWorkerRequest::Result &result) {
            m_request = nullptr;
            m_busy = false;
            emit backgroundBusyChanged(false);
            const auto data = result.data;
            if (!result.success) {
                m_allSelected = false;
                {
                    QSignalBlocker block(m_table);
                    m_table->clearSelection();
                }
                m_message->setText(QStringLiteral("操作未完成：%1").arg(result.error));
                updateActions();
                return;
            }
            if (mutation) {
                m_allSelected = false;
                m_offset = 0;
                m_notice =
                    data.value("ignored").toBool()
                        ? QStringLiteral("已忽略 %1 项；已有正文保留。")
                              .arg(data.value("changed").toInt())
                        : QStringLiteral("已取消 %1 项忽略，可选择重新处理或等待下一次更新。")
                              .arg(data.value("changed").toInt());
                load();
                return;
            }
            const QString token = data.value("selection_token").toString();
            if (m_allSelected && token != m_selectionToken) {
                m_allSelected = false;
                m_notice = QStringLiteral("筛选结果已变化，已取消跨页全选；请核对后重新选择。");
            }
            m_selectionToken = token;
            m_matchingIds = data.value("selection_ids").toArray();
            {
                QSignalBlocker block(m_category);
                const QString selected = m_category->currentData().toString();
                m_category->clear();
                for (const auto &value : data.value("categories").toArray()) {
                    const auto category = value.toObject();
                    const QString key = category.value("id").toString();
                    const int count = category.value("count").toInt();
                    if (key == "all" || key == selected || count > 0)
                        m_category->addItem(QStringLiteral("%1（%2）")
                                                .arg(category.value("label").toString())
                                                .arg(count),
                                            key);
                }
                const int index = m_category->findData(selected);
                m_category->setCurrentIndex(qMax(0, index));
            }
            m_writable = data.value("writable").toBool();
            m_total = data.value("total").toInt();
            setProperty("failureListTotal", m_total);
            setProperty("failureListOffset", m_offset);
            const auto rows = data.value("results").toArray();
            setProperty("failureListLoaded", true);
            if (rows.isEmpty() && m_total > 0 && m_offset >= m_total) {
                m_offset = ((m_total - 1) / 50) * 50;
                load();
                return;
            }
            {
                QSignalBlocker block(m_table);
                m_table->setRowCount(0);
                for (const auto &value : rows) {
                    const auto row = value.toObject();
                    const int index = m_table->rowCount();
                    m_table->insertRow(index);
                    auto *name = new QTableWidgetItem(row.value("name").toString());
                    name->setData(Qt::UserRole, row);
                    name->setToolTip(row.value("path").toString());
                    m_table->setItem(index, 0, name);
                    m_table->setItem(index, 1,
                                     new QTableWidgetItem(
                                         row.value("ignored").toBool() ? QStringLiteral("已忽略")
                                         : row.value("status").toString() == "missing"
                                             ? QStringLiteral("来源缺失")
                                             : QStringLiteral("提取失败")));
                    auto *reason = new QTableWidgetItem(row.value("reason").toString());
                    reason->setToolTip(reason->text());
                    m_table->setItem(index, 2, reason);
                }
                if (m_allSelected)
                    m_table->selectAll();
            }
            m_detail->clear();
            m_page->setText(m_total ? QStringLiteral("%1–%2 / %3 项")
                                          .arg(m_offset + 1)
                                          .arg(m_offset + rows.size())
                                          .arg(m_total)
                                    : QStringLiteral("0 项"));
            m_message->setText(
                !m_writable
                    ? QStringLiteral("共享知识库只读；请在资料来源机器处理失败项，再导出新版本。")
                : !m_notice.isEmpty() ? m_notice
                : m_total             ? QStringLiteral("可按原因筛选；“全选筛选结果”包括其他页。忽略和重试均保"
                                                                   "留失败时的旧正文。")
                                      : QStringLiteral("当前分类没有记录。"));
            updateActions();
            m_table->setFocus();
        });
    connect(m_request, &JsonWorkerRequest::progress, this, [this](const QJsonObject &) {
        m_message->setText(QStringLiteral("正在检查旧版失败记录，请稍候…"));
    });
}
