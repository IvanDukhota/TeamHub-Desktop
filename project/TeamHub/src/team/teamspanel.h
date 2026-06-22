#ifndef TEAMSPANEL_H
#define TEAMSPANEL_H

#include <QLabel>
#include <QListWidget>
#include <QMap>
#include <QPushButton>
#include <QStackedWidget>
#include <QWidget>

#include "teammanager.h"

class AuthManager;
class QFrame;
class VoiceChat;

class TeamsPanel : public QWidget
{
    Q_OBJECT
public:
    explicit TeamsPanel(QWidget *parent = nullptr);

    void setAuthManager(AuthManager *authManager);
    void setVoiceChat(VoiceChat *voiceChat);
    void refresh();
    void clear();

signals:
    void logMessage(const QString &msg);

private:
    AuthManager *auth = nullptr;
    TeamManager *teamManager = nullptr;
    VoiceChat *voiceChat = nullptr;

    QWidget *callPane;
    QLabel *callStatusLabel;
    QListWidget *peersList;
    QPushButton *btnMuteMic;
    QPushButton *btnDeafen;
    QPushButton *btnLeaveCall;

    QStackedWidget *pages;

    QListWidget *teamsList;
    QLabel *teamsEmptyHint;

    QLabel *detailsTitle;
    QLabel *detailsDesc;
    QLabel *membersHeader;
    QListWidget *membersList;
    QPushButton *btnNewRoom;
    QListWidget *roomsList;
    QWidget *roomsEmptyWidget;
    QPushButton *btnCreateRoomEmpty;

    QString selectedTeamId;
    bool selectedTeamIsAdmin = false;

    QString currentVoiceRoomKey;
    QString currentVoiceRoomLabel;
    QString currentVoiceRoomTeamId;
    bool joiningVoiceRoom = false;
    bool micMuted = false;
    bool audioMuted = false;
    QMap<int, QString> voipNicknames;
    QMap<int, QWidget *> speakingFrames;

    void setupUi();
    QWidget *makeCallPane();
    QWidget *makeTeamsPage();
    QWidget *makeDetailPage();

    QWidget *makeTeamRow(const TeamManager::TeamInfo &team);
    QWidget *makeMemberRow(const TeamManager::MemberInfo &member);
    QWidget *makeRoomRow(const TeamManager::RoomInfo &room);
    QWidget *makePeerRow(const QString &name, const QString &avatarUrl, int peerId, bool isMe);
    void setPeerSpeaking(int peerId, bool speaking);

    void openTeamDetail(const QString &teamId, const QString &teamName);
    void backToTeamsList();
    void setLive(bool on);
    void showCallPane(bool on);
    void returnToVoiceRoomTeam();

    void onNewRoomClicked();
    void onRoomsContextMenu(const QPoint &pos);
    void onPeersContextMenu(const QPoint &pos);
    void onPeersUpdated(const QStringList &ids);
    void onLeaveCallClicked();

    void populateTeams(const QList<TeamManager::TeamInfo> &teams);
    void populateDetails(const TeamManager::TeamDetails &details);
    void populateRooms(const QList<TeamManager::RoomInfo> &rooms);

    void joinVoiceRoom(const QString &roomKey, const QString &label, const QString &teamId);
};

#endif // TEAMSPANEL_H
