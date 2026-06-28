#include "collabsession.h"
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

CollabSession::CollabSession(int siteId, Role role, QObject *parent)
    : QObject(parent)
    , socket(new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this))
    , reconnectTimer(new QTimer(this))
    , currentSiteId(siteId)
    , currentRole(role)
{
    connect(socket, &QWebSocket::connected, this, &CollabSession::onConnected);
    connect(socket, &QWebSocket::disconnected, this, &CollabSession::onDisconnected);
    connect(socket, &QWebSocket::textMessageReceived, this, &CollabSession::onRawMessage);
    connect(socket,
            QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
            this,
            &CollabSession::onError);

    reconnectTimer->setSingleShot(true);
    connect(reconnectTimer, &QTimer::timeout, this, [this]() { socket->open(QUrl(serverUrl)); });
}

CollabSession::~CollabSession()
{
    disconnectFromServer();
    for (auto *mgr : active)
        mgr->deleteLater();
    active.clear();
}

void CollabSession::connectToServer(const QString &url)
{
    serverUrl = url;
    wantReconnect = true;
    reconnectAttempt = 0;
    socket->open(QUrl(url));
}

void CollabSession::disconnectFromServer()
{
    wantReconnect = false;
    reconnectTimer->stop();
    pendingQueue.clear();
    socket->close();
}

bool CollabSession::isConnected() const
{
    return socket->state() == QAbstractSocket::ConnectedState;
}

void CollabSession::setProject(const QString &projectRoot, const QStringList &relFiles)
{
    rootPath = projectRoot;
    files = relFiles;
}

void CollabSession::initFileCache(const QString &relPath, const QString &text)
{
    textCache[relPath] = text;
}

void CollabSession::sendAllSnapshots(const QMap<QString, QString> &fileTexts)
{
    for (auto it = fileTexts.cbegin(); it != fileTexts.cend(); ++it) {
        const QString &relPath = it.key();
        const QString &text = it.value();

        textCache[relPath] = text;

        QJsonObject msg;
        msg["type"] = "snapshot";
        msg["file"] = relPath;
        msg["text"] = text;
        sendMessage(msg);
    }
}

void CollabSession::broadcastRunOutput(const QString &text)
{
    QJsonObject msg;
    msg["type"] = "run_output";
    msg["text"] = text;
    sendMessage(msg);
}

void CollabSession::notifyFileCreated(const QString &relPath, const QString &text)
{
    if (!files.contains(relPath))
        files.append(relPath);
    textCache[relPath] = text;

    QJsonObject msg;
    msg["type"] = "file_create";
    msg["file"] = relPath;
    msg["text"] = text;
    sendMessage(msg);
}

void CollabSession::notifyFileDeleted(const QString &relPath)
{
    files.removeAll(relPath);
    textCache.remove(relPath);
    fileStates.remove(relPath);
    if (auto *mgr = active.take(relPath)) {
        lastUsed.remove(relPath);
        mgr->deleteLater();
    }

    QJsonObject msg;
    msg["type"] = "file_delete";
    msg["file"] = relPath;
    sendMessage(msg);
}

void CollabSession::sendCursorLeave(const QString &relPath)
{
    QJsonObject msg;
    msg["type"] = "cursor_leave";
    msg["file"] = relPath;
    msg["siteId"] = currentSiteId;
    sendMessage(msg);
}

void CollabSession::sendFileFocus(const QString &relPath)
{
    QJsonObject msg;
    msg["type"] = "file_focus";
    msg["file"] = relPath;
    msg["siteId"] = currentSiteId;
    sendMessage(msg);
}

void CollabSession::notifyFileRenamed(const QString &oldPath, const QString &newPath)
{
    const int idx = files.indexOf(oldPath);
    if (idx >= 0)
        files[idx] = newPath;

    if (textCache.contains(oldPath))
        textCache[newPath] = textCache.take(oldPath);
    if (fileStates.contains(oldPath))
        fileStates[newPath] = fileStates.take(oldPath);
    if (auto *mgr = active.take(oldPath)) {
        mgr->setFilePath(newPath);
        active[newPath] = mgr;
        lastUsed[newPath] = lastUsed.take(oldPath);
    }

    QJsonObject msg;
    msg["type"] = "file_rename";
    msg["old"] = oldPath;
    msg["new"] = newPath;
    sendMessage(msg);
}

RGAManager *CollabSession::getOrCreateRGA(const QString &relPath)
{
    if (auto *mgr = active.value(relPath, nullptr)) {
        lastUsed[relPath] = QDateTime::currentDateTime();
        return mgr;
    }

    if (active.size() >= MAX_ACTIVE)
        evictLRU();

    auto *mgr = new RGAManager(currentSiteId, this);
    mgr->setFilePath(relPath);
    mgr->setSendFunction([this](QJsonObject msg) { sendMessage(msg); });

    const FileState &state = fileStates.value(relPath);
    if (!state.sequence.rgaseq.isEmpty()) {
        mgr->setSequence(state.sequence);
        mgr->setTimestamp(state.maxTimestamp);
    } else if (textCache.contains(relPath)) {
        mgr->buildFromText(textCache[relPath]);
        fileStates[relPath].sequence = mgr->getSequence();
        fileStates[relPath].maxTimestamp = mgr->getTimestamp();
    }

    active[relPath] = mgr;
    lastUsed[relPath] = QDateTime::currentDateTime();
    return mgr;
}

void CollabSession::releaseRGA(const QString &relPath)
{
    auto *mgr = active.take(relPath);
    if (!mgr)
        return;

    auto &state = fileStates[relPath];
    state.sequence = mgr->getSequence();
    state.maxTimestamp = mgr->getTimestamp();
    textCache[relPath] = mgr->getText();
    lastUsed.remove(relPath);
    mgr->deleteLater();
}

void CollabSession::evictLRU()
{
    if (active.isEmpty())
        return;

    QString oldest;
    QDateTime oldestTime = QDateTime::currentDateTime().addSecs(1);

    for (auto it = lastUsed.cbegin(); it != lastUsed.cend(); ++it) {
        if (active.contains(it.key()) && it.value() < oldestTime) {
            oldestTime = it.value();
            oldest = it.key();
        }
    }
    if (oldest.isEmpty())
        oldest = active.firstKey();

    auto *mgr = active.take(oldest);
    auto &state = fileStates[oldest];
    state.sequence = mgr->getSequence();
    state.maxTimestamp = mgr->getTimestamp();
    textCache[oldest] = mgr->getText();
    lastUsed.remove(oldest);
    mgr->deleteLater();
}

void CollabSession::applyOpToInactive(const QString &file, const QJsonObject &op)
{
    auto &state = fileStates[file];

    if (state.sequence.rgaseq.isEmpty() && textCache.contains(file)) {
        const QString text = textCache[file];
        int ts = 0;
        RGAId parent{};
        state.sequence.rgaseq.reserve(text.size());
        for (QChar c : text) {
            ++ts;
            RGANode node;
            node.id = {ts, 0};
            node.parent = parent;
            node.val = c;
            node.tombstone = false;
            state.sequence.rgaseq.append(node);
            parent = node.id;
        }
        state.maxTimestamp = ts;
        state.sequence.rebuildIndex();
    }

    const QString type = op["type"].toString();

    if (type == "insert") {
        RGANode node = RGAManager::nodeFromJson(op["node"].toObject());
        state.maxTimestamp = qMax(state.maxTimestamp, node.id.timestamp) + 1;
        state.sequence.insert(node);
        textCache[file] = state.sequence.toText();
    } else if (type == "delete") {
        RGAId id = RGAManager::idFromJson(op["id"].toObject());
        state.sequence.remove(id);
        textCache[file] = state.sequence.toText();
    } else if (type == "undelete") {
        RGAId id = RGAManager::idFromJson(op["id"].toObject());
        state.sequence.undelete(id);
        textCache[file] = state.sequence.toText();
    }
}

void CollabSession::onConnected()
{
    reconnectAttempt = 0;
    reconnectTimer->stop();
    sendRegister();
    while (!pendingQueue.isEmpty())
        socket->sendTextMessage(QJsonDocument(pendingQueue.dequeue()).toJson(QJsonDocument::Compact));
    emit connected();
}

void CollabSession::onDisconnected()
{
    if (sessionEndedByServer) {
        sessionEndedByServer = false;
        emit disconnected();
        return;
    }
    if (!wantReconnect)
        return;
    emit disconnected();
    scheduleReconnect();
}

void CollabSession::onRawMessage(const QString &message)
{
    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
    if (doc.isObject())
        handleMessage(doc.object());
}

void CollabSession::onError(QAbstractSocket::SocketError)
{
    emit errorOccurred(socket->errorString());
    scheduleReconnect();
}

void CollabSession::scheduleReconnect()
{
    if (!wantReconnect || reconnectTimer->isActive())
        return;
    if (reconnectAttempt >= MAX_RECONNECT_ATTEMPTS) {
        wantReconnect = false;
        pendingQueue.clear();
        emit errorOccurred("Connection lost. Failed to reconnect after "
                           + QString::number(MAX_RECONNECT_ATTEMPTS) + " attempts.");
        return;
    }

    ++reconnectAttempt;
    const int delayMs = (1 << (reconnectAttempt - 1)) * 1000;
    emit reconnecting(reconnectAttempt, MAX_RECONNECT_ATTEMPTS);
    reconnectTimer->start(delayMs);
}

void CollabSession::handleMessage(const QJsonObject &obj)
{
    const QString type = obj["type"].toString();
    const QString file = obj["file"].toString();

    if (type == "error") {
        wantReconnect = false;
        reconnectTimer->stop();
        emit errorOccurred(obj["message"].toString());
        socket->close();
        return;
    }

    if (type == "session_ended") {
        wantReconnect = false;
        reconnectTimer->stop();
        sessionEndedByServer = true;
        socket->close();
        return;
    }

    if (type == "kicked") {
        emit kicked();
        return;
    }

    if (type == "role_change") {
        const int targetSite = obj["siteId"].toInt();
        const QString newRole = obj["role"].toString();
        if (targetSite == currentSiteId)
            mode = (newRole == "read") ? Mode::ReadOnly : Mode::ReadWrite;
        emit peerRoleChanged(targetSite, newRole);
        return;
    }

    if (type == "session_report") {
        emit sessionReportReady(parseSessionReport(obj["data"].toObject()));
        return;
    }

    if (type == "session_report_ai") {
        emit sessionAiInsightsReady(parseAiInsights(obj["data"].toObject()));
        return;
    }

    if (type == "project_init") {
        QStringList receivedFiles;
        for (const QJsonValue &v : obj["files"].toArray())
            receivedFiles.append(v.toString());
        files = receivedFiles;
        if (obj["mode"].toString() == "readonly")
            mode = Mode::ReadOnly;
        if (obj.contains("session_start"))
            startEpoch = static_cast<qint64>(obj["session_start"].toDouble());
        emit projectInitReceived(obj["host"].toInt(), receivedFiles);
        return;
    }

    if (type == "run_output") {
        emit runOutputReceived(obj["text"].toString());
        return;
    }

    if (type == "user_list") {
        QMap<int, QString> users;
        QMap<int, QString> avatars;
        QMap<int, QString> roles;
        for (const QJsonValue &v : obj["users"].toArray()) {
            const QJsonObject u = v.toObject();
            const int sid = u["siteId"].toInt();
            users[sid] = u["username"].toString();
            avatars[sid] = u["avatarUrl"].toString();
            if (u.contains("role"))
                roles[sid] = u["role"].toString();
        }
        peerAvatars = avatars;
        if (!roles.isEmpty())
            emit rolesUpdated(roles);
        emit usersUpdated(users);
        return;
    }

    if (type == "cursor_leave") {
        const int sid = obj["siteId"].toInt();
        if (!file.isEmpty()) {
            cursorCache[file].remove(sid);
            if (active.contains(file))
                active[file]->handleIncomingMessage(obj);
        } else {
            for (auto &fileCursors : cursorCache)
                fileCursors.remove(sid);
            for (auto *mgr : active.values())
                mgr->handleIncomingMessage(obj);
        }
        return;
    }

    if (type == "file_focus") {
        const int sid = obj["siteId"].toInt();
        if (sid != currentSiteId)
            emit remoteFileFocusChanged(sid, file);
        return;
    }

    if (type == "file_create") {
        const QString fp = obj["file"].toString();
        if (!files.contains(fp))
            files.append(fp);
        textCache[fp] = obj["text"].toString();
        emit remoteFileCreated(fp);
        return;
    }

    if (type == "file_delete") {
        const QString fp = obj["file"].toString();
        files.removeAll(fp);
        textCache.remove(fp);
        fileStates.remove(fp);
        if (auto *mgr = active.take(fp)) {
            lastUsed.remove(fp);
            mgr->deleteLater();
        }
        emit remoteFileDeleted(fp);
        return;
    }

    if (type == "file_rename") {
        const QString old = obj["old"].toString();
        const QString newer = obj["new"].toString();
        const int idx = files.indexOf(old);
        if (idx >= 0)
            files[idx] = newer;
        if (textCache.contains(old))
            textCache[newer] = textCache.take(old);
        if (fileStates.contains(old))
            fileStates[newer] = fileStates.take(old);
        if (auto *mgr = active.take(old)) {
            mgr->setFilePath(newer);
            active[newer] = mgr;
            lastUsed[newer] = lastUsed.take(old);
        }
        emit remoteFileRenamed(old, newer);
        return;
    }

    if (type == "snapshot") {
        if (!file.isEmpty()) {
            textCache[file] = obj["text"].toString();
            fileStates.remove(file);
            if (!files.contains(file))
                files.append(file);
        }
        if (!file.isEmpty() && active.contains(file))
            active[file]->handleIncomingMessage(obj);
        return;
    }

    if (type == "cursor") {
        if (!file.isEmpty()) {
            const int sid = obj["siteId"].toInt();
            const int pos = obj["position"].toInt();
            if (sid != currentSiteId)
                cursorCache[file][sid] = pos;
            if (active.contains(file))
                active[file]->handleIncomingMessage(obj);
        }
        return;
    }

    if (type == "insert" || type == "delete" || type == "undelete") {
        if (file.isEmpty())
            return;
        int actorSiteId = 0;
        if (type == "insert")
            actorSiteId = obj["node"].toObject()["id"].toObject()["siteId"].toInt();
        else
            actorSiteId = obj["siteId"].toInt();
        if (actorSiteId != 0 && actorSiteId != currentSiteId)
            emit remoteOpReceived(actorSiteId, type);
        if (active.contains(file))
            active[file]->handleIncomingMessage(obj);
        else
            applyOpToInactive(file, obj);
        lastUsed[file] = QDateTime::currentDateTime();
        return;
    }
}

void CollabSession::sendMessage(const QJsonObject &msg)
{
    if (mode == Mode::ReadOnly) {
        const QString t = msg["type"].toString();
        if (t == "insert" || t == "delete")
            return;
    }
    if (!isConnected()) {
        if (wantReconnect) {
            const QString t = msg["type"].toString();
            if (t == "insert" || t == "delete" || t == "undelete"
                || t == "snapshot" || t == "file_create"
                || t == "file_delete" || t == "file_rename"
                || t == "final_state") {
                pendingQueue.enqueue(msg);
            }
        }
        return;
    }
    socket->sendTextMessage(QJsonDocument(msg).toJson(QJsonDocument::Compact));
}

void CollabSession::kickUser(int siteId)
{
    QJsonObject msg;
    msg["type"] = "kick";
    msg["siteId"] = siteId;
    sendMessage(msg);
}

void CollabSession::sendRoleChange(int targetSiteId, const QString &role)
{
    QJsonObject msg;
    msg["type"] = "role_change";
    msg["siteId"] = targetSiteId;
    msg["role"] = role;
    sendMessage(msg);
}

void CollabSession::sendFinalStates(const QMap<QString, QString> &texts)
{
    QJsonObject files;
    for (auto it = texts.cbegin(); it != texts.cend(); ++it)
        files[it.key()] = it.value();

    QJsonObject msg;
    msg["type"] = "final_state";
    msg["files"] = files;
    sendMessage(msg);
}

void CollabSession::requestSessionReport()
{
    QJsonObject msg;
    msg["type"] = "session_report_request";
    sendMessage(msg);
}

void CollabSession::endSession()
{
    QJsonObject msg;
    msg["type"] = "end_session";
    sendMessage(msg);
}

SessionReportData CollabSession::parseSessionReport(const QJsonObject &data)
{
    SessionReportData r;
    r.roomName = data["room"].toString();
    r.startTime = data["start_time"].toString();
    r.endTime = data["end_time"].toString();
    r.durationSec = data["duration_sec"].toInt();

    for (const QJsonValue &pv : data["participants"].toArray()) {
        const QJsonObject po = pv.toObject();
        ParticipantStats p;
        p.siteId = po["site_id"].toInt();
        p.username = po["username"].toString();
        p.isHost = po["is_host"].toBool();
        p.activeSec = po["active_sec"].toInt();
        p.totalInserts = po["total_inserts"].toInt();
        p.totalDeletes = po["total_deletes"].toInt();
        for (const QJsonValue &fv : po["files_touched"].toArray())
            p.filesTouched.append(fv.toString());
        r.participants.append(p);
    }

    QMap<int, QString> siteToName;
    for (const ParticipantStats &p : r.participants)
        siteToName[p.siteId] = p.username;

    for (const QJsonValue &fv : data["files"].toArray()) {
        const QJsonObject fo = fv.toObject();
        FileInfo fi;
        fi.name = fo["name"].toString();
        for (const QJsonValue &ev : fo["editors"].toArray()) {
            const int sid = ev.toInt();
            fi.editorSiteIds.append(sid);
            fi.editorNames.append(siteToName.value(sid, QString("user_%1").arg(sid)));
        }
        r.files.append(fi);
    }

    return r;
}

AiInsights CollabSession::parseAiInsights(const QJsonObject &data)
{
    AiInsights ai;
    ai.available = true;
    ai.text = data["text"].toString();
    return ai;
}

void CollabSession::sendRegister()
{
    QJsonObject msg;
    msg["type"] = "register";
    msg["siteId"] = currentSiteId;
    msg["username"] = username.isEmpty() ? QString("user_%1").arg(currentSiteId) : username;
    msg["avatarUrl"] = avatarUrl;
    msg["role"] = (currentRole == Role::Host) ? "host" : "guest";

    if (currentRole == Role::Host) {
        msg["mode"] = (mode == Mode::ReadOnly) ? "readonly" : "readwrite";
        if (!files.isEmpty()) {
            QJsonArray arr;
            for (const QString &f : files)
                arr.append(f);
            msg["files"] = arr;
        }
    }
    sendMessage(msg);
}
