#include "KnowledgeJobController.h"
#include <QTimer>
#include <QVariant>
#include <signal.h>
#include <unistd.h>

QProcess *KnowledgeJobController::create()
{
    if(active())return nullptr;
    auto *process=new QProcess(this);m_process=process;return process;
}
void KnowledgeJobController::signalProcess(QProcess *process,bool force)
{
    if(!process || process->state()==QProcess::NotRunning)return;
    const auto pid=process->processId();
    if(process->property("usesProcessGroup").toBool() && pid>1 && getpgid(pid)==pid){
        ::kill(-pid,force?SIGKILL:SIGTERM);
        if(!force)::kill(-pid,SIGCONT);
    }else if(force)process->kill();else process->terminate();
}
void KnowledgeJobController::stop(bool force)
{
    if(!active())return;
    auto process=m_process;const auto pid=process->processId();signalProcess(process,force);
    if(!force)QTimer::singleShot(3500,this,[this,process,pid]{
        if(process && process==m_process && process->processId()==pid)signalProcess(process,true);
    });
}
KnowledgeJobController::~KnowledgeJobController()
{
    if(!active())return;
    signalProcess(m_process,false);
    if(m_process && !m_process->waitForFinished(4000)){signalProcess(m_process,true);if(m_process)m_process->waitForFinished(1000);}
}
