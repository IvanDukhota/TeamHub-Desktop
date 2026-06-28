#include "rgamanager.h"
#include <QFileInfo>
#include <QJsonArray>

RGAManager::RGAManager(int siteId, QObject *parent)
    : QObject(parent)
    , siteId(siteId)
    , timestamp(0)
{}

void RGAManager::setFilePath(const QString &filePath)
{
    this->filePath = filePath;
}

void RGAManager::setSendFunction(std::function<void(QJsonObject)> fn)
{
    sendFn = std::move(fn);
}

void RGAManager::handleIncomingMessage(const QJsonObject &obj)
{
    processMessage(obj);
}

QString RGAManager::getText()
{
    return sequence.toText();
}

static int byteOffsetToCharIndex(const QByteArray &utf8, int byteOffset)
{
    int charIndex = 0;
    for (int b = 0; b < byteOffset && b < utf8.size();) {
        const unsigned char c = static_cast<unsigned char>(utf8.at(b));
        if (c < 0x80)
            b += 1;
        else if (c < 0xE0)
            b += 2;
        else if (c < 0xF0)
            b += 3;
        else
            b += 4;
        ++charIndex;
    }
    return charIndex;
}

void RGAManager::localInsert(int bytePos, QChar ch)
{
    ++timestamp;
    const QByteArray utf8 = sequence.toText().toUtf8();
    const int charPos = byteOffsetToCharIndex(utf8, bytePos);

    RGANode node;
    node.id.timestamp = timestamp;
    node.id.siteId = siteId;
    node.parent = sequence.idAtPosition(charPos - 1);
    node.val = ch;
    node.tombstone = false;

    sequence.insert(node);

    UndoOp op{UndoOp::Type::Insert, node};
    if (grouping) {
        pendingGroup.append(op);
    } else {
        undoStack.push({op});
    }
    redoStack.clear();
    notifyUndoState();

    QJsonObject msg;
    msg["type"] = "insert";
    msg["node"] = nodeToJson(node);
    sendMessage(msg);

    emit textChanged(sequence.toText());
}

void RGAManager::localRemove(int bytePos)
{
    const QByteArray utf8 = sequence.toText().toUtf8();
    const int charPos = byteOffsetToCharIndex(utf8, bytePos);

    RGAId id = sequence.idAtPosition(charPos);
    if (id.timestamp == 0 && id.siteId == 0)
        return;

    RGANode *nodePtr = sequence.findById(id);
    if (!nodePtr)
        return;
    const RGANode savedNode = *nodePtr;

    sequence.remove(id);

    UndoOp op{UndoOp::Type::Delete, savedNode};
    if (grouping) {
        pendingGroup.append(op);
    } else {
        undoStack.push({op});
    }
    redoStack.clear();
    notifyUndoState();

    QJsonObject msg;
    msg["type"] = "delete";
    msg["id"] = idToJson(id);
    msg["siteId"] = siteId;
    sendMessage(msg);

    emit textChanged(sequence.toText());
}

void RGAManager::remoteInsert(const RGANode &node)
{
    timestamp = qMax(timestamp, node.id.timestamp) + 1;
    sequence.insert(node);
    emit remoteTextChanged(sequence.toText());
}

void RGAManager::remoteDelete(const RGAId &id)
{
    sequence.remove(id);
    emit remoteTextChanged(sequence.toText());
}

void RGAManager::processMessage(const QJsonObject &obj)
{
    const QString type = obj["type"].toString();

    if (type == "insert") {
        remoteInsert(nodeFromJson(obj["node"].toObject()));
    } else if (type == "delete") {
        remoteDelete(idFromJson(obj["id"].toObject()));
    } else if (type == "snapshot") {
        QString path = obj["path"].toString();
        if (path.isEmpty())
            path = obj["file"].toString();
        const QString filename = QFileInfo(path).fileName();
        const QString text = obj["text"].toString();

        buildFromText(text);

        emit onInitReceived(sequence.toText(), filename);
        emit remoteTextChanged(sequence.toText());
    } else if (type == "cursor") {
        const int sid = obj["siteId"].toInt();
        const int pos = obj["position"].toInt();
        if (sid != siteId)
            emit remoteCursorMoved(sid, pos);
    } else if (type == "cursor_leave") {
        emit remoteCursorLeft(obj["siteId"].toInt());
    } else if (type == "undelete") {
        sequence.undelete(idFromJson(obj["id"].toObject()));
        emit remoteTextChanged(sequence.toText());
    } else if (type == "user_list") {
        QJsonArray arr = obj["siteIds"].toArray();
        QList<int> ids;
        for (const QJsonValue &v : arr)
            if (!v.isNull())
                ids.append(v.toInt());
        emit usersUpdated(ids);
    }
}

RGAId RGAManager::idFromJson(const QJsonObject &obj)
{
    return {obj["timestamp"].toInt(), obj["siteId"].toInt()};
}

RGANode RGAManager::nodeFromJson(const QJsonObject &obj)
{
    RGANode node;
    node.id = idFromJson(obj["id"].toObject());
    node.parent = idFromJson(obj["parent"].toObject());
    const QString v = obj["val"].toString();
    node.val = v.isEmpty() ? QChar() : v.at(0);
    node.tombstone = obj["tombstone"].toBool();
    return node;
}

QJsonObject RGAManager::idToJson(const RGAId &id) const
{
    QJsonObject obj;
    obj["timestamp"] = id.timestamp;
    obj["siteId"] = id.siteId;
    return obj;
}

QJsonObject RGAManager::nodeToJson(const RGANode &node) const
{
    QJsonObject obj;
    obj["id"] = idToJson(node.id);
    obj["parent"] = idToJson(node.parent);
    obj["val"] = QString(node.val);
    obj["tombstone"] = node.tombstone;
    return obj;
}

void RGAManager::sendMessage(QJsonObject msg)
{
    if (!sendFn)
        return;
    if (!filePath.isEmpty() && !msg.contains("file"))
        msg["file"] = filePath;
    sendFn(msg);
}

void RGAManager::buildFromText(const QString &text)
{
    sequence.clear();
    sequence.rgaseq.reserve(text.size());
    timestamp = 0;

    RGAId parent{};
    for (int i = 0; i < text.size(); ++i) {
        ++timestamp;
        RGANode node;
        node.id = {timestamp, 0};
        node.parent = parent;
        node.val = text.at(i);
        node.tombstone = false;
        sequence.rgaseq.append(node);
        parent = node.id;
    }
    sequence.rebuildIndex();
}

void RGAManager::sendInitText(const QString &text, const QString &path)
{
    QJsonObject msg;
    msg["type"] = "snapshot";
    msg["text"] = text;
    msg["path"] = path;
    sendMessage(msg);
}

void RGAManager::sendCursorPosition(int scintillaPos)
{
    QJsonObject msg;
    msg["type"] = "cursor";
    msg["siteId"] = siteId;
    msg["position"] = scintillaPos;
    sendMessage(msg);
}

void RGAManager::notifyUndoState()
{
    emit undoAvailableChanged(!undoStack.isEmpty());
    emit redoAvailableChanged(!redoStack.isEmpty());
}

void RGAManager::beginGroup()
{
    grouping = true;
    pendingGroup.clear();
}

void RGAManager::endGroup()
{
    grouping = false;
    if (!pendingGroup.isEmpty()) {
        undoStack.push(pendingGroup);
        pendingGroup.clear();
        notifyUndoState();
    }
}

void RGAManager::undo()
{
    if (grouping && !pendingGroup.isEmpty()) {
        undoStack.push(pendingGroup);
        pendingGroup.clear();
        grouping = false;
    }
    if (undoStack.isEmpty())
        return;

    const auto group = undoStack.pop();
    QVector<UndoOp> reverseGroup;

    for (int i = group.size() - 1; i >= 0; --i) {
        const UndoOp &op = group[i];
        QJsonObject msg;
        if (op.type == UndoOp::Type::Insert) {
            sequence.remove(op.node.id);
            msg["type"] = "delete";
            msg["id"] = idToJson(op.node.id);
        } else {
            sequence.undelete(op.node.id);
            msg["type"] = "undelete";
            msg["id"] = idToJson(op.node.id);
        }
        sendMessage(msg);
        reverseGroup.append(op);
    }

    redoStack.push(reverseGroup);
    notifyUndoState();
    emit remoteTextChanged(sequence.toText());
}

void RGAManager::redo()
{
    if (redoStack.isEmpty())
        return;

    const auto group = redoStack.pop();
    QVector<UndoOp> reverseGroup;

    for (int i = group.size() - 1; i >= 0; --i) {
        const UndoOp &op = group[i];
        QJsonObject msg;
        if (op.type == UndoOp::Type::Insert) {
            sequence.undelete(op.node.id);
            msg["type"] = "undelete";
            msg["id"] = idToJson(op.node.id);
        } else {
            sequence.remove(op.node.id);
            msg["type"] = "delete";
            msg["id"] = idToJson(op.node.id);
        }
        sendMessage(msg);
        reverseGroup.append(op);
    }

    undoStack.push(reverseGroup);
    notifyUndoState();
    emit remoteTextChanged(sequence.toText());
}

void RGAManager::debug()
{
    for (const RGANode &node : sequence.rgaseq)
        qDebug() << "VAL:" << node.val << "ID:" << node.id.timestamp << node.id.siteId
                 << "PARENT:" << node.parent.timestamp << node.parent.siteId
                 << "TOMBSTONE:" << node.tombstone;
}
