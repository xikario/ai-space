#include "Migration.h"
#include <QSettings>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryFile>
#include <QThread>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCryptographicHash>

MigrationResult migrateLegacyData(bool includeKnowledge)
{
    MigrationResult result;
    QSettings settings;
    // Earlier upstream Qt builds used NativeFormat (.conf on Linux), while
    // OEM Qt builds used .ini. Preserve the former only when no .ini exists.
    const QString previousConfig =
        QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) +
        QStringLiteral("/xikario/ai-space.conf");
    if (!QFileInfo::exists(settings.fileName()) && QFileInfo::exists(previousConfig)) {
        QSettings previous(previousConfig, QSettings::IniFormat);
        for (const QString &key : previous.allKeys())
            settings.setValue(key, previous.value(key));
        settings.sync();
        if (previous.status() != QSettings::NoError || settings.status() != QSettings::NoError) {
            result.ok = false;
            result.error =
                QStringLiteral("无法导入旧版 .conf 设置；原配置已保留，请检查配置目录权限。");
            return result;
        }
    }
    if (settings.value("migration/legacyV1", false).toBool())
        return result;
    const QString config = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    const QString data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    const QString cache = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    QSettings legacy(config + "/kylin/ukui-fences.ini", QSettings::IniFormat);
    legacy.beginGroup("smartSpace");
    const bool importPreferences = !settings.value("migration/legacyPreferencesV1", false).toBool();
    if (importPreferences) {
        for (const QString &key : legacy.allKeys()) {
            if (key == "autoStart" || key == "knowledgeDirectory" || key == "providerConfig" ||
                key.startsWith("ai") || key == "knowledgeEnabled" ||
                key == "knowledgeSendContent" || key == "currentSpace")
                continue;
            if (!settings.contains("smartSpace/" + key))
                settings.setValue("smartSpace/" + key, legacy.value(key));
        }
        const auto copy = [&](const QString &source, const QString &target) {
            if (!QFileInfo(source).isFile())
                return true;
            if (QFileInfo::exists(target))
                return QFileInfo(target).isFile();
            QFile input(source);
            if (!input.open(QIODevice::ReadOnly) ||
                !QDir().mkpath(QFileInfo(target).absolutePath()))
                return false;
            QSaveFile output(target);
            if (!output.open(QIODevice::WriteOnly))
                return false;
            output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
            while (!input.atEnd()) {
                if (QThread::currentThread()->isInterruptionRequested())
                    return false;
                const QByteArray bytes = input.read(65536);
                if ((bytes.isEmpty() && input.error() != QFileDevice::NoError) ||
                    output.write(bytes) != bytes.size())
                    return false;
            }
            if (!output.commit())
                return false;
            ++result.copied;
            return true;
        };
        const QString oldCache = cache + "/kylin/ukui-fences/smart-space";
        const QString newCache =
            QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/smart-space";
        const QStringList snapshots{"index.json", "index.json.ui.bin.gz", "index.json.previous",
                                    "index.json.fast.resume.jsonl", "index.json.ocr.resume.jsonl"};
        for (const auto &file :
             QDir(oldCache).entryInfoList(snapshots, QDir::Files | QDir::NoSymLinks)) {
            if (!copy(file.absoluteFilePath(), newCache + '/' + file.fileName()))
                result.ok = false;
        }
        const QString providers = config + "/ai-space/providers.json";
        const QString oldProviders =
            legacy.value("providerConfig", config + "/kyfences/smart-space-providers.json")
                .toString();
        if (QFileInfo(oldProviders).isFile() && !QFileInfo::exists(providers)) {
            QFile source(oldProviders);
            if (!source.open(QIODevice::ReadOnly))
                result.ok = false;
            else {
                const auto bytes = source.readAll();
                auto document = QJsonDocument::fromJson(bytes);
                if (document.isObject()) {
                    auto object = document.object();
                    auto entries = object.value("providers").toArray();
                    for (int i = 0; i < entries.size(); ++i) {
                        if (!entries[i].isObject())
                            continue;
                        auto entry = entries[i].toObject();
                        const QString inherited = entry.value("inheritFrom").toString().trimmed();
                        if (inherited.isEmpty())
                            continue;
                        // Keep include semantics after relocating the provider JSON.
                        // Environment/home expansion remains owned by the worker.
                        if (inherited.contains('$') || inherited.startsWith('~'))
                            continue;
                        const QString original = QDir(QFileInfo(oldProviders).absolutePath())
                                                     .absoluteFilePath(inherited);
                        const QString name =
                            QString::fromLatin1(QCryptographicHash::hash(original.toUtf8(),
                                                                         QCryptographicHash::Sha256)
                                                    .toHex()) +
                            ".json";
                        if (QFileInfo(original).isFile() &&
                            copy(original,
                                 QFileInfo(providers).absolutePath() + "/providers.d/" + name))
                            entry.insert("inheritFrom", "providers.d/" + name);
                        else
                            result.ok = false;
                        entries[i] = entry;
                    }
                    object.insert("providers", entries);
                    document.setObject(object);
                }
                if (result.ok) {
                    QDir().mkpath(QFileInfo(providers).absolutePath());
                    QSaveFile output(providers);
                    const auto payload = document.isObject() ? document.toJson() : bytes;
                    if (!output.open(QIODevice::WriteOnly))
                        result.ok = false;
                    else {
                        output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
                        if (output.write(payload) != payload.size() || !output.commit())
                            result.ok = false;
                    }
                }
            }
        }
        if (!settings.contains("smartSpace/providerConfig"))
            settings.setValue("smartSpace/providerConfig", providers);
        if (result.ok)
            settings.setValue("migration/legacyPreferencesV1", true);
    }
    const QString knowledge = data + "/ai-space/knowledge";
    const QString oldDb =
        legacy.value("knowledgeDirectory", data + "/ukui-fences/knowledge").toString() +
        "/smart-space-knowledge.sqlite";
    const QString newDb = knowledge + "/smart-space-knowledge.sqlite";
    result.knowledgePending = QFileInfo(oldDb).isFile() && !QFileInfo::exists(newDb);
    if (includeKnowledge && result.knowledgePending) {
        QDir().mkpath(knowledge);
        QFile::setPermissions(knowledge, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                             QFileDevice::ExeOwner);
        QTemporaryFile staging(knowledge + "/.import-XXXXXX.sqlite");
        if (!staging.open()) {
            result.ok = false;
            result.error = QStringLiteral("无法创建知识库副本。");
            return result;
        }
        const QString stagePath = staging.fileName();
        staging.close();
        // SQLite backup gives a consistent snapshot even if the old process
        // still owns an active WAL. Never copy or edit the source DB in place.
        QProcess backup;
        backup.start(
            "/usr/bin/python3",
            {"-c",
             "import os,sqlite3,sys; os.nice(10); from pathlib import Path; "
             "s=sqlite3.connect(Path(sys.argv[1]).as_uri()+'?mode=ro',uri=True); "
             "d=sqlite3.connect(sys.argv[2]); s.backup(d,pages=1024); d.close(); s.close()",
             oldDb, stagePath});
        QElapsedTimer deadline;
        deadline.start();
        while (!backup.waitForFinished(250) && backup.state() != QProcess::NotRunning &&
               deadline.elapsed() < 60000 && !QThread::currentThread()->isInterruptionRequested()) {
        }
        if (backup.state() != QProcess::NotRunning) {
            backup.kill();
            backup.waitForFinished();
            result.ok = false;
        } else if (backup.exitStatus() != QProcess::NormalExit || backup.exitCode() != 0)
            result.ok = false;
        else if (QFile::rename(stagePath, newDb)) {
            ++result.copied;
            QFile::setPermissions(newDb, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        } else
            result.ok = false;
        result.knowledgePending = !QFileInfo::exists(newDb);
    }
    if (!settings.contains("smartSpace/knowledgeDirectory"))
        settings.setValue("smartSpace/knowledgeDirectory", knowledge);
    // An independent login entry is opt-in; importing preferences does not
    // install another autostart task beside an older running Fences instance.
    if (!settings.contains("smartSpace/autoStart"))
        settings.setValue("smartSpace/autoStart", false);
    if (result.ok && !result.knowledgePending)
        settings.setValue("migration/legacyV1", true);
    settings.sync();
    if (settings.status() != QSettings::NoError)
        result.ok = false;
    if (!result.ok)
        result.error =
            QStringLiteral("部分旧数据未能复制；原始配置和索引保持不变，可修正目录权限后重试。");
    return result;
}
