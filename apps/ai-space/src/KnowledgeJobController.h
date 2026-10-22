#pragma once
#include <QObject>
#include <QPointer>
#include <QProcess>

// One owner and one cancellation path for library workers and descendants.
class KnowledgeJobController final : public QObject {
public:
    explicit KnowledgeJobController(QObject *parent=nullptr):QObject(parent){}
    ~KnowledgeJobController() override;
    QProcess *create();
    void stop(bool force=false);
    bool active()const{return m_process && m_process->state()!=QProcess::NotRunning;}
private:
    QPointer<QProcess> m_process;
    void signalProcess(QProcess *process,bool force);
};
