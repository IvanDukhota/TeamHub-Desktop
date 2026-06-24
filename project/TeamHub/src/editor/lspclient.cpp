#include "lspclient.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

LspClient::LspClient(QObject *parent) : QObject(parent) {}

LspClient::~LspClient()
{
    stop();
}

QString LspClient::toUri(const QString &path)
{
    QString p = QDir::fromNativeSeparators(path);
    if (!p.startsWith('/'))
        p.prepend('/');
    return "file://" + p;
}

void LspClient::start(const QString &rootPath)
{
    stop();

    proc = new QProcess(this);
    proc->setProcessChannelMode(QProcess::SeparateChannels);
    connect(proc, &QProcess::readyReadStandardOutput, this, &LspClient::onReadyRead);

#ifdef Q_OS_WIN
    proc->start("cmd.exe", {"/c", "pyright-langserver", "--stdio"});
#else
    proc->start("pyright-langserver", {"--stdio"});
#endif
    if (!proc->waitForStarted(3000)) {
        qDebug() << "[LSP] pyright-langserver failed to start:" << proc->errorString();
        delete proc;
        proc = nullptr;
        return;
    }
    qDebug() << "[LSP] pyright-langserver started, sending initialize...";

    QJsonObject caps;
    QJsonObject tdCaps;
    QJsonObject completionCaps;
    QJsonObject completionItemCaps;
    completionItemCaps["snippetSupport"] = false;
    completionCaps["completionItem"] = completionItemCaps;
    tdCaps["completion"] = completionCaps;
    caps["textDocument"] = tdCaps;

    QJsonObject params;
    params["rootUri"] = toUri(rootPath);
    params["rootPath"] = rootPath;
    params["capabilities"] = caps;
    params["processId"] = (int) QCoreApplication::applicationPid();

    const int id = nextId++;
    QJsonObject req;
    req["jsonrpc"] = "2.0";
    req["id"] = id;
    req["method"] = "initialize";
    req["params"] = params;
    pending[id] = "initialize";
    send(req);
}

void LspClient::stop()
{
    if (!proc)
        return;
    if (proc->state() != QProcess::NotRunning) {
        QJsonObject req;
        req["jsonrpc"] = "2.0";
        req["id"] = nextId++;
        req["method"] = "shutdown";
        req["params"] = QJsonValue(QJsonValue::Null);
        send(req);
        proc->closeWriteChannel();
        proc->waitForFinished(1500);
        if (proc->state() != QProcess::NotRunning)
            proc->kill();
    }
    delete proc;
    proc = nullptr;
    ready = false;
    buf.clear();
    pending.clear();
}

bool LspClient::isRunning() const
{
    return proc && proc->state() == QProcess::Running;
}

bool LspClient::isInitialized() const
{
    return ready;
}

void LspClient::didOpen(const QString &path, const QString &text)
{
    if (!ready)
        return;
    QJsonObject td;
    td["uri"] = toUri(path);
    td["languageId"] = path.endsWith(".py", Qt::CaseInsensitive) ? "python" : "plaintext";
    td["version"] = 1;
    td["text"] = text;

    QJsonObject params;
    params["textDocument"] = td;

    QJsonObject notif;
    notif["jsonrpc"] = "2.0";
    notif["method"] = "textDocument/didOpen";
    notif["params"] = params;
    send(notif);
}

void LspClient::didChange(const QString &path, const QString &text, int version)
{
    if (!ready)
        return;
    QJsonObject tdId;
    tdId["uri"] = toUri(path);
    tdId["version"] = version;

    QJsonObject change;
    change["text"] = text;
    QJsonArray changes;
    changes.append(change);

    QJsonObject params;
    params["textDocument"] = tdId;
    params["contentChanges"] = changes;

    QJsonObject notif;
    notif["jsonrpc"] = "2.0";
    notif["method"] = "textDocument/didChange";
    notif["params"] = params;
    send(notif);
}

void LspClient::didClose(const QString &path)
{
    if (!ready)
        return;
    QJsonObject tdId;
    tdId["uri"] = toUri(path);

    QJsonObject params;
    params["textDocument"] = tdId;

    QJsonObject notif;
    notif["jsonrpc"] = "2.0";
    notif["method"] = "textDocument/didClose";
    notif["params"] = params;
    send(notif);
}

void LspClient::requestHover(const QString &path, int line, int col)
{
    if (!ready)
        return;
    QJsonObject pos;
    pos["line"] = line;
    pos["character"] = col;

    QJsonObject tdId;
    tdId["uri"] = toUri(path);

    QJsonObject params;
    params["textDocument"] = tdId;
    params["position"] = pos;

    const int id = nextId++;
    QJsonObject req;
    req["jsonrpc"] = "2.0";
    req["id"] = id;
    req["method"] = "textDocument/hover";
    req["params"] = params;
    pending[id] = "textDocument/hover";
    send(req);
}

void LspClient::requestCompletion(const QString &path, int line, int col)
{
    if (!ready)
        return;
    QJsonObject pos;
    pos["line"] = line;
    pos["character"] = col;

    QJsonObject tdId;
    tdId["uri"] = toUri(path);

    QJsonObject params;
    params["textDocument"] = tdId;
    params["position"] = pos;

    const int id = nextId++;
    QJsonObject req;
    req["jsonrpc"] = "2.0";
    req["id"] = id;
    req["method"] = "textDocument/completion";
    req["params"] = params;
    pending[id] = "textDocument/completion";
    send(req);
}

void LspClient::send(const QJsonObject &obj)
{
    if (!proc || proc->state() != QProcess::Running)
        return;
    const QByteArray body = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    const QByteArray header = "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n";
    proc->write(header);
    proc->write(body);
}

void LspClient::onReadyRead()
{
    buf += proc->readAllStandardOutput();

    while (true) {
        const int hdrEnd = buf.indexOf("\r\n\r\n");
        if (hdrEnd < 0)
            break;

        int contentLen = -1;
        for (const QByteArray &line : buf.left(hdrEnd).split('\n')) {
            const QByteArray t = line.trimmed();
            if (t.startsWith("Content-Length:")) {
                contentLen = t.mid(15).trimmed().toInt();
                break;
            }
        }
        if (contentLen < 0) {
            buf.clear();
            break;
        }

        const int bodyStart = hdrEnd + 4;
        if (buf.size() < bodyStart + contentLen)
            break;

        const QByteArray body = buf.mid(bodyStart, contentLen);
        buf.remove(0, bodyStart + contentLen);

        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
        if (err.error == QJsonParseError::NoError && doc.isObject())
            dispatch(doc.object());
    }
}

void LspClient::dispatch(const QJsonObject &msg)
{
    if (!msg.contains("id") || msg.contains("method"))
        return;

    const int id = msg["id"].toInt();
    const QString method = pending.take(id);

    if (method == "initialize") {
        QJsonObject notif;
        notif["jsonrpc"] = "2.0";
        notif["method"] = "initialized";
        notif["params"] = QJsonObject();
        send(notif);
        ready = true;
        qDebug() << "[LSP] pyright initialized, ready for completions";
        emit initialized();
        return;
    }

    if (method == "textDocument/hover") {
        const QJsonValue result = msg["result"];
        QString content;
        if (result.isObject()) {
            const QJsonValue contents = result.toObject()["contents"];
            if (contents.isString())
                content = contents.toString();
            else if (contents.isObject())
                content = contents.toObject()["value"].toString();
            else if (contents.isArray()) {
                QStringList parts;
                for (const QJsonValue &v : contents.toArray()) {
                    if (v.isString())
                        parts << v.toString();
                    else if (v.isObject())
                        parts << v.toObject()["value"].toString();
                }
                content = parts.join("\n\n");
            }
        }
        if (!content.trimmed().isEmpty())
            emit hoverReady(content);
        return;
    }

    if (method == "textDocument/completion") {
        QJsonArray arr;
        const QJsonValue result = msg["result"];
        if (result.isArray())
            arr = result.toArray();
        else if (result.isObject())
            arr = result.toObject()["items"].toArray();

        QList<LspCompletionItem> items;
        items.reserve(arr.size());
        for (const QJsonValue &v : arr) {
            const QJsonObject item = v.toObject();
            const QString label = item["label"].toString();
            if (label.isEmpty())
                continue;
            LspCompletionItem ci;
            ci.label = label;
            ci.insertText = item.contains("insertText") ? item["insertText"].toString() : label;
            ci.sortText = item.contains("sortText") ? item["sortText"].toString() : label;
            ci.kind = item["kind"].toInt(0);
            items.append(ci);
        }
        qDebug() << "[LSP] completionReady, items:" << items.size();
        emit completionReady(items);
    }
}
