#pragma once
#include <QDialog>
#include <QJsonArray>
#include <QPointer>
class QCheckBox;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class JsonWorkerRequest;
class QPushButton;
class QTableWidget;

class KnowledgeFailuresDialog final : public QDialog
{
    Q_OBJECT
  public:
    enum { Retry = 2 };
    KnowledgeFailuresDialog(const QString &db, const QString &worker, bool light, int cpu,
                            int memory, QWidget *parent);
    ~KnowledgeFailuresDialog() override;
    QJsonArray retryIds() const { return m_retryIds; }
    bool includeOcr() const;
    bool backgroundBusy() const { return m_busy; }
  signals:
    void backgroundBusyChanged(bool busy);

  private:
    void load();
    void start(const QStringList &arguments, bool mutation = false);
    void updateActions();
    void changePolicy(bool ignore);
    void selectAllMatching();
    QJsonArray selectedIds() const;
    QString m_db, m_worker, m_notice, m_selectionToken;
    QPointer<JsonWorkerRequest> m_request;
    bool m_busy = false, m_writable = false, m_allSelected = false;
    int m_offset = 0, m_total = 0;
    QJsonArray m_retryIds, m_matchingIds;
    QComboBox *m_filter, *m_category;
    QTableWidget *m_table;
    QPlainTextEdit *m_detail;
    QCheckBox *m_ocr;
    QLabel *m_message, *m_page, *m_selection;
    QPushButton *m_retry, *m_ignore, *m_restore, *m_previous, *m_next, *m_refresh, *m_selectAll;
};
