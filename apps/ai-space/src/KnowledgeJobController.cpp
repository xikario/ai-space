#include "KnowledgeJobController.h"
#include <QTimer>
#include <QVariant>
#include <signal.h>
#include <unistd.h>

QProcess *KnowledgeJobController::create()
{
    if (active()) return nullptr;
    auto *process = new QProcess(this);
    m_process = process;
    connect(process, &QProcess::started, this, [this, process] {
        const qint64 pid = process->processId();
        if (!process->property("usesProcessGroup").toBool() || pid <= 1) return;
        // setsid is the launch wrapper. Its PID names the private session/group;
        // retain ownership after the leader exits so descendants are reclaimed.
        m_groups.insert(pid);
        process->setProperty("ownedGroup", pid);
    });
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, process](int, QProcess::ExitStatus) {
        const qint64 group = process->property("ownedGroup").toLongLong();
        if (group > 1) {
            ::kill(-group, SIGKILL);
            m_groups.remove(group);
        }
    });
    return process;
}

void KnowledgeJobController::signalProcess(QProcess *process, bool force)
{
    if (!process || process->state() == QProcess::NotRunning) return;
    const auto pid = process->processId();
    if (process->property("usesProcessGroup").toBool() && pid > 1 && getpgid(pid) == pid) {
        m_groups.insert(pid);
        ::kill(-pid, force ? SIGKILL : SIGTERM);
        if (!force) ::kill(-pid, SIGCONT);
    } else if (force) process->kill();
    else process->terminate();
}

void KnowledgeJobController::stop(bool force)
{
    signalProcess(m_process, force);
    if (force) {
        for (const auto group : m_groups) ::kill(-group, SIGKILL);
    } else {
        const auto groups = m_groups;
        QTimer::singleShot(3500, this, [this, groups] {
            for (const auto group : groups)
                if (m_groups.contains(group)) ::kill(-group, SIGKILL);
        });
    }
}

KnowledgeJobController::~KnowledgeJobController()
{
    stop(false);
    if (active() && !m_process->waitForFinished(4000)) {
        signalProcess(m_process, true);
        m_process->waitForFinished(1000);
    }
    for (const auto group : m_groups) ::kill(-group, SIGKILL);
}
