#pragma once
#include <QDialog>
#include <QStringList>
class QPlainTextEdit;
class QLineEdit;
class QSpinBox;
class QCheckBox;
class QLabel;
class KnowledgeManagerDialog final : public QDialog
{
    Q_OBJECT
  public:
    enum Action { Save = 1, Import = 2, Export = 3, Build = 4, Ocr = 5 };
    KnowledgeManagerDialog(const QString &directory, const QString &personalDb,
                           const QStringList &roots, int reserve, int quota, int cpu, int memory,
                           const QString &languages, bool light, const QString &worker,
                           QWidget *parent);
    QString directory() const;
    QStringList roots() const;
    int reserveGiB() const;
    int quotaGiB() const;
    int ocrCpu() const;
    int ocrMemory() const;
    QString ocrLanguages() const;
    bool backgroundBusy() const { return m_managementBusy; }
  signals:
    void backgroundBusyChanged(bool busy);

  public:
    bool includeOcr() const;
    bool mountOnly() const;

  protected:
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;

  private:
    QPoint m_dragOffset;
    bool m_dragging = false;
    bool validate(int action);
    void updateSpace();
    QLineEdit *m_directory;
    QPlainTextEdit *m_roots;
    QLineEdit *m_languages;
    QSpinBox *m_reserve;
    QSpinBox *m_quota;
    QSpinBox *m_cpu;
    QSpinBox *m_memory;
    QCheckBox *m_ocr;
    QCheckBox *m_mount;
    QLabel *m_space;
    QLabel *m_message;
    bool m_managementBusy = false;
    QString m_notice;
};
