#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QAction>
#include <QComboBox>
#include <QDockWidget>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QMap>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QToolButton>
#include <QTreeWidget>

#include "auth/authmanager.h"
#include "collab/collabsession.h"
#include "collab/sessionreport.h"
#include "collab/sessionreportdialog.h"
#include "db/projectdb.h"
#include "debug/debugadapter.h"
#include "editor/codeeditor.h"
#include "filebrowser/filebrowser.h"
#include "git/gitpanel.h"
#include "settings/settingsmanager.h"
#include "team/teamspanel.h"
#include "terminal/terminal.h"
#include "voicechat/voicechat.h"

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    //Activity Bar
    QWidget *activityBar;
    QToolButton *btnFiles;
    QToolButton *btnCollab;
    QToolButton *btnTeam;
    //Left Sidebar
    QWidget *leftPanel;
    QLabel *leftTitle;
    QStackedWidget *leftStack;
    FileBrowser *fileBrowser;
    TeamsPanel *teamsPanel;

    QSplitter *centralSplitter;

    //Editor Area
    QTabWidget *editorTabs;
    CodeEditor *editor;

    //Bottom Dock
    QDockWidget *bottomDock;
    QTabWidget *bottomTabs;
    QPlainTextEdit *outputPane;
    Terminal *terminal;
    QWidget *gitPane;
    GitPanel *gitPanel_ = nullptr;
    QWidget *debugPane;

    //Status Bar
    QLabel *statusFile;
    QLabel *statusPosition;
    QLabel *statusEncoding;
    QLabel *statusLanguage;

    //State
    int activeSidePanel;
    QString currentFilePath;

    // Collaboration
    CollabSession *session = nullptr;
    QListWidget *collabUsersList = nullptr;
    QLabel *collabStatusLabel = nullptr;
    QPushButton *btnStopAllCollab = nullptr;
    QPushButton *btnStartCollab = nullptr;
    QPushButton *btnJoinCollab = nullptr;
    QPushButton *btnSessionReport = nullptr;
    QWidget *collabNoSessionPane = nullptr;
    QWidget *collabInSessionPane = nullptr;
    QMap<int, QString> peerFiles;
    QMap<int, QString> peerNames;
    QMap<int, QString> peerAvatars;
    QString currentCollabFile;

    // Session report
    SessionReportDialog *reportDialog = nullptr;
    bool pendingEndCollab = false;

    // Run
    QProcess *runProcess = nullptr;
    QComboBox *runFileCombo = nullptr;
    QAction *actStop = nullptr;
    int inputStartPos = 0;

    // Debug
    DebugAdapter *debugAdapter = nullptr;
    QTreeWidget *debugCallStack = nullptr;
    QTreeWidget *debugVariables = nullptr;
    QAction *actDebugContinue = nullptr;
    QAction *actDebugStepOver = nullptr;
    QAction *actDebugStepIn = nullptr;
    QAction *actDebugStepOut = nullptr;
    QAction *actDebugStop = nullptr;
    QAction *actDebugMain = nullptr;

    // Project state
    int currentProjectId = -1;
    QString currentProjectPath;

    // Auth
    AuthManager *auth = nullptr;
    QToolButton *btnProfile = nullptr;
    void setupAuthManager();
    void updateProfileButton();
    void openProfileDialog();
    void updateCollabAccess();

    void openSettings();
    void applySettings();
    void refreshRecentMenu(QMenu *menu);
    void openProjectFolder(const QString &path);
    void saveSessionToDb();

    // Voice
    VoiceChat *voiceChat = nullptr;

    //Setup UI
    void setupMenuBar();
    void setupMainToolBar();
    void setupCentralWidget();
    void setupActivityBar();
    void setupLeftPanel();
    void setupEditorArea();
    void setupBottomDock();
    void setupDebugPanel();
    void setupStatusBar();
    void applyTheme();

    void setSidePanelPage(int index);
    void updateWindowTitle();

    void clearTabs();
    CodeEditor *createTab(const QString &name);

    // Session helpers
    void stopAllCollab();
    void markTabAsCollab(CodeEditor *ed, bool on);
    void wireEditorToManager(CodeEditor *ed, RGAManager *mgr);
    void wireEditorToSession(CodeEditor *ed, const QString &relPath);

    QString toSessionKey(const QString &editorPath) const;
    QMap<QString, QString> collectCurrentFileTexts() const;

    void onCollabUsersUpdated(QMap<int, QString> users);
    void onRemoteFileFocusChanged(int siteId, const QString &file);
    void refreshCollabUsersList();
    QWidget *makeCollabUserRow(int id, const QString &label, const QString &avatarUrl);

private slots:
    void onActivityButton(int page);
    void onCursorPositionUpdated(int line, int index);
    void onModificationChanged(bool modified);
    void onTabCloseRequested(int tabIndex);
    void onTabChanged(int index);
    void openFileFromBrowser(const QString &path);

    void newFile();
    void openFile();
    void openFolder();
    void cloneRepo();
    void runFile();
    void stopRun();
    void startDebugging();
    void stopDebugging();
    void updateRunCombo();
    bool saveFile();
    bool saveFileAs();

    void toggleSidePanel();
    void toggleBottomDock();

    void startCollab(const QString &room,
                     CollabSession::Mode mode = CollabSession::Mode::ReadWrite,
                     const QStringList &selectedFiles = {});
    void joinCollab();
    bool showStartCollabDialog();

    void onSessionProjectInit(int hostSiteId, const QStringList &files);
    void onSessionRunOutput(const QString &text);
    void onSessionFileCreated(const QString &relPath);
    void onSessionFileDeleted(const QString &relPath);
    void onSessionFileRenamed(const QString &oldPath, const QString &newPath);

    void onCollabUserContextMenu(const QPoint &pos);
    void onSessionReportReady(const SessionReportData &report);
    void onSessionAiInsightsReady(const AiInsights &ai);
    void onEndCollabRequested();

    void openDiffTab(const QString &relPath, bool staged);
    void onDebugStopped(const QString &filePath, int line, const QString &reason);
    void onDebugContinued();
    void onDebugTerminated();
    void onDebugVariablesReady(const QList<DebugAdapter::Var> &vars);
    void onDebugSubVariablesReady(int parentRef, const QList<DebugAdapter::Var> &vars);
    void onDebugVariableExpanded(QTreeWidgetItem *item);
    void onDebugCallStackReady(const QList<DebugAdapter::FrameInfo> &frames);
    void onDebugCallStackClicked(QTreeWidgetItem *item, int column);
    void clearDebugHighlights();
};

#endif // MAINWINDOW_H
