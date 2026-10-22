#pragma once
#include <QSet>
#include <QStringList>
#include <QVector>
namespace AiSpaceUi {
struct FormatOption {
    const char *id;
    const char *label;
};

const QVector<FormatOption> &formatOptions();
const QSet<QString> &knownFormatIds();
bool formatAllowed(const QString &suffix,const QStringList &selected);
}
