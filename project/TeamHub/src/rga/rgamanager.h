#ifndef RGAMANAGER_H
#define RGAMANAGER_H
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QStack>
#include <QVector>
#include "rgaseq.h"
#include <functional>

struct UndoOp
{
    enum class Type { Insert, Delete } type;
    RGANode node;
};

class RGAManager : public QObject
{
    Q_OBJECT
public:
    explicit RGAManager(int siteId, QObject *parent = nullptr);

    void setFilePath(const QString &filePath);
    void setSendFunction(std::function<void(QJsonObject)> fn);
    void handleIncomingMessage(const QJsonObject &obj);

    QString getText();
    void buildFromText(const QString &text);
    void sendInitText(const QString &text, const QString &path);
    void sendCursorPosition(int scintillaPos);

    int getSiteId() const { return siteId; }
    QString getFilePath() const { return filePath; }
    bool canUndo() const { return !undoStack.isEmpty(); }
    bool canRedo() const { return !redoStack.isEmpty(); }

    RGASequence getSequence() const { return sequence; }
    void setSequence(const RGASequence &seq) { sequence = seq; }
    int getTimestamp() const { return timestamp; }
    void setTimestamp(int ts) { timestamp = ts; }

    static RGANode nodeFromJson(const QJsonObject &obj);
    static RGAId idFromJson(const QJsonObject &obj);

    void debug();

public slots:
    void localInsert(int position, QChar ch);
    void localRemove(int position);
    void undo();
    void redo();
    void beginGroup();
    void endGroup();

signals:
    void textChanged(const QString &newText);
    void remoteTextChanged(const QString &newText);
    void undoAvailableChanged(bool available);
    void redoAvailableChanged(bool available);
    void onInitReceived(QString text, QString filename);
    void remoteCursorMoved(int siteId, int position);
    void remoteCursorLeft(int siteId);
    void usersUpdated(QList<int> siteIds);

private:
    void notifyUndoState();
    void processMessage(const QJsonObject &obj);
    void remoteInsert(const RGANode &node);
    void remoteDelete(const RGAId &id);
    QJsonObject nodeToJson(const RGANode &node) const;
    QJsonObject idToJson(const RGAId &id) const;
    void sendMessage(QJsonObject msg);

    RGASequence sequence;
    int siteId;
    int timestamp;

    QString filePath;
    std::function<void(QJsonObject)> sendFn;

    QStack<QVector<UndoOp>> undoStack;
    QStack<QVector<UndoOp>> redoStack;
    QVector<UndoOp> pendingGroup;
    bool grouping = false;
};
#endif
