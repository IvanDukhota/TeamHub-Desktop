#ifndef VOICECHAT_H
#define VOICECHAT_H

#include <QAudioSink>
#include <QAudioSource>
#include <QHostInfo>
#include <QIODevice>
#include <QJsonArray>
#include <QMap>
#include <QNetworkInterface>
#include <QObject>
#include <QRandomGenerator>
#include <QElapsedTimer>
#include <QTimer>
#include <QUdpSocket>
#include <QWebSocket>
#include <QtEndian>
#include <cstring>

#include <opus/opus.h>

struct PeerInfo
{
    int id = 0;
    QString ip;
    quint16 port = 0;
    QString name;
    QString avatarUrl;
    bool connected = false;
    QAudioSink *sink = nullptr;
    QIODevice *output = nullptr;
    OpusDecoder *decoder = nullptr;
    bool locallyMuted = false;
    float localVolume = 1.0f;
};

class VoiceChat : public QObject
{
    Q_OBJECT

public:
    explicit VoiceChat(QObject *parent = nullptr);
    ~VoiceChat();

    void connectToServer(const QString &host, quint16 port);
    void disconnectFromServer();
    void startCall();
    void stopCall();
    bool isCallActive() const;
    bool isConnected() const;
    void setRoom(const QString &r);
    void setAuthToken(const QString &t) { authToken = t; }
    void setTeamId(const QString &t) { teamId = t; }
    void setUsername(const QString &n) { username = n; }
    void setAvatarUrl(const QString &u) { avatarUrl_ = u; }
    int id() const { return publicId; }
    bool isHost() const { return isRoomHost_; }
    QString peerName(int peerId) const;
    QString peerAvatarUrl(int peerId) const;

    void setPeerMuted(int peerId, bool muted);
    void setPeerVolume(int peerId, float volume);
    bool isPeerMuted(int peerId) const;
    float peerVolume(int peerId) const;
    void kickPeer(int peerId);

    bool micMuted = false;
    bool audioMuted = false;

    void setMicMuted(bool m) { micMuted = m; }
    void setAudioMuted(bool m) { audioMuted = m; }

signals:
    void hostStatusChanged(bool isHost);
    void voipKicked();
    void statusChanged(const QString &status);
    void peerConnected(const QString &ip, quint16 port);
    void peerDisconnected(const QString &ip, quint16 port);
    void connectedToServer();
    void disconnectedFromServer();
    void peersUpdated(const QStringList &ids);
    void registrationDenied(const QString &reason);
    void speakingChanged(bool speaking);
    void peerSpeakingChanged(int peerId, bool speaking);

private slots:
    void onUdpReadyRead();
    void onPunchTimerTimeout();
    void onStunTimeout();
    void onWebSocketConnected();
    void onWebSocketDisconnected();
    void onWebSocketTextMessageReceived(const QString &message);
    void onAudioInputReady();

private:
    void performStun();
    void parseStunResponse(const QByteArray &data);
    void registerWithServer();
    void updatePeerList(const QJsonArray &peerArray);
    void markPeerConnected(int index);
    void createPeerSink(PeerInfo &peer);
    void destroyPeerSink(PeerInfo &peer);

    void setMode(const QString &m);

    static QAudioFormat audioFormat();

    QWebSocket *webSocket;
    QUdpSocket *udpSocket;
    QTimer *punchTimer;
    QTimer *stunTimer;

    QAudioSource *audioSource = nullptr;
    QIODevice *audioInput = nullptr;

    QList<PeerInfo> peers;

    QString publicIp;
    int publicId;
    quint16 publicPort = 0;
    QByteArray stunTransactionId;

    QString serverHost;
    quint16 serverPort = 0;

    OpusEncoder *opusEncoder = nullptr;
    QByteArray captureBuffer;
    static constexpr int OPUS_FRAME_SIZE = 320;
    QTimer *flushTimer;

    QString room = "default";
    QString mode = "hybrid"; // relay / hybrid / p2p
    bool isRoomHost_ = false;

    QString authToken;
    QString teamId;
    QString username;
    QString avatarUrl_;

    QTimer *silenceTimer = nullptr;
    bool isSpeaking = false;

    float vadNoiseFloor = 150.0f;
    bool vadActive = false;
    QElapsedTimer vadSpeechTimer;
    int vadConsecutive = 0;
    static constexpr int   VAD_HANGOVER_MS      = 700;
    static constexpr float VAD_MULTIPLIER       = 2.0f;
    static constexpr float VAD_HOLD_MULTIPLIER  = 1.2f;
    static constexpr float VAD_ADAPT_RATE       = 0.01f;
    static constexpr float VAD_MIN_NOISE_FLOOR  = 80.0f;
    static constexpr int   VAD_CONFIRM_FRAMES   = 2;
};

#endif // VOICECHAT_H