#include "JsonWorkerRequest.h"
#include "KnowledgeJobController.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QVariant>
#include <iostream>
#include <signal.h>

static void require(bool condition, const char *message)
{
    if (!condition) { std::cerr << message << std::endl; std::exit(1); }
}
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir temp;
    require(temp.isValid(), "temporary directory");
    auto script = [&](const QString &name, const QByteArray &body) {
        const auto path=temp.filePath(name);QFile file(path);
        require(file.open(QIODevice::WriteOnly), "write fixture");file.write(body);return path;
    };
    auto request = [&](const QString &path,int idle=1000,int cap=4096,bool cancel=false) {
        QEventLoop loop;JsonWorkerRequest::Result answer;int callbacks=0;
        auto *job=JsonWorkerRequest::run(&loop,path,{},[&](const JsonWorkerRequest::Result &result){answer=result;++callbacks;loop.quit();},idle,cap);
        if(cancel)job->cancel();
        QTimer::singleShot(5000,&loop,&QEventLoop::quit);loop.exec();
        require(callbacks==1,"request must complete exactly once");return answer;
    };
    auto ok=script("ok.py","import json\nprint(json.dumps({'phase':'verify'}),flush=True)\nprint(json.dumps({'done':True,'value':4}),flush=True)\n");
    require(request(ok).success,"progress followed by final JSON");
    auto error=script("error.py","print('{\"done\":true,\"error\":\"工具不可用\"}')\n");
    require(!request(error).success,"worker error is visible");
    auto huge=script("huge.py","print('x'*20000)\n");
    require(request(huge,1000,128).oversized,"oversize distinguished");
    auto hung=script("hung.py","import time\ntime.sleep(60)\n");
    require(request(hung,100).timedOut,"timeout distinguished");
    require(request(hung,1000,4096,true).canceled,"cancel before launch");
    require(!request(temp.filePath("missing.py")).success,"missing worker visible");
    const auto saved=qgetenv("PATH");qputenv("PATH",temp.path().toUtf8());
    require(!request(ok).success,"missing python visible");qputenv("PATH",saved);
    // A leader may exit on TERM while a descendant ignores it. Closing the
    // controller must still reap the owned group rather than leave it running.
    auto descendant=script("descendant.py","import os,signal,time\npid=os.fork()\nif pid==0:\n signal.signal(signal.SIGTERM,signal.SIG_IGN)\n while True:time.sleep(1)\nelse:\n print(pid,flush=True)\n while True:time.sleep(1)\n");
    qint64 child=0;
    {
        KnowledgeJobController controller;
        auto *process=controller.create();process->setProperty("usesProcessGroup",true);
        process->start(QStandardPaths::findExecutable("setsid"),{QStandardPaths::findExecutable("python3"),descendant});
        require(process->waitForReadyRead(3000),"descendant launched");child=process->readAllStandardOutput().trimmed().toLongLong();
        require(child>1,"valid descendant pid");
        controller.stop();require(process->waitForFinished(3000),"leader terminates");
    }
    // A dead process may briefly be a zombie until init reaps it. Its /proc
    // status must therefore be absent or Z, never sleeping/running.
    QEventLoop settle;QTimer::singleShot(100,&settle,&QEventLoop::quit);settle.exec();
    QFile status(QString("/proc/%1/stat").arg(child));
    require(!status.open(QIODevice::ReadOnly) || status.readAll().contains(") Z"),"no surviving descendant");
    std::cout << "JSON protocol, cancellation and descendant cleanup passed\n";
}
