#pragma once

#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <functional>

// Small read/configuration requests only. Build jobs retain their process-group owner.
class JsonWorkerRequest final : public QObject {
    Q_OBJECT
public:
    struct Result {
        QJsonObject data;
        QString error;
        bool success = false;
        bool oversized = false;
        bool timedOut = false;
        bool canceled = false;
    };
    using Callback = std::function<void(const Result &)>;
    static JsonWorkerRequest *run(QObject *owner, const QString &script,
                                  const QStringList &arguments, Callback callback,
                                  int idleTimeoutMs = 30000, int maxBytes = 4 * 1024 * 1024);
    ~JsonWorkerRequest() override;
    void cancel();
signals:
    void progress(const QJsonObject &value);
private:
    explicit JsonWorkerRequest(QObject *parent);
    void readOutput();
    void consume(const QByteArray &line);
    void complete();
    QProcess *m_process;
    QTimer m_idle, m_total;
    QByteArray m_buffer, m_diagnostics;
    Callback m_callback;
    Result m_result;
    int m_received = 0, m_limit = 0;
    bool m_completed = false;
};
