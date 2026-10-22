#pragma once
#include <QDateTime>
#include <QString>
#include <QColor>
class QWidget;
namespace AiSpaceUi {
QString cssColor(const QColor &color);
void styleModernCombos(QWidget *root);
QString normalizedPath(QString path);
bool pathIsInside(const QString &path,const QString &folder);
QString categoryTitle(const QString &category);
QString formattedFileSize(qint64 size);
QString formattedModified(const QDateTime &modified);
}
