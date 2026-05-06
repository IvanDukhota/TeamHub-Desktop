#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QAction>
#include <QDockWidget>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QToolButton>
#include <QTreeWidget>

#include "editor/codeeditor.h"
#include "filebrowser/filebrowser.h"
#include "terminal/terminal.h"
#include "rga/rgamanager.h"

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    //Activity Bar
    QWidget     *activityBar;
    QToolButton *btnFiles;
    QToolButton *btnTasks;
    QToolButton *btnTeam;
    QToolButton *btnVoip;
    //Left Sidebar
    QWidget        *leftPanel;
    QLabel         *leftTitle;
    QStackedWidget *leftStack;
    FileBrowser *fileBrowser;
    QListWidget    *taskList;
    QListWidget    *teamList;

    QSplitter  *centralSplitter;

    //Editor Area
    QTabWidget *editorTabs;
    CodeEditor *editor;

    //Bottom Dock
    QDockWidget    *bottomDock;
    QTabWidget     *bottomTabs;
    QPlainTextEdit *outputPane;
    QPlainTextEdit *terminalPane;
    Terminal *terminal;
    QWidget   *gitPane;
    QDockWidget *voipDock;

    //Status Bar
    QLabel *statusFile;
    QLabel *statusPosition;
    QLabel *statusEncoding;
    QLabel *statusLanguage;

    //State
    int     activeSidePanel;
    QString currentFilePath;

    // RGA Colab
    RGAManager* rgamanager;
    bool collabActive = false;

    //Setup UI
    void setupMenuBar();
    void setupMainToolBar();
    void setupCentralWidget();
    void setupActivityBar();
    void setupLeftPanel();
    void setupEditorArea();
    void setupBottomDock();
    void setupVoipDock();
    void setupStatusBar();
    void applyTheme();

    void setSidePanelPage(int index);
    void updateWindowTitle();

    void clearTabs();

    CodeEditor* currentEditor();

private slots:
    void onActivityButton(int page);
    void onCursorPositionUpdated(int line, int index);
    void onModificationChanged(bool modified);
    void onTabCloseRequested(int tabIndex);
    void onTabChanged(int index);
    void openFileFromBrowser(const QString& path);

    void newFile();
    void openFile();
    void openFolder();
    void runFile();
    bool saveFile();
    bool saveFileAs();

    void toggleSidePanel();
    void toggleBottomDock();
    void toggleVoipDock();

    void onCollabTextChanged(const QString& newText);
    void startCollab();
    void stopCollab();
};

#endif // MAINWINDOW_H
