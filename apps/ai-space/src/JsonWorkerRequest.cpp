#include "JsonWorkerRequest.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QPointer>
#include <QStandardPaths>

JsonWorkerRequest::JsonWorkerRequest(QObject *parent) : QObject(parent), m_process(new QProcess(this))
{
    m_idle.setSingleShot(true);
    m_total.setSingleShot(true);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &JsonWorkerRequest::readOutput);
    connect(m_process, &QProcess::readyReadStandardError, this, [this] {
        const auto bytes = m_process->readAllStandardError();
        m_diagnostics.append(bytes.left(qMax(0, 8192 - m_diagnostics.size())));
    });
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int, QProcess::ExitStatus) { complete(); });
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            m_result.error = QStringLiteral("无法启动知识库工具，请检查 Python 和应用安装：%1")
                                 .arg(m_process->errorString());
            complete();
        }
    });
    const auto timeout = [this] {
        m_result.timedOut = true;
        m_process->kill();
    };
    connect(&m_idle, &QTimer::timeout, this, timeout);
    connect(&m_total, &QTimer::timeout, this, timeout);
}

JsonWorkerRequest *JsonWorkerRequest::run(QObject *owner, const QString &script,
                                         const QStringList &arguments, Callback callback,
                                         int idleTimeoutMs, int maxBytes)
{
    auto *request = new JsonWorkerRequest(owner);
    request->m_callback = std::move(callback);
    request->m_limit = maxBytes;
    request->m_idle.setInterval(idleTimeoutMs);
    request->m_total.setInterval(qMax(300000, idleTimeoutMs));
    QTimer::singleShot(0, request, [request, script, arguments] {
        if (request->m_result.canceled) { request->complete(); return; }
        const QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
        if (python.isEmpty()) {
            request->m_result.error = QStringLiteral("找不到 Python 3，请安装后重试");
            request->complete();
            return;
        }
        QStringList command{script};
        command.append(arguments);
        request->m_process->start(python, command);
        request->m_idle.start();
        request->m_total.start();
    });
    return request;
}

void JsonWorkerRequest::readOutput()
{
    const auto bytes = m_process->readAllStandardOutput();
    m_received += bytes.size();
    if (m_received > m_limit) {
        m_result.oversized = true;
        m_buffer.clear();
        m_process->kill();
        return;
    }
    m_idle.start();
    m_buffer.append(bytes);
    int newline = 0;
    while ((newline = m_buffer.indexOf('\n')) >= 0) {
        const auto line = m_buffer.left(newline);
        m_buffer.remove(0, newline + 1);
        consume(line);
    }
}

void JsonWorkerRequest::consume(const QByteArray &line)
{
    const auto document = QJsonDocument::fromJson(line);
    if (!document.isObject())
        return;
    const auto value = document.object();
    if (value.contains(QStringLiteral("phase")) && !value.value(QStringLiteral("done")).toBool())
        emit progress(value);
    else
        m_result.data = value;
}

void JsonWorkerRequest::complete()
{
    if (m_completed)
        return;
    readOutput();
    consume(m_buffer);
    m_completed = true;
    m_idle.stop();
    m_total.stop();
    if (m_result.error.isEmpty()) {
        if (m_result.canceled)
            m_result.error = QStringLiteral("操作已取消");
        else if (m_result.oversized)
            m_result.error = QStringLiteral("后台返回内容超过限制，请缩小范围后重试");
        else if (m_result.timedOut)
            m_result.error = QStringLiteral("后台读取超时，请确认存储设备已连接后重试");
        else if (!m_result.data.value(QStringLiteral("error")).toString().isEmpty())
            m_result.error = m_result.data.value(QStringLiteral("errorDescription")).toString(
                m_result.data.value(QStringLiteral("error")).toString());
        else if (m_process->exitStatus() != QProcess::NormalExit || m_process->exitCode() != 0)
            m_result.error = QStringLiteral("后台工具异常退出：%1")
                                 .arg(QString::fromUtf8(m_diagnostics).trimmed().left(500));
        else if (m_result.data.isEmpty())
            m_result.error = QStringLiteral("后台未返回有效数据，请检查安装或刷新重试");
    }
    m_result.success = m_result.error.isEmpty();
    const QPointer<JsonWorkerRequest> guard(this);
    auto callback = std::move(m_callback);
    if (callback)
        callback(m_result);
    if (guard)
        deleteLater();
}

void JsonWorkerRequest::cancel()
{
    m_result.canceled = true;
    if (m_process->state() == QProcess::NotRunning) complete();
    else m_process->kill();
}

JsonWorkerRequest::~JsonWorkerRequest()
{
    if (m_process->state() != QProcess::NotRunning) {
        m_process->disconnect(this);
        m_process->setParent(QCoreApplication::instance());
        connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                m_process, &QObject::deleteLater);
        m_process->kill();
    }
}
