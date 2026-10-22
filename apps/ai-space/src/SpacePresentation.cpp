#include "SpacePresentation.h"
#include <QDir>
namespace AiSpaceUi
{
QString normalizedPath(QString path)
{
    path = QDir::cleanPath(path);
    while (path.size() > 1 && path.endsWith(QLatin1Char('/')))
        path.chop(1);
    return path;
}

bool pathIsInside(const QString &path, const QString &folder)
{
    const QString child = normalizedPath(path);
    const QString parent = normalizedPath(folder);
    return child == parent || child.startsWith(parent + QLatin1Char('/'));
}

QString categoryTitle(const QString &category)
{
    if (category == QLatin1String("image"))
        return QStringLiteral("图片");
    if (category == QLatin1String("pdf"))
        return QStringLiteral("PDF");
    if (category == QLatin1String("presentation"))
        return QStringLiteral("演示");
    if (category == QLatin1String("spreadsheet"))
        return QStringLiteral("表格");
    if (category == QLatin1String("document"))
        return QStringLiteral("文档");
    return QStringLiteral("其他");
}

QString formattedFileSize(qint64 size)
{
    if (size < 0)
        return QString();
    constexpr qreal kib = 1024.0;
    constexpr qreal mib = kib * 1024.0;
    constexpr qreal gib = mib * 1024.0;
    if (size >= gib)
        return QStringLiteral("%1 GB").arg(size / gib, 0, 'f', 1);
    if (size >= mib)
        return QStringLiteral("%1 MB").arg(size / mib, 0, 'f', 1);
    if (size >= kib)
        return QStringLiteral("%1 KB").arg(size / kib, 0, 'f', 0);
    return QStringLiteral("%1 B").arg(size);
}

QString formattedModified(const QDateTime &modified)
{
    if (!modified.isValid())
        return QString();
    const QDateTime local = modified.toLocalTime();
    const QDate today = QDate::currentDate();
    if (local.date() == today)
        return QStringLiteral("今天 %1").arg(local.time().toString(QStringLiteral("HH:mm")));
    if (local.date() == today.addDays(-1))
        return QStringLiteral("昨天 %1").arg(local.time().toString(QStringLiteral("HH:mm")));
    return local.toString(QStringLiteral("MM-dd HH:mm"));
}

} // namespace AiSpaceUi

#include <QComboBox>
#include <QListView>
namespace AiSpaceUi
{
QString cssColor(const QColor &color)
{
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alpha());
}

void styleModernCombos(QWidget *root)
{
    if (!root)
        return;
    const QList<QComboBox *> combos = root->findChildren<QComboBox *>();
    for (QComboBox *combo : combos) {
        if (!qobject_cast<QListView *>(combo->view())) {
            auto *view = new QListView(combo);
            view->setFrameShape(QFrame::NoFrame);
            combo->setView(view);
        }
    }
}

} // namespace AiSpaceUi
