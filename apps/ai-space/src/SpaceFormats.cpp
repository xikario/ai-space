#include "SpaceFormats.h"
namespace AiSpaceUi
{
const QVector<FormatOption> &formatOptions()
{
    static const QVector<FormatOption> options{{"pdf", "PDF"},
                                               {"docx", "DOCX"},
                                               {"doc", "DOC"},
                                               {"wps", "WPS"},
                                               {"odt", "ODT"},
                                               {"pptx", "PPTX"},
                                               {"ppt", "PPT"},
                                               {"dps", "DPS"},
                                               {"odp", "ODP"},
                                               {"xlsx", "XLSX"},
                                               {"xls", "XLS"},
                                               {"et", "ET"},
                                               {"ods", "ODS"},
                                               {"txt", "TXT"},
                                               {"md", "MD"},
                                               {"markdown", "MARKDOWN"},
                                               {"csv", "CSV"},
                                               {"tsv", "TSV"},
                                               {"rtf", "RTF"},
                                               {"json", "JSON"},
                                               {"xml", "XML"},
                                               {"yaml", "YAML"},
                                               {"yml", "YML"},
                                               {"html", "HTML"},
                                               {"htm", "HTM"},
                                               {"log", "LOG"},
                                               {"ini", "INI"},
                                               {"conf", "CONF"},
                                               {"jpg", "JPG"},
                                               {"jpeg", "JPEG"},
                                               {"png", "PNG"},
                                               {"webp", "WEBP"},
                                               {"tif", "TIF"},
                                               {"tiff", "TIFF"},
                                               {"bmp", "BMP"},
                                               {"gif", "GIF"},
                                               {"css", "CSS"},
                                               {"js", "JS"},
                                               {"ts", "TS"},
                                               {"cpp", "CPP"},
                                               {"c", "C"},
                                               {"h", "H"},
                                               {"hpp", "HPP"},
                                               {"py", "PY"},
                                               {"sh", "SH"},
                                               {"desktop", "DESKTOP"},
                                               {"__other__", "其他或无扩展名"}};
    return options;
}

const QSet<QString> &knownFormatIds()
{
    static const QSet<QString> ids = [] {
        QSet<QString> result;
        for (const FormatOption &option : formatOptions()) {
            const QString id = QString::fromLatin1(option.id);
            if (id != QLatin1String("__other__"))
                result.insert(id);
        }
        return result;
    }();
    return ids;
}

bool formatAllowed(const QString &suffix, const QStringList &selected)
{
    if (selected.isEmpty())
        return true;
    const QString extension = suffix.toLower().remove(QLatin1Char('.'));
    if (selected.contains(extension))
        return true;
    return selected.contains(QStringLiteral("__other__")) && !knownFormatIds().contains(extension);
}

} // namespace AiSpaceUi
