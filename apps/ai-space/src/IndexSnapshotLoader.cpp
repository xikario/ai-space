#include "SmartSpaceWidget.h"
#include "SpacePresentation.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTextBrowser>
#include <QTimer>
#include <QToolTip>
#include <QtEndian>
#include <algorithm>
#include <cstring>
#include <zlib.h>
using namespace AiSpaceUi;

SmartSpaceWidget::IndexSnapshot SmartSpaceWidget::readIndexSnapshot(
    const QString &canonicalPath, const QString &streamFilename, const QString &ocrPath,
    const QString &resumePath, const QStringList &excludedFolders, int maxItems)
{
    IndexSnapshot snapshot;
    auto pathExcluded = [&excludedFolders](const QString &path) {
        for (const QString &folder : excludedFolders)
            if (pathIsInside(path, folder)) return true;
        return false;
    };
    QJsonObject root;
    QVector<SmartSpaceEntry> loaded;
    auto appendEntry = [&loaded, &pathExcluded](const QJsonObject &object) {
        SmartSpaceEntry entry;
        entry.path = normalizedPath(object.value(QStringLiteral("path")).toString());
        entry.root = normalizedPath(object.value(QStringLiteral("root")).toString());
        entry.parentPath = normalizedPath(object.value(QStringLiteral("parentPath")).toString());
        if (entry.parentPath.isEmpty())
            entry.parentPath = normalizedPath(QFileInfo(entry.path).absolutePath());
        entry.name = object.value(QStringLiteral("name")).toString();
        if (entry.name.isEmpty())
            entry.name = QFileInfo(entry.path).fileName();
        entry.suffix = object.value(QStringLiteral("suffix")).toString();
        if (entry.suffix.isEmpty() && !object.value(QStringLiteral("isDir")).toBool())
            entry.suffix = QFileInfo(entry.path).suffix().toLower();
        entry.category = object.value(QStringLiteral("category")).toString();
        entry.content = object.value(QStringLiteral("content")).toString();
        entry.extractor = object.value(QStringLiteral("extractor")).toString();
        entry.ocrStatus = object.value(QStringLiteral("ocrStatus")).toString();
        entry.ocrReason = object.value(QStringLiteral("ocrReason")).toString();
        entry.isDir = object.value(QStringLiteral("isDir")).toBool();
        entry.size = static_cast<qint64>(object.value(QStringLiteral("size")).toDouble());
        entry.modified = QDateTime::fromString(
            object.value(QStringLiteral("modified")).toString(), Qt::ISODateWithMs);
        if (!entry.path.isEmpty() && !pathExcluded(entry.path))
            loaded.append(entry);
    };

    bool loadedFromStream = false;
    QString streamFailure;
    const QByteArray streamPath = QFile::encodeName(streamFilename);
    const QFileInfo streamInfo(streamFilename);
    const QFileInfo canonicalInfo(canonicalPath);
    const bool streamIsCurrent = streamInfo.exists() &&
        (!canonicalInfo.exists() ||
         streamInfo.lastModified() >= canonicalInfo.lastModified());
    if (streamInfo.exists() && !streamIsCurrent)
        streamFailure = QStringLiteral("流式索引旧于主索引");
    if (streamIsCurrent) {
        gzFile archive = gzopen(streamPath.constData(), "rb");
        if (!archive) {
            streamFailure = QStringLiteral("无法打开流式索引");
        } else {
            QByteArray decompressedBuffer(256 * 1024, '\0');
            int bufferOffset = 0;
            int bufferSize = 0;
            auto readExact = [archive, &decompressedBuffer, &bufferOffset,
                              &bufferSize](void *destination, int bytes) {
                char *cursor = static_cast<char *>(destination);
                int remaining = bytes;
                while (remaining > 0) {
                    if (bufferOffset >= bufferSize) {
                        bufferSize = gzread(archive, decompressedBuffer.data(),
                                            decompressedBuffer.size());
                        bufferOffset = 0;
                        if (bufferSize <= 0)
                            return false;
                    }
                    const int count = qMin(remaining,
                                           bufferSize - bufferOffset);
                    std::memcpy(cursor,
                                decompressedBuffer.constData() + bufferOffset,
                                static_cast<size_t>(count));
                    cursor += count;
                    bufferOffset += count;
                    remaining -= count;
                }
                return true;
            };
            auto readU32 = [&readExact](quint32 &value) {
                quint32 encoded = 0;
                if (!readExact(&encoded, sizeof(encoded)))
                    return false;
                value = qFromLittleEndian(encoded);
                return true;
            };
            auto readI64 = [&readExact](qint64 &value) {
                qint64 encoded = 0;
                if (!readExact(&encoded, sizeof(encoded)))
                    return false;
                value = qFromLittleEndian(encoded);
                return true;
            };
            auto readString = [&readExact, &readU32](QString &value) {
                quint32 length = 0;
                if (!readU32(length) || length > 64U * 1024U * 1024U)
                    return false;
                QByteArray encoded;
                encoded.resize(static_cast<int>(length));
                if (length > 0 && !readExact(encoded.data(), encoded.size()))
                    return false;
                value = QString::fromUtf8(encoded);
                return true;
            };

            char magic[8] = {};
            bool streamOk = readExact(magic, sizeof(magic)) &&
                std::memcmp(magic, "UKFIDX1\n", sizeof(magic)) == 0;
            quint32 streamVersion = 0;
            quint32 metadataLength = 0;
            streamOk = streamOk && readU32(streamVersion) &&
                streamVersion == 1 && readU32(metadataLength) &&
                metadataLength <= 4U * 1024U * 1024U;
            QByteArray metadata;
            if (streamOk) {
                metadata.resize(static_cast<int>(metadataLength));
                streamOk = metadataLength == 0 ||
                    readExact(metadata.data(), metadata.size());
            }
            int expectedItems = -1;
            int parsedItems = 0;
            if (streamOk) {
                QJsonParseError metaError;
                const QJsonDocument metaDocument = QJsonDocument::fromJson(
                    metadata, &metaError);
                root = metaDocument.object();
                quint32 encodedItems = 0;
                streamOk = readU32(encodedItems);
                expectedItems = static_cast<int>(encodedItems);
                streamOk = metaError.error == QJsonParseError::NoError &&
                    metaDocument.isObject() && !root.isEmpty() && streamOk &&
                    expectedItems >= 0 && expectedItems <= 1000000 &&
                    root.value(QStringLiteral("uiStreamItems")).toInt(-1) ==
                        expectedItems;
                if (!streamOk)
                    streamFailure = QStringLiteral("流式索引元数据无效");
                if (streamOk && canonicalInfo.exists()) {
                    QFile canonical(canonicalPath);
                    if (canonical.open(QIODevice::ReadOnly)) {
                        // Current writers put the generation first, so no huge
                        // QJsonDocument is needed to compare the pair.
                        const auto prefix = QString::fromUtf8(canonical.read(512));
                        const QRegularExpression expression(QStringLiteral("^\\s*\\{\\s*\"generation\"\\s*:\"([a-f0-9]{32})\""));
                        const auto match = expression.match(prefix);
                        const auto generation = root.value(QStringLiteral("generation")).toString();
                        if ((!generation.isEmpty() || match.hasMatch()) &&
                            (!match.hasMatch() || match.captured(1) != generation)) {
                            streamOk = false;
                            streamFailure = QStringLiteral("索引两份文件版本不一致，请重新扫描");
                        }
                    } else {
                        streamOk = false;
                        streamFailure = QStringLiteral("无法核对主索引版本");
                    }
                }

            }
            if (streamOk)
                loaded.reserve(expectedItems);
            while (streamOk && parsedItems < expectedItems) {
                quint8 flags = 0;
                qint64 size = 0;
                qint64 modifiedMs = -1;
                SmartSpaceEntry entry;
                streamOk = readExact(&flags, sizeof(flags)) &&
                    readI64(size) && readI64(modifiedMs) &&
                    readString(entry.path) && readString(entry.root) &&
                    readString(entry.name) && readString(entry.category) &&
                    readString(entry.content) && readString(entry.extractor) &&
                    readString(entry.ocrStatus) && readString(entry.ocrReason);
                if (!streamOk) {
                    streamOk = false;
                    streamFailure = QStringLiteral("流式索引条目无效");
                    break;
                }
                ++parsedItems;
                entry.path = normalizedPath(entry.path);
                entry.root = normalizedPath(entry.root);
                entry.parentPath = normalizedPath(
                    QFileInfo(entry.path).absolutePath());
                if (entry.name.isEmpty())
                    entry.name = QFileInfo(entry.path).fileName();
                entry.isDir = (flags & 1U) != 0;
                if (!entry.isDir)
                    entry.suffix = QFileInfo(entry.path).suffix().toLower();
                entry.size = size;
                if (modifiedMs >= 0)
                    entry.modified = QDateTime::fromMSecsSinceEpoch(
                        modifiedMs, Qt::UTC);
                if (!entry.path.isEmpty() && !pathExcluded(entry.path))
                    loaded.append(entry);
            }
            int zlibError = Z_OK;
            gzerror(archive, &zlibError);
            if (zlibError != Z_OK && zlibError != Z_STREAM_END) {
                streamOk = false;
                streamFailure = QStringLiteral("流式索引压缩数据损坏");
            }
            if (parsedItems != expectedItems) {
                streamOk = false;
                streamFailure = QStringLiteral("流式索引条目数不完整");
            }
            if (gzclose(archive) != Z_OK) {
                streamOk = false;
                streamFailure = QStringLiteral("流式索引解压失败");
            }
            loadedFromStream = streamOk;
            if (!streamOk) {
                root = QJsonObject();
                loaded.clear();
            }
        }
    }

    if (!loadedFromStream) {
        QFile file(canonicalPath);
        if (!file.open(QIODevice::ReadOnly)) {
            snapshot.error = QStringLiteral("无法读取索引文件，请检查文件权限");
            return snapshot;
        }
        if(file.size()>64LL*1024*1024) {
            snapshot.error=QStringLiteral("大清单的流式索引不可用，请重新执行扫描生成；现有正文知识库不受影响");
            return snapshot;
        }
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            const QString detail = streamFailure.isEmpty()
                ? error.errorString()
                : QStringLiteral("%1；%2").arg(error.errorString(), streamFailure);
            snapshot.error = QStringLiteral("索引加载失败（%1）· 索引文件保持不变").arg(detail);
            return snapshot;
        }
        root = document.object();
        const QJsonArray items = root.value(QStringLiteral("items")).toArray();
        loaded.reserve(items.size());
        for (const QJsonValue &value : items)
            appendEntry(value.toObject());
    }

    const QString generated = root.value(QStringLiteral("generatedAt")).toString();
    const int errorCount = root.value(QStringLiteral("errors")).toArray().size();
    const QJsonObject stats = root.value(QStringLiteral("stats")).toObject();
    const QJsonObject capabilities = root.value(
        QStringLiteral("capabilities")).toObject();
    const int reused = stats.value(QStringLiteral("reused")).toInt();
    int pendingOcr = stats.value(QStringLiteral("ocrPending")).toInt(-1);
    // The published fast-full snapshot keeps the original OCR candidate count.
    // While an OCR backfill is paused, the append-only resume journal contains
    // the newer per-file states, so use its latest state for the status bar.
    QFile ocrResume(ocrPath);
    if (ocrResume.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QHash<QString, bool> pendingByPath;
        bool resumeMatchesSnapshot = false;
        while (!ocrResume.atEnd()) {
            const QByteArray line = ocrResume.readLine().trimmed();
            if (line.isEmpty())
                continue;
            const QJsonDocument checkpoint = QJsonDocument::fromJson(line);
            if (!checkpoint.isObject())
                continue;
            const QJsonObject record = checkpoint.object();
            if (record.value(QStringLiteral("kind")).toString() ==
                QLatin1String("header")) {
                const QJsonObject signature = record.value(
                    QStringLiteral("signature")).toObject();
                resumeMatchesSnapshot = signature.value(
                    QStringLiteral("snapshotGeneratedAt")).toString() ==
                    root.value(QStringLiteral("generatedAt")).toString();
                if (!resumeMatchesSnapshot)
                    break;
                continue;
            }
            if (!resumeMatchesSnapshot)
                continue;
            const QJsonObject item = record.value(QStringLiteral("item")).toObject();
            const QString path = item.value(QStringLiteral("path")).toString();
            if (path.isEmpty())
                continue;
            pendingByPath.insert(
                path, item.value(QStringLiteral("ocrStatus")).toString() ==
                          QLatin1String("pending"));
        }
        if (resumeMatchesSnapshot && !pendingByPath.isEmpty())
            pendingOcr = std::count(pendingByPath.cbegin(),
                                    pendingByPath.cend(), true);
    }
    if (pendingOcr < 0) {
        pendingOcr = 0;
        for (const SmartSpaceEntry &entry : loaded) {
            if (entry.ocrStatus == QLatin1String("pending"))
                ++pendingOcr;
        }
    }
    const bool truncated = stats.value(QStringLiteral("truncated")).toInt() != 0;
    const bool fullRebuild = root.value(QStringLiteral("fullRebuild")).toBool();
    const QString storedIndexMode = root.value(
        QStringLiteral("indexMode")).toString();
    const int snapshotLimit = capabilities.value(
        QStringLiteral("maxItems")).toInt();
    const bool fullResumePending = !fullRebuild && QFileInfo::exists(
        resumePath);
    const QDateTime generatedTime = QDateTime::fromString(generated, Qt::ISODateWithMs);
    const QString displayTime = generatedTime.isValid()
        ? generatedTime.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
        : generated.left(19).replace(QLatin1Char('T'), QLatin1Char(' '));
    const QString snapshotKind = fullRebuild
        ? (storedIndexMode == QLatin1String("fast-full")
            ? QStringLiteral("快速全量已同步")
            : QStringLiteral("全量索引已同步"))
        : (fullResumePending
            ? QStringLiteral("全量待续扫 · 当前展示上次日常索引")
            : QStringLiteral("日常索引已同步"));
    const QString limitState = fullRebuild
        ? QStringLiteral(" · 全量无文件数上限")
        : (truncated
            ? QStringLiteral(" · 已达日常上限 %1").arg(
                  snapshotLimit > 0 ? snapshotLimit : maxItems)
            : QString());
    const QString ocrState = pendingOcr > 0
        ? QStringLiteral(" · 待检测/OCR %1 项").arg(pendingOcr)
        : QString();
    snapshot.statusPrefix = snapshotKind + QStringLiteral(" · ");
    snapshot.statusSuffix = QStringLiteral(" 项%1%2%3%4 · %5")
        .arg(errorCount ? QStringLiteral(" · %1 项跳过").arg(errorCount) : QString())
        .arg(reused ? QStringLiteral(" · 复用 %1 项").arg(reused) : QString())
        .arg(limitState).arg(ocrState).arg(displayTime);
    snapshot.entries = std::move(loaded);
    snapshot.valid = true;
    return snapshot;
}
