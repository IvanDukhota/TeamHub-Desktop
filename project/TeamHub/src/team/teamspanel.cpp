#include "teamspanel.h"

#include "../auth/authmanager.h"
#include "../avatar/avatar.h"
#include "../config/appconfig.h"
#include "../voicechat/voicechat.h"

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QCoreApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QHash>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QSlider>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidgetAction>

class AvatarRing : public QWidget
{
    QPixmap m_pix;
    QColor m_ringColor;
    int m_border;

public:
    AvatarRing(int totalSize, int border, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_ringColor(Qt::transparent)
        , m_border(border)
    {
        setFixedSize(totalSize, totalSize);
        setAttribute(Qt::WA_TranslucentBackground);
    }
    void setPixmap(const QPixmap &p)
    {
        m_pix = p;
        update();
    }
    void setRingColor(const QColor &c)
    {
        m_ringColor = c;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        if (m_ringColor.alpha() > 0) {
            p.setBrush(m_ringColor);
            p.setPen(Qt::NoPen);
            p.drawEllipse(rect());
        }
        if (!m_pix.isNull()) {
            const int d = m_border;
            const QRect r(d, d, width() - 2 * d, height() - 2 * d);
            QPainterPath path;
            path.addEllipse(r);
            p.setClipPath(path);
            p.drawPixmap(r, m_pix.scaled(r.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
        }
    }
};

static QIcon loadVoiceIcon(const QString &name)
{
    QImage img(QString(TEAMHUB_ICONS_DIR) + name);
    if (img.isNull())
        img = QImage(QCoreApplication::applicationDirPath() + "/icons/" + name);
    if (img.isNull())
        return QIcon();
    img = img.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) {
            const QColor c(img.pixel(x, y));
            if (c.red() > 230 && c.green() > 230 && c.blue() > 230)
                img.setPixel(x, y, qRgba(0, 0, 0, 0));
        }
    return QIcon(QPixmap::fromImage(img));
}

namespace {

QLabel *makeAvatar(const QString &text,
                   const QColor &color,
                   int size = 32,
                   const QString &avatarUrl = QString())
{
    auto *avatar = new QLabel;
    avatar->setFixedSize(size, size);
    avatar->setAlignment(Qt::AlignCenter);
    Avatar::load(avatar,
                 avatarUrl,
                 Avatar::letterPixmap(text, color, size),
                 size,
                 [avatar](QPixmap pix) { avatar->setPixmap(pix); });
    return avatar;
}

QLabel *makePill(const QString &text, bool highlighted)
{
    auto *pill = new QLabel(text);
    pill->setStyleSheet(QString("background: %1; color: %2; border-radius: 8px; "
                                "padding: 1px 8px; font-size: 10px;")
                            .arg(highlighted ? "#007acc" : "#3c3c3c")
                            .arg(highlighted ? "#ffffff" : "#aaaaaa"));
    return pill;
}

QLabel *makeSectionLabel(const QString &text)
{
    auto *lbl = new QLabel(text);
    lbl->setObjectName("stubLabel");
    return lbl;
}
}

TeamsPanel::TeamsPanel(QWidget *parent)
    : QWidget(parent)
{
    setupUi();
}

void TeamsPanel::setupUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    callPane = makeCallPane();
    root->addWidget(callPane);
    callPane->setVisible(false);

    pages = new QStackedWidget;
    pages->addWidget(makeTeamsPage());
    pages->addWidget(makeDetailPage());
    root->addWidget(pages, 1);
}

QWidget *TeamsPanel::makeCallPane()
{
    auto *pane = new QWidget;
    pane->setObjectName("callBanner");
    auto *cl = new QVBoxLayout(pane);
    cl->setContentsMargins(10, 10, 10, 10);
    cl->setSpacing(6);

    callStatusLabel = new QLabel("Disconnected");
    callStatusLabel->setObjectName("stubLabel");
    cl->addWidget(callStatusLabel);

    peersList = new QListWidget;
    peersList->setObjectName("teamList");
    peersList->setMinimumHeight(120);
    peersList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(peersList,
            &QListWidget::customContextMenuRequested,
            this,
            &TeamsPanel::onPeersContextMenu);
    cl->addWidget(peersList, 1);

    const QIcon icMicOn = loadVoiceIcon("mic_on.png");
    const QIcon icMicOff = loadVoiceIcon("mic_off.png");
    const QIcon icSound = loadVoiceIcon("sound.png");
    const QIcon icNoSound = loadVoiceIcon("no-sound.png");
    const QIcon icLeave = loadVoiceIcon("leave.png");

    const QString discordBtnStyle = "QPushButton {"
                                    "  background: transparent;"
                                    "  border: none;"
                                    "  border-radius: 6px;"
                                    "  padding: 0px;"
                                    "}"
                                    "QPushButton:hover { background: rgba(255,255,255,38); }"
                                    "QPushButton:pressed { background: rgba(255,255,255,20); }"
                                    "QPushButton:checked { background: rgba(255,255,255,18); }";

    auto makeIconBtn = [&discordBtnStyle](bool checkable) {
        auto *btn = new QPushButton;
        btn->setCheckable(checkable);
        btn->setFixedSize(34, 34);
        btn->setIconSize(QSize(20, 20));
        btn->setStyleSheet(discordBtnStyle);
        return btn;
    };

    btnMuteMic = makeIconBtn(true);
    btnMuteMic->setToolTip("Mute microphone");
    if (!icMicOn.isNull())
        btnMuteMic->setIcon(icMicOn);
    else
        btnMuteMic->setText("Mic");
    connect(btnMuteMic, &QPushButton::toggled, this, [this, icMicOn, icMicOff](bool on) {
        micMuted = on;
        if (!icMicOn.isNull())
            btnMuteMic->setIcon(on ? icMicOff : icMicOn);
        if (voiceChat)
            voiceChat->setMicMuted(on);
    });

    btnDeafen = makeIconBtn(true);
    btnDeafen->setToolTip("Deafen");
    if (!icSound.isNull())
        btnDeafen->setIcon(icSound);
    else
        btnDeafen->setText("Sound");
    connect(btnDeafen, &QPushButton::toggled, this, [this, icSound, icNoSound](bool on) {
        audioMuted = on;
        if (!icSound.isNull())
            btnDeafen->setIcon(on ? icNoSound : icSound);
        if (voiceChat)
            voiceChat->setAudioMuted(on);
    });

    btnLeaveCall = makeIconBtn(false);
    btnLeaveCall->setToolTip("Leave room");
    if (!icLeave.isNull())
        btnLeaveCall->setIcon(icLeave);
    else
        btnLeaveCall->setText("Leave");
    connect(btnLeaveCall, &QPushButton::clicked, this, &TeamsPanel::onLeaveCallClicked);

    auto *callControls = new QHBoxLayout;
    callControls->setContentsMargins(0, 0, 0, 0);
    callControls->addStretch(1);
    callControls->addWidget(btnMuteMic);
    callControls->addStretch(1);
    callControls->addWidget(btnDeafen);
    callControls->addStretch(1);
    callControls->addWidget(btnLeaveCall);
    callControls->addStretch(1);
    cl->addLayout(callControls);

    return pane;
}

QWidget *TeamsPanel::makeTeamsPage()
{
    auto *page = new QWidget;
    auto *vl = new QVBoxLayout(page);
    vl->setContentsMargins(10, 10, 10, 10);
    vl->setSpacing(8);

    auto *header = new QHBoxLayout;
    header->addWidget(makeSectionLabel("MY TEAMS"));
    header->addStretch(1);

    btnNewTeam = new QPushButton("+");
    btnNewTeam->setObjectName("voipBtn");
    btnNewTeam->setFixedSize(26, 26);
    btnNewTeam->setToolTip("Create a new team");
    btnNewTeam->setEnabled(false);
    connect(btnNewTeam, &QPushButton::clicked, this, &TeamsPanel::onNewTeamClicked);
    header->addWidget(btnNewTeam);
    vl->addLayout(header);

    teamsList = new QListWidget;
    teamsList->setObjectName("teamList");
    teamsList->setUniformItemSizes(true);
    connect(teamsList, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        if (!item)
            return;
        openTeamDetail(item->data(Qt::UserRole).toString(), item->data(Qt::UserRole + 1).toString());
    });
    vl->addWidget(teamsList, 1);

    teamsEmptyHint = new QLabel("Sign in to see your teams.");
    teamsEmptyHint->setObjectName("stubLabel");
    teamsEmptyHint->setWordWrap(true);
    teamsEmptyHint->setAlignment(Qt::AlignCenter);
    vl->addWidget(teamsEmptyHint);

    return page;
}

QWidget *TeamsPanel::makeDetailPage()
{
    auto *page = new QWidget;
    auto *vl = new QVBoxLayout(page);
    vl->setContentsMargins(10, 10, 10, 10);
    vl->setSpacing(6);

    auto *btnBack = new QPushButton(u8"← All teams");
    btnBack->setFlat(true);
    btnBack->setCursor(Qt::PointingHandCursor);
    btnBack->setStyleSheet("QPushButton { color: #7fb3d3; border: none; background: transparent; "
                           "text-align: left; padding: 2px 0; font-size: 12px; } "
                           "QPushButton:hover { color: #9ecbe8; text-decoration: underline; }");
    connect(btnBack, &QPushButton::clicked, this, &TeamsPanel::backToTeamsList);
    vl->addWidget(btnBack);

    detailsTitle = new QLabel;
    detailsTitle->setObjectName("panelTitle");
    detailsTitle->setWordWrap(true);
    vl->addWidget(detailsTitle);

    detailsDesc = new QLabel;
    detailsDesc->setObjectName("stubLabel");
    detailsDesc->setWordWrap(true);
    vl->addWidget(detailsDesc);

    auto *splitter = new QSplitter(Qt::Vertical);
    splitter->setChildrenCollapsible(false);
    splitter->setStyleSheet("QSplitter::handle { background: #3c3c3c; } "
                            "QSplitter::handle:hover { background: #007acc; }");

    auto *membersSection = new QWidget;
    auto *msl = new QVBoxLayout(membersSection);
    msl->setContentsMargins(0, 8, 0, 0);
    msl->setSpacing(6);
    membersHeader = makeSectionLabel("MEMBERS");
    msl->addWidget(membersHeader);
    membersList = new QListWidget;
    membersList->setObjectName("teamList");
    membersList->setUniformItemSizes(true);
    msl->addWidget(membersList, 1);
    splitter->addWidget(membersSection);

    auto *roomsSection = new QWidget;
    auto *rsl = new QVBoxLayout(roomsSection);
    rsl->setContentsMargins(0, 8, 0, 0);
    rsl->setSpacing(6);
    auto *roomsHeader = new QHBoxLayout;
    roomsHeader->addWidget(makeSectionLabel("VOICE ROOMS"));
    roomsHeader->addStretch(1);
    btnNewRoom = new QPushButton("+");
    btnNewRoom->setObjectName("voipBtn");
    btnNewRoom->setFixedSize(24, 24);
    btnNewRoom->setToolTip("Create a new voice room");
    btnNewRoom->setVisible(false);
    connect(btnNewRoom, &QPushButton::clicked, this, &TeamsPanel::onNewRoomClicked);
    roomsHeader->addWidget(btnNewRoom);
    rsl->addLayout(roomsHeader);

    roomsList = new QListWidget;
    roomsList->setObjectName("teamList");
    roomsList->setUniformItemSizes(true);
    roomsList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(roomsList,
            &QListWidget::customContextMenuRequested,
            this,
            &TeamsPanel::onRoomsContextMenu);
    rsl->addWidget(roomsList, 1);

    roomsEmptyHint = new QLabel("No voice rooms yet.");
    roomsEmptyHint->setObjectName("stubLabel");
    roomsEmptyHint->setAlignment(Qt::AlignCenter);
    roomsEmptyHint->setVisible(false);
    rsl->addWidget(roomsEmptyHint);

    splitter->addWidget(roomsSection);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);

    vl->addWidget(splitter, 1);

    return page;
}

void TeamsPanel::setAuthManager(AuthManager *authManager)
{
    auth = authManager;
    if (!auth)
        return;

    teamManager = new TeamManager(auth, this);

    connect(teamManager, &TeamManager::myTeamsReceived, this, &TeamsPanel::populateTeams);
    connect(teamManager, &TeamManager::myTeamsFailed, this, [this](const QString &err) {
        emit logMessage("[Team] Failed to load teams: " + err);
    });

    connect(teamManager, &TeamManager::teamCreated, this, [this](const TeamManager::TeamInfo &team) {
        emit logMessage("[Team] Created team: " + team.name);
        refresh();
    });
    connect(teamManager, &TeamManager::teamCreateFailed, this, [this](const QString &err) {
        emit logMessage("[Team] Failed to create team: " + err);
    });

    connect(teamManager, &TeamManager::teamDetailsReceived, this, &TeamsPanel::populateDetails);
    connect(teamManager,
            &TeamManager::teamDetailsFailed,
            this,
            [this](const QString &, const QString &err) {
                emit logMessage("[Team] Failed to load team details: " + err);
            });

    connect(teamManager,
            &TeamManager::teamRoomsReceived,
            this,
            [this](const QString &teamId, const QList<TeamManager::RoomInfo> &rooms) {
                if (teamId == selectedTeamId)
                    populateRooms(rooms);
            });
    connect(teamManager,
            &TeamManager::teamRoomsFailed,
            this,
            [this](const QString &, const QString &err) {
                emit logMessage("[Team] Failed to load voice rooms: " + err);
            });

    connect(teamManager,
            &TeamManager::roomCreated,
            this,
            [this](const QString &teamId, const TeamManager::RoomInfo &) {
                emit logMessage("[Team] Voice room created");
                if (teamId == selectedTeamId)
                    teamManager->fetchTeamRooms(teamId);
            });
    connect(teamManager, &TeamManager::roomCreateFailed, this, [this](const QString &err) {
        emit logMessage("[Team] Failed to create voice room: " + err);
    });

    connect(teamManager,
            &TeamManager::roomDeleted,
            this,
            [this](const QString &teamId, const QString &) {
                emit logMessage("[Team] Voice room deleted");
                if (teamId == selectedTeamId)
                    teamManager->fetchTeamRooms(teamId);
            });
    connect(teamManager, &TeamManager::roomDeleteFailed, this, [this](const QString &err) {
        emit logMessage("[Team] Failed to delete voice room: " + err);
    });
}

void TeamsPanel::setVoiceChat(VoiceChat *vc)
{
    voiceChat = vc;
    if (!voiceChat)
        return;

    connect(voiceChat, &VoiceChat::statusChanged, this, [this](const QString &status) {
        if (callPane->isVisible() && !callStatusLabel->property("live").toBool())
            callStatusLabel->setText(status);
    });

    connect(voiceChat, &VoiceChat::peersUpdated, this, &TeamsPanel::onPeersUpdated);

    connect(voiceChat, &VoiceChat::speakingChanged, this, [this](bool speaking) {
        if (voiceChat)
            setPeerSpeaking(voiceChat->id(), speaking);
    });
    connect(voiceChat, &VoiceChat::peerSpeakingChanged, this, &TeamsPanel::setPeerSpeaking);

    connect(voiceChat, &VoiceChat::disconnectedFromServer, this, [this]() {
        joiningVoiceRoom = false;
    });

    connect(voiceChat, &VoiceChat::voipKicked, this, [this]() {
        peersList->clear();
        currentVoiceRoomKey.clear();
        setLive(false);
        showCallPane(false);
        returnToVoiceRoomTeam();
        emit logMessage("[Voice] You were kicked from the room");
    });

    connect(voiceChat, &VoiceChat::registrationDenied, this, [this](const QString &reason) {
        peersList->clear();
        currentVoiceRoomKey.clear();
        joiningVoiceRoom = false;
        setLive(false);
        showCallPane(false);
        returnToVoiceRoomTeam();
        emit logMessage("[Voice] " + reason);
    });
}

void TeamsPanel::refresh()
{
    if (!auth || !auth->isLoggedIn()) {
        clear();
        return;
    }

    btnNewTeam->setEnabled(true);
    teamManager->fetchMyTeams();

    if (pages->currentIndex() == 1 && !selectedTeamId.isEmpty())
        teamManager->fetchTeamDetails(selectedTeamId);
}

void TeamsPanel::clear()
{
    teamsList->clear();
    selectedTeamId.clear();
    selectedTeamIsAdmin = false;
    btnNewTeam->setEnabled(false);
    pages->setCurrentIndex(0);
    teamsEmptyHint->setText(auth && auth->isLoggedIn() ? "You have no teams yet — create one above."
                                                       : "Sign in to see your teams.");
    teamsEmptyHint->setVisible(true);
    teamsList->setVisible(false);
}

void TeamsPanel::showCallPane(bool on)
{
    callPane->setVisible(on);
    pages->setVisible(!on);
}

void TeamsPanel::returnToVoiceRoomTeam()
{
    if (!currentVoiceRoomTeamId.isEmpty())
        openTeamDetail(currentVoiceRoomTeamId, QString());
}

void TeamsPanel::setLive(bool on)
{
    callStatusLabel->setProperty("live", on);
    callStatusLabel->style()->unpolish(callStatusLabel);
    callStatusLabel->style()->polish(callStatusLabel);
}

void TeamsPanel::openTeamDetail(const QString &teamId, const QString &teamName)
{
    if (!teamManager)
        return;

    selectedTeamId = teamId;

    detailsTitle->setText(teamName);
    detailsDesc->clear();
    membersHeader->setText("MEMBERS");
    membersList->clear();
    roomsList->clear();
    roomsEmptyHint->setVisible(false);
    btnNewRoom->setVisible(false);

    pages->setCurrentIndex(1);

    teamManager->fetchTeamDetails(selectedTeamId);
    teamManager->fetchTeamRooms(selectedTeamId);
}

void TeamsPanel::backToTeamsList()
{
    pages->setCurrentIndex(0);
}

QWidget *TeamsPanel::makeTeamRow(const TeamManager::TeamInfo &team)
{
    auto *row = new QWidget;
    row->setStyleSheet("background: transparent;");
    auto *h = new QHBoxLayout(row);
    h->setContentsMargins(8, 8, 8, 8);
    h->setSpacing(10);

    h->addWidget(makeAvatar(Avatar::initialFor(team.name), Avatar::colorForId(team.id), 36));

    auto *textCol = new QVBoxLayout;
    textCol->setSpacing(1);
    auto *nameLbl = new QLabel(team.name);
    nameLbl->setStyleSheet("color: #e8e8e8; font-weight: 600; font-size: 13px;");
    auto *subLbl = new QLabel(
        team.membersCount == 1 ? "1 member" : QString("%1 members").arg(team.membersCount));
    subLbl->setStyleSheet("color: #8a8a8a; font-size: 11px;");
    textCol->addWidget(nameLbl);
    textCol->addWidget(subLbl);
    h->addLayout(textCol, 1);

    auto *chevron = new QLabel(u8"›");
    chevron->setStyleSheet("color: #6a6a6a; font-size: 16px;");
    h->addWidget(chevron);

    return row;
}

QWidget *TeamsPanel::makeMemberRow(const TeamManager::MemberInfo &member)
{
    auto *row = new QWidget;
    row->setStyleSheet("background: transparent;");
    auto *h = new QHBoxLayout(row);
    h->setContentsMargins(8, 6, 8, 6);
    h->setSpacing(10);

    h->addWidget(makeAvatar(Avatar::initialFor(member.user.username),
                            Avatar::colorForId(member.id),
                            28,
                            member.user.avatarUrl));

    auto *nameLbl = new QLabel(member.user.username);
    nameLbl->setStyleSheet("color: #d4d4d4; font-size: 12px;");
    h->addWidget(nameLbl, 1);

    h->addWidget(makePill(member.hasRole ? member.role.name : "Member",
                          member.hasRole && member.role.isAdmin));

    return row;
}

QWidget *TeamsPanel::makeRoomRow(const TeamManager::RoomInfo &room)
{
    auto *row = new QWidget;
    row->setStyleSheet("background: transparent;");
    auto *h = new QHBoxLayout(row);
    h->setContentsMargins(8, 6, 8, 6);
    h->setSpacing(8);

    auto *icon = new QLabel(u8"♪");
    icon->setStyleSheet("color: #6a6a6a; font-size: 13px;");
    icon->setAlignment(Qt::AlignCenter);
    icon->setFixedWidth(20);
    h->addWidget(icon);

    auto *textCol = new QVBoxLayout;
    textCol->setSpacing(1);
    auto *nameLbl = new QLabel(room.name);
    nameLbl->setStyleSheet("color: #e8e8e8; font-weight: 600; font-size: 12px;");
    const QString limit = room.maxParticipants > 0 ? QString("max %1").arg(room.maxParticipants)
                                                   : QString("no limit");
    auto *subLbl = new QLabel(limit);
    subLbl->setStyleSheet("color: #8a8a8a; font-size: 10px;");
    textCol->addWidget(nameLbl);
    textCol->addWidget(subLbl);
    h->addLayout(textCol, 1);

    auto *btnJoin = new QPushButton("Join");
    btnJoin->setObjectName("voipBtn");
    btnJoin->setFixedWidth(52);
    const QString roomKey = room.roomKey;
    const QString roomName = room.name;
    const QString teamId = selectedTeamId;
    connect(btnJoin, &QPushButton::clicked, this, [this, roomKey, roomName, teamId]() {
        joinVoiceRoom(roomKey, roomName, teamId);
    });
    h->addWidget(btnJoin);

    return row;
}

void TeamsPanel::populateTeams(const QList<TeamManager::TeamInfo> &teams)
{
    teamsList->clear();
    for (const auto &team : teams) {
        auto *item = new QListWidgetItem;
        item->setSizeHint(QSize(0, 58));
        item->setData(Qt::UserRole, team.id);
        item->setData(Qt::UserRole + 1, team.name);
        teamsList->addItem(item);
        teamsList->setItemWidget(item, makeTeamRow(team));
    }

    const bool empty = teams.isEmpty();
    teamsList->setVisible(!empty);
    teamsEmptyHint->setVisible(empty);
    if (empty)
        teamsEmptyHint->setText("You have no teams yet — create one above.");
}

void TeamsPanel::populateDetails(const TeamManager::TeamDetails &details)
{
    if (details.team.id != selectedTeamId)
        return;

    detailsTitle->setText(details.team.name);
    detailsDesc->setText(details.team.description.isEmpty() ? "No description"
                                                            : details.team.description);

    const int myId = auth ? auth->currentUser().id : 0;
    selectedTeamIsAdmin = (details.team.createdBy.id == myId);

    membersHeader->setText(QString("MEMBERS (%1)").arg(details.members.size()));

    membersList->clear();
    for (const auto &member : details.members) {
        auto *item = new QListWidgetItem;
        item->setSizeHint(QSize(0, 44));
        membersList->addItem(item);
        membersList->setItemWidget(item, makeMemberRow(member));

        if (member.user.id == myId && member.hasRole && member.role.isAdmin)
            selectedTeamIsAdmin = true;
    }

    btnNewRoom->setVisible(selectedTeamIsAdmin);
}

void TeamsPanel::populateRooms(const QList<TeamManager::RoomInfo> &rooms)
{
    roomsList->clear();
    for (const auto &room : rooms) {
        auto *item = new QListWidgetItem;
        item->setSizeHint(QSize(0, 50));
        item->setData(Qt::UserRole, room.roomKey);
        item->setData(Qt::UserRole + 1, room.name);
        item->setData(Qt::UserRole + 2, room.id);
        roomsList->addItem(item);
        roomsList->setItemWidget(item, makeRoomRow(room));
    }

    const bool empty = rooms.isEmpty();
    roomsList->setVisible(!empty);
    roomsEmptyHint->setVisible(empty);
}

void TeamsPanel::onNewTeamClicked()
{
    if (!teamManager)
        return;

    bool ok = false;
    const QString name
        = QInputDialog::getText(this, "New Team", "Team name:", QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty())
        return;

    const QString desc = QInputDialog::getText(this,
                                               "New Team",
                                               "Description (optional):",
                                               QLineEdit::Normal,
                                               QString(),
                                               &ok);

    teamManager->createTeam(name.trimmed(), ok ? desc.trimmed() : QString());
}

void TeamsPanel::onNewRoomClicked()
{
    if (!teamManager || selectedTeamId.isEmpty())
        return;

    bool ok = false;
    const QString name = QInputDialog::getText(this,
                                               "New Voice Room",
                                               "Room name:",
                                               QLineEdit::Normal,
                                               QString(),
                                               &ok);
    if (!ok || name.trimmed().isEmpty())
        return;

    const int maxParticipants = QInputDialog::getInt(this,
                                                     "New Voice Room",
                                                     "Max participants (0 = no limit):",
                                                     0,
                                                     0,
                                                     100,
                                                     1,
                                                     &ok);
    if (!ok)
        return;

    teamManager->createTeamRoom(selectedTeamId, name.trimmed(), maxParticipants);
}

void TeamsPanel::onRoomsContextMenu(const QPoint &pos)
{
    auto *item = roomsList->itemAt(pos);
    if (!item || !selectedTeamIsAdmin)
        return;

    const QString roomId = item->data(Qt::UserRole + 2).toString();

    QMenu menu(roomsList);
    menu.addAction("Delete room",
                   [this, roomId]() { teamManager->deleteTeamRoom(selectedTeamId, roomId); });
    menu.exec(roomsList->viewport()->mapToGlobal(pos));
}

void TeamsPanel::joinVoiceRoom(const QString &roomKey, const QString &label, const QString &teamId)
{
    if (!voiceChat || !auth || joiningVoiceRoom)
        return;

    if (currentVoiceRoomKey == roomKey && voiceChat->isConnected())
        return;

    joiningVoiceRoom = true;
    currentVoiceRoomKey = roomKey;
    currentVoiceRoomLabel = label;
    currentVoiceRoomTeamId = teamId;

    voiceChat->setAuthToken(auth->token());
    voiceChat->setTeamId(teamId);
    voiceChat->setUsername(auth->currentUser().username);
    voiceChat->setAvatarUrl(auth->currentUser().avatarUrl);

    voiceChat->setRoom(roomKey);
    emit logMessage("[Voice] Joining room: " + label);

    showCallPane(true);
    setLive(false);
    peersList->clear();
    callStatusLabel->setText("Connecting to " + label + u8"…");

    if (voiceChat->isConnected())
        voiceChat->disconnectFromServer();

    QTimer::singleShot(300, this, [this]() {
        voiceChat->connectToServer(AppConfig::voiceServerHost(), AppConfig::voiceServerPort());
    });
}

void TeamsPanel::onLeaveCallClicked()
{
    if (!voiceChat)
        return;

    voiceChat->disconnectFromServer();
    speakingFrames.clear();
    peersList->clear();
    currentVoiceRoomKey.clear();
    setLive(false);
    showCallPane(false);
    returnToVoiceRoomTeam();
    emit logMessage("[Voice] Left room");
}

QWidget *TeamsPanel::makePeerRow(const QString &name,
                                 const QString &avatarUrl,
                                 int peerId,
                                 bool isMe)
{
    auto *row = new QWidget;
    row->setStyleSheet("background: transparent;");
    auto *h = new QHBoxLayout(row);
    h->setContentsMargins(8, 6, 8, 6);
    h->setSpacing(10);

    auto *ring = new AvatarRing(34, 3, row);
    Avatar::load(ring,
                 avatarUrl,
                 Avatar::letterPixmap(Avatar::initialFor(name),
                                      Avatar::colorForId(QString::number(peerId)),
                                      28),
                 28,
                 [ring](QPixmap pix) { ring->setPixmap(pix); });
    speakingFrames[peerId] = ring;
    h->addWidget(ring);

    auto *nameLbl = new QLabel(isMe ? name + " (You)" : name);
    nameLbl->setObjectName("peerName");
    nameLbl->setStyleSheet("color: #d4d4d4; font-size: 12px;");
    h->addWidget(nameLbl, 1);

    return row;
}

void TeamsPanel::setPeerSpeaking(int peerId, bool speaking)
{
    if (auto *w = speakingFrames.value(peerId, nullptr))
        static_cast<AvatarRing *>(w)->setRingColor(speaking ? QColor("#43b581")
                                                            : QColor(Qt::transparent));
}

void TeamsPanel::onPeersUpdated(const QStringList &ids)
{
    if (!voiceChat)
        return;

    if (joiningVoiceRoom) {
        joiningVoiceRoom = false;
        if (!voiceChat->isCallActive())
            voiceChat->startCall();
    }

    speakingFrames.clear();
    peersList->clear();

    const QString myName = auth ? auth->currentUser().username : "You";
    const QString myAvatar = auth ? auth->currentUser().avatarUrl : QString();
    const int myId = voiceChat->id();

    auto *meItem = new QListWidgetItem;
    meItem->setSizeHint(QSize(0, 44));
    meItem->setData(Qt::UserRole, myId);
    peersList->addItem(meItem);
    peersList->setItemWidget(meItem, makePeerRow(myName, myAvatar, myId, true));

    for (const QString &uid : ids) {
        const int peerId = uid.toInt();
        const QString pName = voiceChat->peerName(peerId);
        const QString name = voipNicknames.value(peerId, pName.isEmpty() ? uid : pName);
        const QString avatarUrl = voiceChat->peerAvatarUrl(peerId);

        auto *item = new QListWidgetItem;
        item->setSizeHint(QSize(0, 44));
        item->setData(Qt::UserRole, peerId);
        peersList->addItem(item);
        peersList->setItemWidget(item, makePeerRow(name, avatarUrl, peerId, false));
    }

    setLive(true);
    callStatusLabel->setText("Connected — " + currentVoiceRoomLabel);
}

void TeamsPanel::onPeersContextMenu(const QPoint &pos)
{
    if (!voiceChat)
        return;

    QListWidgetItem *item = peersList->itemAt(pos);
    if (!item)
        return;

    const int peerId = item->data(Qt::UserRole).toInt();
    if (peerId == voiceChat->id())
        return;

    QMenu menu(peersList);

    const bool muted = voiceChat->isPeerMuted(peerId);
    menu.addAction(muted ? "Unmute" : "Mute for me", [this, peerId, muted]() {
        voiceChat->setPeerMuted(peerId, !muted);
        callStatusLabel->setText(muted ? "Unmuted peer" : "Muted peer locally");
    });

    menu.addSeparator();

    auto *volWidget = new QWidget;
    auto *volLayout = new QHBoxLayout(volWidget);
    volLayout->setContentsMargins(8, 4, 8, 4);
    volLayout->addWidget(new QLabel("Volume:"));
    auto *slider = new QSlider(Qt::Horizontal);
    slider->setRange(0, 200);
    slider->setValue(qRound(voiceChat->peerVolume(peerId) * 100));
    slider->setFixedWidth(120);
    connect(slider, &QSlider::valueChanged, this, [this, peerId](int val) {
        voiceChat->setPeerVolume(peerId, val / 100.0f);
    });
    volLayout->addWidget(slider);
    auto *volAction = new QWidgetAction(&menu);
    volAction->setDefaultWidget(volWidget);
    menu.addAction(volAction);

    menu.addSeparator();

    const QString defaultName = voiceChat->peerName(peerId).isEmpty() ? QString::number(peerId)
                                                                      : voiceChat->peerName(peerId);
    const QString currentNick = voipNicknames.value(peerId, defaultName);
    menu.addAction("Set nickname", [this, peerId, currentNick, defaultName, item]() {
        bool ok;
        const QString nick = QInputDialog::getText(this,
                                                   "Set Nickname",
                                                   "Nickname:",
                                                   QLineEdit::Normal,
                                                   currentNick,
                                                   &ok);
        if (!ok)
            return;
        if (nick.isEmpty())
            voipNicknames.remove(peerId);
        else
            voipNicknames[peerId] = nick;
        if (auto *w = peersList->itemWidget(item))
            if (auto *lbl = w->findChild<QLabel *>("peerName"))
                lbl->setText(voipNicknames.value(peerId, defaultName));
    });

    menu.addAction("Copy ID",
                   [peerId]() { QApplication::clipboard()->setText(QString::number(peerId)); });

    if (voiceChat->isHost()) {
        menu.addSeparator();
        menu.addAction("Kick from room", [this, peerId]() { voiceChat->kickPeer(peerId); });
    }

    menu.exec(peersList->viewport()->mapToGlobal(pos));
}
