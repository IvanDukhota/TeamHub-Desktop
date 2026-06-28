#include "appconfig.h"

#include <QCoreApplication>
#include <QFile>
#include <QHash>
#include <QTextStream>

namespace {

QHash<QString, QString> parseEnvFile(const QString &path)
{
    QHash<QString, QString> values;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return values;

    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const QString line = stream.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;

        const int eq = line.indexOf('=');
        if (eq <= 0)
            continue;

        const QString key = line.left(eq).trimmed();
        QString value = line.mid(eq + 1).trimmed();
        if (value.size() >= 2
            && ((value.front() == '"' && value.back() == '"')
                || (value.front() == '\'' && value.back() == '\'')))
            value = value.mid(1, value.size() - 2);

        values.insert(key, value);
    }

    return values;
}

const QHash<QString, QString> &envValues()
{
    static const QHash<QString, QString> values = [] {
        QHash<QString, QString> map = parseEnvFile(QCoreApplication::applicationDirPath() + "/.env");
        if (map.isEmpty())
            map = parseEnvFile("C:/TeamHub-Desktop/project/TeamHub/.env");
        return map;
    }();
    return values;
}

QString envOr(const QString &key, const QString &fallback)
{
    return envValues().value(key, fallback);
}

}

namespace AppConfig {

QString djangoBaseUrl()
{
    return envOr("DJANGO_BASE_URL", "http://localhost:8000");
}

QString rgaServerUrl()
{
    return envOr("RGA_SERVER_URL", "ws://localhost:8000/ws/collab");
}

QString voiceServerHost()
{
    return envOr("VOICE_SERVER_HOST", "localhost");
}

quint16 voiceServerPort()
{
    return static_cast<quint16>(envOr("VOICE_SERVER_PORT", "8000").toUShort());
}

}
