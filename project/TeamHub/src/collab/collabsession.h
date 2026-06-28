#ifndef COLLABSESSION_H
#define COLLABSESSION_H

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QQueue>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QWebSocket>

#include "../rga/rgamanager.h"
#include "../rga/rgaseq.h"
#include "sessionreport.h"

class CollabSession : public QObject
{
    Q_OBJECT
public:
    enum class Role { Host, Guest };
    enum class Mode { ReadWrite, ReadOnly };

    explicit CollabSession(int siteId, Role role, QObject *parent = nullptr);
    void setUsername(const QString &name) { username = name; }
    void setAvatarUrl(const QString &url) { avatarUrl = url; }
    ~CollabSession() override;

    void connectToServer(const QString &url);
    void disconnectFromServer();
    bool isConnected() const;

    RGAManager *getOrCreateRGA(const QString &relPath);
    void releaseRGA(const QString &relPath);
    bool hasActiveRGA(const QString &relPath) const { return active.contains(relPath); }
    void setProject(const QString &projectRoot, const QStringList &relFiles);
    void initFileCache(const QString &relPath, const QString &text);
    void sendAllSnapshots(const QMap<QString, QString> &fileTexts);
    void broadcastRunOutput(const QString &text);
    void notifyFileCreated(const QString &relPath, const QString &text = {});
    void notifyFileDeleted(const QString &relPath);
    void notifyFileRenamed(const QString &oldPath, const QString &newPath);
    void sendCursorLeave(const QString &relPath);
    void sendFileFocus(const QString &relPath);
    void kickUser(int siteId);
    void sendRoleChange(int targetSiteId, const QString &role);
    void sendFinalStates(const QMap<QString, QString> &texts);
    void requestSessionReport();
    void endSession();

    Role role() const { return currentRole; }
    Mode collabMode() const { return mode; }
    void setCollabMode(Mode m) { mode = m; }
    int siteId() const { return currentSiteId; }
    QStringList fileList() const { return files; }
    QString projectRoot() const { return rootPath; }
    qint64 sessionStartEpoch() const { return startEpoch; }

    bool hasTextCache(const QString &relPath) const { return textCache.contains(relPath); }
    QString cachedText(const QString &relPath) const { return textCache.value(relPath); }
    QMap<int, int> fileCursors(const QString &relPath) const { return cursorCache.value(relPath); }
    QMap<int, QString> avatars() const { return peerAvatars; }

signals:
    void connected();
    void disconnected();
    void kicked();
    void errorOccurred(const QString &err);
    void reconnecting(int attempt, int maxAttempts);
    void projectInitReceived(int hostSiteId, const QStringList &files);
    void runOutputReceived(const QString &text);
    void usersUpdated(QMap<int, QString> users);
    void rolesUpdated(QMap<int, QString> roles);
    void remoteFileCreated(const QString &relPath);
    void remoteFileDeleted(const QString &relPath);
    void remoteFileRenamed(const QString &oldPath, const QString &newPath);
    void remoteFileFocusChanged(int siteId, const QString &file);
    void sessionReportReady(const SessionReportData &report);
    void sessionAiInsightsReady(const AiInsights &ai);
    void peerRoleChanged(int siteId, QString role);
    void remoteOpReceived(int siteId, const QString &opType);

private slots:
    void onConnected();
    void onDisconnected();
    void onRawMessage(const QString &message);
    void onError(QAbstractSocket::SocketError error);

private:
    struct FileState
    {
        RGASequence sequence;
        int maxTimestamp = 0;
    };

    void handleMessage(const QJsonObject &obj);
    void sendMessage(const QJsonObject &msg);
    void sendRegister();
    void evictLRU();
    void applyOpToInactive(const QString &file, const QJsonObject &op);

    static SessionReportData parseSessionReport(const QJsonObject &data);
    static AiInsights parseAiInsights(const QJsonObject &data);

    void scheduleReconnect();

    static constexpr int MAX_ACTIVE = 5;
    static constexpr int MAX_RECONNECT_ATTEMPTS = 5;

    QWebSocket *socket;
    QTimer *reconnectTimer;
    QString serverUrl;
    int reconnectAttempt = 0;
    bool wantReconnect = false;
    bool sessionEndedByServer = false;

    int currentSiteId;
    Role currentRole;
    Mode mode = Mode::ReadWrite;
    QString username;
    QString avatarUrl;
    QString rootPath;
    QStringList files;
    QMap<int, QString> peerAvatars;

    QMap<QString, RGAManager *> active;
    QMap<QString, FileState> fileStates;
    QMap<QString, QString> textCache;
    QMap<QString, QDateTime> lastUsed;
    QMap<QString, QMap<int, int>> cursorCache;
    qint64 startEpoch = 0;

    QQueue<QJsonObject> pendingQueue;
};

#endif // COLLABSESSION_H
