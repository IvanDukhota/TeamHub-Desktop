#include "voicechat.h"

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMediaDevices>
#include <QNetworkDatagram>
#include <cmath>

static constexpr const char *STUN_HOST = "stun.l.google.com";
static constexpr quint16 STUN_PORT = 19302;
static constexpr int PUNCH_INTERVAL = 500;
static constexpr int STUN_TIMEOUT = 5000;

static constexpr quint16 STUN_BINDING_REQUEST = 0x0001;
static constexpr quint16 STUN_BINDING_RESPONSE = 0x0101;
static constexpr quint32 STUN_MAGIC_COOKIE = 0x2112A442;
static constexpr quint16 ATTR_MAPPED_ADDRESS = 0x0001;
static constexpr quint16 ATTR_XOR_MAPPED_ADDRESS = 0x0020;

static const QByteArray PUNCH_PAYLOAD = QByteArrayLiteral("PUNCH");

QAudioFormat VoiceChat::audioFormat()
{
    QAudioFormat fmt;
    fmt.setSampleRate(16000);
    fmt.setChannelCount(1);
    fmt.setSampleFormat(QAudioFormat::Int16);
    return fmt;
}

VoiceChat::VoiceChat(QObject *parent)
    : QObject(parent)
    , webSocket(new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this))
    , udpSocket(new QUdpSocket(this))
    , punchTimer(new QTimer(this))
    , stunTimer(new QTimer(this))
{
    connect(webSocket, &QWebSocket::connected, this, &VoiceChat::onWebSocketConnected);
    connect(webSocket, &QWebSocket::disconnected, this, &VoiceChat::onWebSocketDisconnected);
    connect(webSocket,
            &QWebSocket::textMessageReceived,
            this,
            &VoiceChat::onWebSocketTextMessageReceived);

    connect(udpSocket, &QUdpSocket::readyRead, this, &VoiceChat::onUdpReadyRead);

    punchTimer->setInterval(PUNCH_INTERVAL);
    connect(punchTimer, &QTimer::timeout, this, &VoiceChat::onPunchTimerTimeout);

    stunTimer->setSingleShot(true);
    stunTimer->setInterval(STUN_TIMEOUT);
    connect(stunTimer, &QTimer::timeout, this, &VoiceChat::onStunTimeout);

    publicId = QRandomGenerator::global()->bounded(100000, 999999);

    flushTimer = new QTimer(this);
    flushTimer->setSingleShot(false);
    flushTimer->setInterval(40);

    silenceTimer = new QTimer(this);
    silenceTimer->setSingleShot(true);
    silenceTimer->setInterval(400);
    connect(silenceTimer, &QTimer::timeout, this, [this]() {
        if (!isSpeaking)
            return;
        isSpeaking = false;
        emit speakingChanged(false);
        if (webSocket->state() == QAbstractSocket::ConnectedState) {
            QJsonObject msg;
            msg["type"] = "speaking";
            msg["id"] = publicId;
            msg["speaking"] = false;
            webSocket->sendTextMessage(QJsonDocument(msg).toJson(QJsonDocument::Compact));
        }
    });
}

VoiceChat::~VoiceChat()
{
    stopCall();
    punchTimer->stop();
    stunTimer->stop();
    webSocket->close();
    udpSocket->close();
}

void VoiceChat::connectToServer(const QString &host, quint16 port)
{
    serverHost = host;
    serverPort = port;

    if (udpSocket->state() != QAbstractSocket::BoundState) {
        udpSocket->close();
        if (!udpSocket->bind(QHostAddress::AnyIPv4,
                             0,
                             QAbstractSocket::ShareAddress | QAbstractSocket::ReuseAddressHint)) {
            emit statusChanged("UDP bind failed: " + udpSocket->errorString());
            return;
        }
    }

    if (host == "localhost" || host == "127.0.0.1") {
        publicIp = "127.0.0.1";
        publicPort = udpSocket->localPort();
        webSocket->open(QUrl(QString("ws://%1:%2").arg(host).arg(port)));
        return;
    }

    emit statusChanged("Performing STUN discovery…");
    performStun();
}

void VoiceChat::disconnectFromServer()
{
    punchTimer->stop();
    stunTimer->stop();
    flushTimer->stop();

    stopCall();
    peers.clear();
    publicIp.clear();
    publicPort = 0;

    if (webSocket->state() != QAbstractSocket::UnconnectedState)
        webSocket->abort();
}

void VoiceChat::startCall()
{
    if (audioSource || audioInput || opusEncoder) {
        return;
    }
    QAudioFormat fmt = audioFormat();
    QAudioDevice inputDev = QMediaDevices::defaultAudioInput();

    if (!inputDev.isFormatSupported(fmt)) {
        emit statusChanged("Microphone does not support 16kHz/16-bit/mono.");
        return;
    }

    audioSource = new QAudioSource(inputDev, fmt, this);
    audioSource->setBufferSize(4096);
    audioInput = audioSource->start();
    int err;
    opusEncoder = opus_encoder_create(16000, 1, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK) {
        emit statusChanged("Opus encoder error: " + QString(opus_strerror(err)));
        return;
    }
    opus_encoder_ctl(opusEncoder, OPUS_SET_BITRATE(24000)); // 24 kbps

    connect(audioInput, &QIODevice::readyRead, this, &VoiceChat::onAudioInputReady);

    for (PeerInfo &peer : peers)
        if (peer.connected)
            createPeerSink(peer);

    emit statusChanged(QString("Call active — %1 peer(s)").arg(peers.size()));
}

void VoiceChat::stopCall()
{
    flushTimer->stop();
    captureBuffer.clear();
    punchTimer->stop();

    silenceTimer->stop();
    if (isSpeaking) {
        isSpeaking = false;
        emit speakingChanged(false);
    }

    if (audioInput) {
        disconnect(audioInput, &QIODevice::readyRead, this, &VoiceChat::onAudioInputReady);
    }

    if (audioSource) {
        audioSource->stop();
        audioSource->deleteLater();

        audioSource = nullptr;
        audioInput = nullptr;
    }

    for (PeerInfo &peer : peers)
        destroyPeerSink(peer);

    if (opusEncoder) {
        opus_encoder_destroy(opusEncoder);
        opusEncoder = nullptr;
    }
}

bool VoiceChat::isCallActive() const
{
    return audioSource != nullptr;
}

bool VoiceChat::isConnected() const
{
    return webSocket->state() == QAbstractSocket::ConnectedState;
}

void VoiceChat::performStun()
{
    QByteArray pkt(20, '\0');
    auto *d = reinterpret_cast<uchar *>(pkt.data());
    qToBigEndian<quint16>(STUN_BINDING_REQUEST, d);
    qToBigEndian<quint16>(0, d + 2);
    qToBigEndian<quint32>(STUN_MAGIC_COOKIE, d + 4);

    stunTransactionId.resize(12);
    for (int i = 0; i < 12; ++i)
        stunTransactionId[i] = static_cast<char>(QRandomGenerator::global()->bounded(256));
    std::memcpy(d + 8, stunTransactionId.constData(), 12);

    QHostInfo::lookupHost("stun.l.google.com", this, [this, pkt](const QHostInfo &info) {
        if (info.error() != QHostInfo::NoError || info.addresses().isEmpty()) {
            emit statusChanged("STUN DNS failed: " + info.errorString());
            return;
        }
        udpSocket->writeDatagram(pkt, info.addresses().constFirst(), 19302);
        stunTimer->start();
    });
}

void VoiceChat::parseStunResponse(const QByteArray &data)
{
    stunTimer->stop();

    if (data.size() < 20)
        return;

    const auto *d = reinterpret_cast<const uchar *>(data.constData());

    if (qFromBigEndian<quint16>(d) != STUN_BINDING_RESPONSE)
        return;

    if (qFromBigEndian<quint32>(d + 4) != STUN_MAGIC_COOKIE)
        return;

    int offset = 20;

    while (offset + 4 <= data.size()) {
        quint16 attrType = qFromBigEndian<quint16>(d + offset);

        quint16 attrLen = qFromBigEndian<quint16>(d + offset + 2);

        offset += 4;

        if (offset + attrLen > data.size())
            break;

        auto extract = [&](bool xorAddr) -> bool {
            if (attrLen < 8)
                return false;

            if (static_cast<quint8>(data[offset + 1]) != 0x01)
                return false;

            quint16 rawPort = qFromBigEndian<quint16>(d + offset + 2);

            quint32 rawIp = qFromBigEndian<quint32>(d + offset + 4);

            if (xorAddr) {
                rawPort ^= quint16(STUN_MAGIC_COOKIE >> 16);
                rawIp ^= STUN_MAGIC_COOKIE;
            }

            publicPort = rawPort;

            publicIp = QString("%1.%2.%3.%4")
                           .arg((rawIp >> 24) & 0xFF)
                           .arg((rawIp >> 16) & 0xFF)
                           .arg((rawIp >> 8) & 0xFF)
                           .arg(rawIp & 0xFF);

            return true;
        };

        if (attrType == ATTR_XOR_MAPPED_ADDRESS) {
            if (extract(true))
                break;
        }

        if (attrType == ATTR_MAPPED_ADDRESS) {
            if (extract(false))
                break;
        }

        offset += (attrLen + 3) & ~3;
    }

    if (publicIp.isEmpty())
        return;

    if (publicPort == 0)
        return;

    emit statusChanged(QString("Public: %1:%2").arg(publicIp).arg(publicPort));

    if (webSocket->state() == QAbstractSocket::ConnectedState) {
        return;
    }

    if (webSocket->state() == QAbstractSocket::ConnectingState) {
        return;
    }

    webSocket->abort();

    webSocket->open(QUrl(QString("ws://%1:%2").arg(serverHost).arg(serverPort)));
}

void VoiceChat::onStunTimeout()
{
    stunTimer->stop();

    emit statusChanged("STUN timeout. Using local address.");

    publicIp.clear();

    const QList<QHostAddress> addresses = QNetworkInterface::allAddresses();

    for (const QHostAddress &addr : addresses) {
        if (addr.protocol() != QAbstractSocket::IPv4Protocol)
            continue;

        if (addr.isLoopback())
            continue;

        publicIp = addr.toString();
        break;
    }

    if (publicIp.isEmpty())
        publicIp = "0.0.0.0";

    publicPort = udpSocket->localPort();

    if (webSocket->state() == QAbstractSocket::ConnectingState) {
        return;
    }

    if (webSocket->state() == QAbstractSocket::ConnectedState) {
        return;
    }

    webSocket->abort();

    webSocket->open(QUrl(QString("ws://%1:%2").arg(serverHost).arg(serverPort)));
}

void VoiceChat::onWebSocketConnected()
{
    emit statusChanged(QString("Connected. Public: %1:%2").arg(publicIp).arg(publicPort));

    registerWithServer();

    emit connectedToServer();
}

void VoiceChat::setPeerMuted(int peerId, bool muted)
{
    for (PeerInfo &p : peers)
        if (p.id == peerId) {
            p.locallyMuted = muted;
            return;
        }
}

void VoiceChat::setPeerVolume(int peerId, float volume)
{
    for (PeerInfo &p : peers) {
        if (p.id == peerId) {
            p.localVolume = qBound(0.0f, volume, 2.0f);
            if (p.sink)
                p.sink->setVolume(p.localVolume);
            return;
        }
    }
}

bool VoiceChat::isPeerMuted(int peerId) const
{
    for (const PeerInfo &p : peers)
        if (p.id == peerId)
            return p.locallyMuted;
    return false;
}

float VoiceChat::peerVolume(int peerId) const
{
    for (const PeerInfo &p : peers)
        if (p.id == peerId)
            return p.localVolume;
    return 1.0f;
}

void VoiceChat::kickPeer(int peerId)
{
    if (!isRoomHost_)
        return;
    QJsonObject msg;
    msg["type"] = "voip_kick";
    msg["target"] = peerId;
    msg["id"] = publicId;
    webSocket->sendTextMessage(QJsonDocument(msg).toJson(QJsonDocument::Compact));
}

void VoiceChat::onWebSocketDisconnected()
{
    punchTimer->stop();

    stopCall();

    peers.clear();

    emit disconnectedFromServer();

    emit statusChanged("Disconnected from server.");
}

void VoiceChat::onWebSocketTextMessageReceived(const QString &message)
{
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError)
        return;

    QJsonObject obj = doc.object();
    QString type = obj.value("type").toString();

    if (type == "peer_list") {
        const int hostId = obj.value("host_id").toInt(-1);
        const bool wasHost = isRoomHost_;
        isRoomHost_ = (hostId == publicId);
        if (isRoomHost_ != wasHost)
            emit hostStatusChanged(isRoomHost_);
        updatePeerList(obj.value("peers").toArray());
        return;
    }

    if (type == "speaking") {
        emit peerSpeakingChanged(obj.value("id").toInt(), obj.value("speaking").toBool());
        return;
    }

    if (type == "voip_kicked") {
        emit voipKicked();
        disconnectFromServer();
        return;
    }

    if (type == "register_denied") {
        const QString reason = obj.value("reason").toString("Access denied.");
        emit registrationDenied(reason);
        emit statusChanged(reason);
        disconnectFromServer();
        return;
    }
}

void VoiceChat::registerWithServer()
{
    QJsonObject msg;

    msg["type"] = "register";
    msg["ip"] = publicIp;
    msg["port"] = static_cast<int>(publicPort);
    msg["id"] = publicId;

    msg["room"] = room;
    msg["mode"] = mode;
    msg["token"] = authToken;
    msg["team_id"] = teamId;
    msg["username"] = username;
    msg["avatarUrl"] = avatarUrl_;

    webSocket->sendTextMessage(QJsonDocument(msg).toJson(QJsonDocument::Compact));
}

QString VoiceChat::peerName(int peerId) const
{
    for (const PeerInfo &p : peers)
        if (p.id == peerId)
            return p.name;
    return QString();
}

QString VoiceChat::peerAvatarUrl(int peerId) const
{
    for (const PeerInfo &p : peers)
        if (p.id == peerId)
            return p.avatarUrl;
    return QString();
}

void VoiceChat::updatePeerList(const QJsonArray &peerArray)
{
    struct Entry
    {
        QString ip;
        quint16 port;
        int id;
        QString name;
        QString avatarUrl;
    };

    QList<Entry> incoming;

    for (const QJsonValue &v : peerArray) {
        QJsonObject o = v.toObject();

        Entry e{o.value("ip").toString(),
                quint16(o.value("port").toInt()),
                o.value("id").toInt(),
                o.value("username").toString(),
                o.value("avatarUrl").toString()};

        if (e.id == publicId)
            continue;

        incoming.append(e);
    }

    for (int i = peers.size() - 1; i >= 0; --i) {
        bool found = false;

        for (const Entry &e : incoming) {
            if (e.id == peers[i].id) {
                found = true;
                break;
            }
        }

        if (!found) {
            emit peerDisconnected(peers[i].ip, peers[i].port);

            destroyPeerSink(peers[i]);

            peers.removeAt(i);
        }
    }

    for (const Entry &e : incoming) {
        bool already = false;

        for (PeerInfo &p : peers) {
            if (p.id == e.id) {
                already = true;
                p.name = e.name;
                p.avatarUrl = e.avatarUrl;
                break;
            }
        }

        if (!already) {
            PeerInfo peer;
            peer.ip = e.ip;
            peer.port = e.port;
            peer.id = e.id;
            peer.name = e.name;
            peer.avatarUrl = e.avatarUrl;

            peers.append(peer);

            emit statusChanged(
                QString("New peer: %1:%2 [%3]").arg(peer.ip).arg(peer.port).arg(peer.id));
        }
    }

    bool anyUnconnected = false;

    for (const PeerInfo &p : std::as_const(peers)) {
        if (!p.connected) {
            anyUnconnected = true;
            break;
        }
    }

    if (anyUnconnected && !punchTimer->isActive())
        punchTimer->start();

    QStringList ids;
    ids.reserve(peers.size());

    for (const PeerInfo &p : std::as_const(peers)) {
        ids << QString::number(p.id);
    }

    emit peersUpdated(ids);
}

void VoiceChat::onPunchTimerTimeout()
{
    bool allDone = true;

    for (PeerInfo &peer : peers) {
        if (!peer.connected) {
            allDone = false;

            udpSocket->writeDatagram(PUNCH_PAYLOAD, QHostAddress(peer.ip), peer.port);

            udpSocket->writeDatagram(QByteArray("HELLO"), QHostAddress(peer.ip), peer.port);
        }
    }

    if (allDone) {
        punchTimer->stop();

        emit statusChanged(QString("All %1 peer(s) connected").arg(peers.size()));
    }
}

void VoiceChat::onUdpReadyRead()
{
    while (udpSocket && udpSocket->hasPendingDatagrams()) {
        QNetworkDatagram dg = udpSocket->receiveDatagram();

        QByteArray data = dg.data();

        QHostAddress senderAddr = dg.senderAddress();
        quint16 senderPort = static_cast<quint16>(dg.senderPort());

        if (data.size() >= 20) {
            const auto *raw = reinterpret_cast<const uchar *>(data.constData());

            if (qFromBigEndian<quint32>(raw + 4) == STUN_MAGIC_COOKIE) {
                parseStunResponse(data);
                continue;
            }
        }

        QString senderIp = senderAddr.toString().remove("::ffff:");

        int idx = -1;

        for (int i = 0; i < peers.size(); ++i) {
            const PeerInfo &p = peers[i];

            if (p.ip == senderIp && p.port == senderPort) {
                idx = i;
                break;
            }
        }

        if (data == PUNCH_PAYLOAD || data == "HELLO") {
            if (idx != -1)
                markPeerConnected(idx);

            if (udpSocket->state() == QAbstractSocket::BoundState) {
                udpSocket->writeDatagram(PUNCH_PAYLOAD, senderAddr, senderPort);
            }

            continue;
        }

        if (idx < 0 || idx >= peers.size())
            continue;

        PeerInfo &peer = peers[idx];

        if (!peer.connected)
            markPeerConnected(idx);

        if (!peer.decoder)
            continue;

        if (!peer.sink)
            continue;

        if (!peer.output)
            continue;

        QByteArray pcmOut(OPUS_FRAME_SIZE * sizeof(opus_int16), '\0');

        int samples = opus_decode(peer.decoder,
                                  reinterpret_cast<const unsigned char *>(data.constData()),
                                  data.size(),
                                  reinterpret_cast<opus_int16 *>(pcmOut.data()),
                                  OPUS_FRAME_SIZE,
                                  0);

        if (samples <= 0) {
            qDebug() << "opus_decode error:" << opus_strerror(samples);

            continue;
        }

        if (audioMuted || peer.locallyMuted)
            continue;

        qint64 bytesWritten = peer.output->write(pcmOut.constData(), samples * sizeof(opus_int16));

        if (bytesWritten < 0) {
            qDebug() << "Audio output write failed";
        }
    }
}

void VoiceChat::onAudioInputReady()
{
    if (!audioInput || !opusEncoder)
        return;

    QByteArray data = audioInput->readAll();

    {
        const int n = data.size() / 2;
        const auto *s = reinterpret_cast<const opus_int16 *>(data.constData());
        float sum = 0.0f;
        for (int i = 0; i < n; ++i)
            sum += float(s[i]) * float(s[i]);
        const float rms = (n > 0 && !micMuted) ? std::sqrt(sum / float(n)) : 0.0f;

        if (rms > vadNoiseFloor * VAD_MULTIPLIER) {
            ++vadConsecutive;
            if (vadActive || vadConsecutive >= VAD_CONFIRM_FRAMES) {
                vadActive = true;
                vadSpeechTimer.restart();
            }
        } else {
            vadConsecutive = 0;
            if (vadActive) {
                if (rms > vadNoiseFloor * VAD_HOLD_MULTIPLIER)
                    vadSpeechTimer.restart();
                else if (vadSpeechTimer.elapsed() >= VAD_HANGOVER_MS)
                    vadActive = false;
            } else {
                float newFloor = vadNoiseFloor * (1.0f - VAD_ADAPT_RATE) + rms * VAD_ADAPT_RATE;
                vadNoiseFloor = std::max(newFloor, VAD_MIN_NOISE_FLOOR);
            }
        }

        if (vadActive) {
            silenceTimer->start();
            if (!isSpeaking) {
                isSpeaking = true;
                emit speakingChanged(true);
                if (webSocket->state() == QAbstractSocket::ConnectedState) {
                    QJsonObject msg;
                    msg["type"] = "speaking";
                    msg["id"] = publicId;
                    msg["speaking"] = true;
                    webSocket->sendTextMessage(QJsonDocument(msg).toJson(QJsonDocument::Compact));
                }
            }
        }
    }

    if (micMuted || !vadActive)
        return;

    captureBuffer.append(data);
    flushTimer->start();
    while (captureBuffer.size() >= OPUS_FRAME_SIZE * 2) {
        const opus_int16 *pcmData = reinterpret_cast<const opus_int16 *>(captureBuffer.constData());

        QByteArray encoded(4000, '\0');

        int encodedLen = opus_encode(opusEncoder,
                                     pcmData,
                                     OPUS_FRAME_SIZE,
                                     reinterpret_cast<unsigned char *>(encoded.data()),
                                     encoded.size());

        if (encodedLen > 0) {
            encoded.resize(encodedLen);

            for (const PeerInfo &peer : std::as_const(peers)) {
                if (peer.connected) {
                    udpSocket->writeDatagram(encoded, QHostAddress(peer.ip), peer.port);
                }
            }
        } else {
            qDebug() << "opus_encode error:" << opus_strerror(encodedLen);
        }

        captureBuffer.remove(0, OPUS_FRAME_SIZE * 2);
    }
}

void VoiceChat::createPeerSink(PeerInfo &peer)
{
    if (peer.sink)
        return;
    peer.sink = new QAudioSink(QMediaDevices::defaultAudioOutput(), audioFormat(), this);
    peer.output = peer.sink->start();

    if (!peer.decoder) {
        int err;
        peer.decoder = opus_decoder_create(16000, 1, &err);
        if (err != OPUS_OK) {
            emit statusChanged("Opus decoder error for peer: " + QString(opus_strerror(err)));
            peer.decoder = nullptr;
        }
    }
}

void VoiceChat::destroyPeerSink(PeerInfo &peer)
{
    if (peer.decoder) {
        opus_decoder_destroy(peer.decoder);
        peer.decoder = nullptr;
    }

    if (!peer.sink)
        return;

    peer.output = nullptr;

    peer.sink->stop();
    peer.sink->deleteLater();
    peer.sink = nullptr;
}

void VoiceChat::markPeerConnected(int index)
{
    if (index < 0 || index >= peers.size())
        return;

    PeerInfo &peer = peers[index];

    if (peer.connected)
        return;

    peer.connected = true;

    if (audioSource)
        createPeerSink(peer);

    emit peerConnected(peer.ip, peer.port);

    emit statusChanged(QString("Peer connected: %1:%2").arg(peer.ip).arg(peer.port));
}

void VoiceChat::setRoom(const QString &r)
{
    room = r;
}

void VoiceChat::setMode(const QString &m)
{
    mode = m;

    if (webSocket->state() != QAbstractSocket::ConnectedState)
        return;

    QJsonObject msg;
    msg["type"] = "mode";
    msg["mode"] = mode;
    msg["id"] = publicId;

    webSocket->sendTextMessage(QJsonDocument(msg).toJson(QJsonDocument::Compact));
}