#include "mainwindow.h"

#include <QApplication>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QMenuBar>
#include <QMessageBox>
#include <QSizePolicy>
#include <QStatusBar>
#include <QToolBar>
#include <QVBoxLayout>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , activeSidePanel(0)
{
    setWindowTitle("TeamHub");
    resize(1400, 900);
    setMinimumSize(800, 500);

    setupMenuBar();
    setupMainToolBar();
    setupCentralWidget();
    setupBottomDock();
    setupVoipDock();
    setupStatusBar();
    applyTheme();

    setSidePanelPage(0);
    btnFiles->setChecked(true);
    connect(editorTabs, &QTabWidget::currentChanged,
            this, &MainWindow::onTabChanged);
    connect(editor, &CodeEditor::cursorPositionUpdated,
            this, &MainWindow::onCursorPositionUpdated);
    connect(editor, &CodeEditor::modifyChanged,
            this, &MainWindow::onModificationChanged);
    connect(editor, &CodeEditor::charInserted,
            this, [this](int pos, QChar ch) {
                if (!collabActive || editor->isApplyingRemote()) return;
                rgamanager->localInsert(pos, ch);
            });

    outputPane->appendPlainText("[TeamHub] Ready.");
    updateWindowTitle();

    rgamanager = new RGAManager((rand()%50)+100 ,this);
    connect(rgamanager, &RGAManager::textChanged,
            this, &MainWindow::onCollabTextChanged);
}

MainWindow::~MainWindow() = default;

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (editor->isModified()) {
        const auto btn = QMessageBox::question(
            this, "Unsaved Changes",
            "The current file has unsaved changes.\nSave before closing?",
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
            QMessageBox::Save);

        if (btn == QMessageBox::Save) {
            if (!saveFile()) { event->ignore(); return; }
        } else if (btn == QMessageBox::Cancel) {
            event->ignore(); return;
        }
    }
    event->accept();
}


void MainWindow::setupMenuBar()
{
    QMenu *fileMenu = menuBar()->addMenu("&File");
    fileMenu->addAction("&New File",     this, &MainWindow::newFile,         QKeySequence::New);
    fileMenu->addAction("&Open Folder",  this, &MainWindow::openFolder);
    fileMenu->addAction("&Open File...", this, &MainWindow::openFile,        QKeySequence::Open);
    fileMenu->addAction("&Save",         this, [this]{ saveFile(); },        QKeySequence::Save);
    fileMenu->addAction("Save &As...",   this, [this]{ saveFileAs(); },      QKeySequence::SaveAs);
    fileMenu->addSeparator();
    fileMenu->addAction("Close &Tab",    this,
        [this]{ onTabCloseRequested(editorTabs->currentIndex()); },
        QKeySequence("Ctrl+W"));
    fileMenu->addSeparator();
    fileMenu->addAction("E&xit", qApp, &QApplication::quit,                  QKeySequence::Quit);

    QMenu *editMenu = menuBar()->addMenu("&Edit");
    editMenu->addAction("&Undo",       this, [this]{ editor->undo(); },      QKeySequence::Undo);
    editMenu->addAction("&Redo",       this, [this]{ editor->redo(); },      QKeySequence::Redo);
    editMenu->addSeparator();
    editMenu->addAction("Cu&t",        this, [this]{ editor->cut(); },       QKeySequence::Cut);
    editMenu->addAction("&Copy",       this, [this]{ editor->copy(); },      QKeySequence::Copy);
    editMenu->addAction("&Paste",      this, [this]{ editor->paste(); },     QKeySequence::Paste);
    editMenu->addAction("Select &All", this, [this]{ editor->selectAll(); }, QKeySequence::SelectAll);
    editMenu->addSeparator();
    auto *actFind = editMenu->addAction("&Find / Replace...", QKeySequence("Ctrl+H"));
    actFind->setEnabled(false);
    auto *actGoto = editMenu->addAction("&Go to Line...",     QKeySequence("Ctrl+G"));
    actGoto->setEnabled(false);

    QMenu *viewMenu = menuBar()->addMenu("&View");
    viewMenu->addAction("Toggle &Side Panel",   this, &MainWindow::toggleSidePanel,  QKeySequence("Ctrl+B"));
    viewMenu->addAction("Toggle &Output Panel", this, &MainWindow::toggleBottomDock, QKeySequence("Ctrl+J"));
    viewMenu->addAction("Toggle &VoIP Panel",   this, &MainWindow::toggleVoipDock);
    viewMenu->addSeparator();
    viewMenu->addAction("Zoom &In",     this, [this]{ editor->zoomIn(); },    QKeySequence::ZoomIn);
    viewMenu->addAction("Zoom &Out",    this, [this]{ editor->zoomOut(); },   QKeySequence::ZoomOut);
    viewMenu->addAction("Reset &Zoom",  this, [this]{ editor->resetZoom(); }, QKeySequence("Ctrl+0"));
    viewMenu->addSeparator();
    QMenu *themeMenu = viewMenu->addMenu("&Theme");
    themeMenu->addAction("Dark",  this, [this]{ editor->setTheme(CodeEditor::Theme::Dark); });
    themeMenu->addAction("Light", this, [this]{ editor->setTheme(CodeEditor::Theme::Light); });

    QMenu *gitMenu = menuBar()->addMenu("&Git");
    for (auto *a : {
            gitMenu->addAction("Init Repository"),
            gitMenu->addAction("Clone..."),
    }) a->setEnabled(false);
    gitMenu->addSeparator();
    for (auto *a : {
            gitMenu->addAction("Pull"),
            gitMenu->addAction("Push"),
            gitMenu->addAction("Commit..."),
    }) a->setEnabled(false);
    gitMenu->addSeparator();
    for (auto *a : {
            gitMenu->addAction("Branches"),
            gitMenu->addAction("Diff"),
    }) a->setEnabled(false);

    QMenu *teamMenu = menuBar()->addMenu("&Team");
    teamMenu->addAction("Connect to Server")->setEnabled(false);
    teamMenu->addSeparator();
    teamMenu->addAction("Members")->setEnabled(false);
    teamMenu->addAction("Share Session")->setEnabled(false);
    teamMenu->addAction("Voice Call", this, &MainWindow::toggleVoipDock);
}


void MainWindow::setupMainToolBar()
{
    auto *tb = addToolBar("Main");
    tb->setObjectName("mainToolBar");
    tb->setMovable(false);

    tb->addAction("New",  this, &MainWindow::newFile);
    tb->addAction("Open", this, &MainWindow::openFile);
    tb->addAction("Save", this, [this]{ saveFile(); });
    tb->addSeparator();

    auto *actRun = tb->addAction("Run", this, &MainWindow::runFile);
    actRun->setToolTip("Run (F5)");
    actRun->setShortcut(QKeySequence("F5"));

    auto *actDebug = tb->addAction("Debug");
    actDebug->setToolTip("Start Debugging (F9)  —  stub");
    actDebug->setShortcut(QKeySequence("F9"));
    actDebug->setEnabled(false);

    tb->addSeparator();

    auto *actCollab = tb->addAction("Collab");
    actCollab->setToolTip("Start collaborative editing session");
    actCollab->setCheckable(true);
    connect(actCollab, &QAction::triggered, this, [this](bool checked) {
        if (checked) {
            startCollab();
        } else {
            rgamanager->disconnectFromServer();
            stopCollab();
        }
    });

    auto *actCall = tb->addAction("Call");
    actCall->setToolTip("Toggle Voice");
    connect(actCall, &QAction::triggered, this, &MainWindow::toggleVoipDock);
}

void MainWindow::setupCentralWidget()
{
    auto *root = new QWidget(this);
    auto *hbox = new QHBoxLayout(root);
    hbox->setContentsMargins(0, 0, 0, 0);
    hbox->setSpacing(0);

    setupActivityBar();
    setupLeftPanel();
    setupEditorArea();

    centralSplitter = new QSplitter(Qt::Horizontal, root);
    centralSplitter->setObjectName("centralSplitter");
    centralSplitter->setChildrenCollapsible(false);
    centralSplitter->addWidget(leftPanel);
    centralSplitter->addWidget(editorTabs);
    centralSplitter->setStretchFactor(0, 0);
    centralSplitter->setStretchFactor(1, 1);
    centralSplitter->setSizes({260, 1140});

    hbox->addWidget(activityBar);
    hbox->addWidget(centralSplitter);

    setCentralWidget(root);
}


void MainWindow::setupActivityBar()
{
    activityBar = new QWidget;
    activityBar->setObjectName("activityBar");
    activityBar->setFixedWidth(48);

    auto *vbox = new QVBoxLayout(activityBar);
    vbox->setContentsMargins(0, 8, 0, 8);
    vbox->setSpacing(0);

    auto makeBtn = [](const QString &label, const QString &tip) {
        auto *btn = new QToolButton;
        btn->setText(label);
        btn->setToolTip(tip);
        btn->setCheckable(true);
        btn->setFixedSize(48, 48);
        btn->setObjectName("activityBtn");
        return btn;
    };

    btnFiles = makeBtn("Files", "Explorer  (Ctrl+B)");
    btnTasks = makeBtn("Tasks", "Task Manager");
    btnTeam  = makeBtn("Team",  "Team");

    vbox->addWidget(btnFiles);
    vbox->addWidget(btnTasks);
    vbox->addWidget(btnTeam);
    vbox->addStretch(1);

    btnVoip = makeBtn("VoIP", "Voice");
    btnVoip->setObjectName("activityBtnVoip");
    vbox->addWidget(btnVoip);

    connect(btnFiles, &QToolButton::clicked, this, [this]{ onActivityButton(0); });
    connect(btnTasks, &QToolButton::clicked, this, [this]{ onActivityButton(1); });
    connect(btnTeam,  &QToolButton::clicked, this, [this]{ onActivityButton(2); });
    connect(btnVoip,  &QToolButton::clicked, this, &MainWindow::toggleVoipDock);
}

void MainWindow::setupLeftPanel()
{
    leftPanel = new QWidget;
    leftPanel->setObjectName("leftPanel");
    leftPanel->setMinimumWidth(140);

    auto *vbox = new QVBoxLayout(leftPanel);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);

    leftTitle = new QLabel("EXPLORER");
    leftTitle->setObjectName("panelTitle");
    leftTitle->setContentsMargins(12, 5, 12, 5);
    vbox->addWidget(leftTitle);

    auto *sep = new QFrame;
    sep->setFrameShape(QFrame::HLine);
    sep->setObjectName("panelSeparator");
    vbox->addWidget(sep);

    leftStack = new QStackedWidget;

    fileBrowser = new FileBrowser(this);
    fileBrowser->setRootPath("C:/TeamHub-Desktop/project/TeamHub");
    leftStack->addWidget(fileBrowser);

    taskList = new QListWidget;
    taskList->setObjectName("taskList");
    taskList->addItem("Task Manager");
    leftStack->addWidget(taskList);

    teamList = new QListWidget;
    teamList->setObjectName("teamList");
    teamList->addItem("Team Panel");
    leftStack->addWidget(teamList);

    vbox->addWidget(leftStack, 1);

    connect(fileBrowser, &FileBrowser::fileDoubleClicked,
            this, &MainWindow::openFileFromBrowser);
}

void MainWindow::openFileFromBrowser(const QString& path)
{
    for (int i = 0; i < editorTabs->count(); i++) {
        CodeEditor* existingEditor = qobject_cast<CodeEditor*>(editorTabs->widget(i));
        if (existingEditor && existingEditor->getFilePath() == path) {
            editorTabs->setCurrentIndex(i);
            return;
        }
    }

    CodeEditor* newEditor = new CodeEditor(editorTabs);
    newEditor->loadFile(path);
    connect(newEditor, &CodeEditor::cursorPositionUpdated,
            this, &MainWindow::onCursorPositionUpdated);
    connect(newEditor, &CodeEditor::modifyChanged,
            this, &MainWindow::onModificationChanged);
    connect(newEditor, &CodeEditor::charInserted,
            this, [this, newEditor](int pos, QChar ch) {
                if (!collabActive || newEditor->isApplyingRemote()) return;
                rgamanager->localInsert(pos, ch);
            });
    const QString name = QFileInfo(path).fileName();
    int index = editorTabs->addTab(newEditor, name);
    editorTabs->setCurrentIndex(index);
    currentFilePath = path;
    //statusFile->setText(name);
    updateWindowTitle();
    outputPane->appendPlainText("[TeamHub] Opened: " + path);
}

void MainWindow::setupEditorArea()
{
    editorTabs = new QTabWidget;
    editorTabs->setObjectName("editorTabs");
    editorTabs->setTabsClosable(true);
    editorTabs->setMovable(true);
    editorTabs->setDocumentMode(true);

    editor = new CodeEditor(editorTabs);
    editorTabs->addTab(editor, "Untitled");

    connect(editorTabs, &QTabWidget::tabCloseRequested,
            this, &MainWindow::onTabCloseRequested);
}


void MainWindow::setupBottomDock()
{
    bottomDock = new QDockWidget("Panel", this);
    bottomDock->setObjectName("bottomDock");
    bottomDock->setFeatures(QDockWidget::DockWidgetClosable |
                              QDockWidget::DockWidgetMovable);
    bottomDock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
    bottomDock->setTitleBarWidget(new QWidget);

    bottomTabs = new QTabWidget;
    bottomTabs->setObjectName("bottomTabs");
    bottomTabs->setTabPosition(QTabWidget::South);

    outputPane = new QPlainTextEdit;
    outputPane->setObjectName("outputPane");
    outputPane->setReadOnly(true);
    outputPane->setPlaceholderText("Build and run output will appear here...");
    bottomTabs->addTab(outputPane, "Output");

    terminal = new Terminal(this);
    terminal->setWorkingDirectory(fileBrowser->rootPath());
    terminal->setObjectName("terminal");
    bottomTabs->addTab(terminal, "Terminal");

    gitPane = new QWidget;
    gitPane->setObjectName("gitPane");
    {
        auto *l   = new QVBoxLayout(gitPane);
        auto *lbl = new QLabel(
            "Git integration\n\n"
            "Planned: staged / unstaged changes, commit history, inline diff.");
        lbl->setAlignment(Qt::AlignCenter);
        lbl->setWordWrap(true);
        lbl->setObjectName("stubLabel");
        l->addWidget(lbl);
    }
    bottomTabs->addTab(gitPane, "Git");

    bottomDock->setWidget(bottomTabs);
    addDockWidget(Qt::BottomDockWidgetArea, bottomDock);
    resizeDocks({bottomDock}, {180}, Qt::Vertical);
}

void MainWindow::setupVoipDock()
{
    voipDock = new QDockWidget("Voice", this);
    voipDock->setObjectName("voipDock");
    voipDock->setFeatures(QDockWidget::DockWidgetClosable  |
                            QDockWidget::DockWidgetMovable   |
                            QDockWidget::DockWidgetFloatable);
    voipDock->setAllowedAreas(Qt::RightDockWidgetArea | Qt::LeftDockWidgetArea);

    auto *panel = new QWidget;
    panel->setObjectName("voipPanel");
    auto *vbox = new QVBoxLayout(panel);
    vbox->setContentsMargins(12, 12, 12, 12);
    vbox->setSpacing(12);

    auto *statusLbl = new QLabel("Not connected");
    statusLbl->setAlignment(Qt::AlignCenter);
    statusLbl->setObjectName("stubLabel");
    vbox->addWidget(statusLbl);

    vbox->addStretch(1);

    auto *noteLbl = new QLabel(
        "VoIP module\n\n"
        "Planned: P2P voice over UDP,\nOpus codec, STUN/TURN.");
    noteLbl->setAlignment(Qt::AlignCenter);
    noteLbl->setWordWrap(true);
    noteLbl->setObjectName("stubLabel");
    vbox->addWidget(noteLbl);

    vbox->addStretch(1);

    voipDock->setWidget(panel);
    addDockWidget(Qt::RightDockWidgetArea, voipDock);
    voipDock->hide();
}

void MainWindow::setupStatusBar()
{
    statusFile     = new QLabel("Untitled");
    statusPosition = new QLabel("Ln 1, Col 1");
    statusEncoding = new QLabel("UTF-8");
    statusLanguage = new QLabel("Python");

    for (auto *lbl : {statusFile, statusPosition, statusEncoding, statusLanguage}) {
        lbl->setContentsMargins(8, 0, 8, 0);
        lbl->setObjectName("statusLabel");
    }

    statusBar()->addWidget(statusFile, 1);
    statusBar()->addPermanentWidget(statusPosition);
    statusBar()->addPermanentWidget(statusEncoding);
    statusBar()->addPermanentWidget(statusLanguage);
}

void MainWindow::onTabChanged(int index){
    CodeEditor* activeEditor = qobject_cast<CodeEditor*>(
        editorTabs->widget(index));
    if(!activeEditor) return;
    editor = activeEditor;
    QString path = activeEditor->getFilePath();
    currentFilePath = path;
    QString filename;
    if(path.isEmpty()){
        filename = "Untitled";
        statusFile->setText(filename);
    }else{
        filename = QFileInfo(path).fileName();
    }
    statusFile->setText(filename);
    statusPosition->setText(
        QString("Ln %1, Col %2")
            .arg(activeEditor->currentLine() + 1)
            .arg(activeEditor->currentColumn() + 1));
    updateWindowTitle();
}

void MainWindow::applyTheme()
{
    setStyleSheet(R"(
/* ── Main window ────────────────────────────────────────────── */
QMainWindow { background: #1e1e1e; }

/* ── Menu bar ───────────────────────────────────────────────── */
QMenuBar {
    background: #3c3c3c;
    color: #cccccc;
    border-bottom: 1px solid #252526;
}
QMenuBar::item:selected { background: #505050; }
QMenu {
    background: #252526;
    color: #cccccc;
    border: 1px solid #454545;
}
QMenu::item:selected    { background: #094771; }
QMenu::item:disabled    { color: #555555; }
QMenu::separator {
    height: 1px;
    background: #454545;
    margin: 2px 0;
}

/* ── Toolbar ────────────────────────────────────────────────── */
QToolBar#mainToolBar {
    background: #3c3c3c;
    border-bottom: 1px solid #252526;
    spacing: 2px;
    padding: 2px 4px;
}
QToolBar#mainToolBar QToolButton {
    color: #cccccc;
    background: transparent;
    border: 1px solid transparent;
    border-radius: 3px;
    padding: 3px 8px;
}
QToolBar#mainToolBar QToolButton:hover   { background: #505050; border-color: #606060; }
QToolBar#mainToolBar QToolButton:pressed { background: #3a3a3a; }
QToolBar#mainToolBar QToolButton:disabled{ color: #555555; }
QToolBar::separator {
    width: 1px;
    background: #555555;
    margin: 4px 4px;
}

/* ── Activity Bar ───────────────────────────────────────────── */
QWidget#activityBar {
    background: #333333;
    border-right: 1px solid #252526;
}
QToolButton#activityBtn,
QToolButton#activityBtnVoip {
    color: #858585;
    background: transparent;
    border: none;
    border-left: 2px solid transparent;
    font-size: 9px;
}
QToolButton#activityBtn:hover,
QToolButton#activityBtnVoip:hover { color: #cccccc; }
QToolButton#activityBtn:checked   { color: #ffffff; border-left-color: #007acc; }
QToolButton#activityBtnVoip:checked { color: #ffffff; border-left-color: #007acc; }

/* ── Left Panel ─────────────────────────────────────────────── */
QWidget#leftPanel {
    background: #252526;
    border-right: 1px solid #3c3c3c;
}
QLabel#panelTitle {
    color: #bbbbbb;
    font-size: 10px;
    font-weight: bold;
    background: #252526;
    letter-spacing: 1px;
}
QFrame#panelSeparator {
    color: #3c3c3c;
    max-height: 1px;
    background: #3c3c3c;
}
QTreeWidget#fileTree {
    background: #252526;
    color: #cccccc;
    border: none;
    outline: 0;
}
QTreeWidget#fileTree::item:hover    { background: #2a2d2e; }
QTreeWidget#fileTree::item:selected { background: #094771; }
QListWidget#taskList,
QListWidget#teamList {
    background: #252526;
    color: #cccccc;
    border: none;
}
QListWidget#taskList::item:hover,
QListWidget#teamList::item:hover    { background: #2a2d2e; }
QListWidget#taskList::item:selected,
QListWidget#teamList::item:selected { background: #094771; }

QTreeView#fileBrowserTree {
    background: #252526;
    color: #cccccc;
    border: none;
    outline: 0;
}
QTreeView#fileBrowserTree::item {
    height: 22px;
    padding-left: 4px;
}
QTreeView#fileBrowserTree::item:hover {
    background: #2a2d2e;
}
QTreeView#fileBrowserTree::item:selected {
    background: #094771;
    color: #ffffff;
}
QTreeView#fileBrowserTree::branch {
    background: #252526;
}
QLineEdit#fileSearch {
    background: #3c3c3c;
    color: #cccccc;
    border: none;
    border-bottom: 1px solid #454545;
    padding: 4px 8px;
    font-size: 12px;
}

/* ── Splitter ───────────────────────────────────────────────── */
QSplitter#centralSplitter::handle {
    background: #3c3c3c;
    width: 1px;
}

/* ── Editor Tabs ────────────────────────────────────────────── */
QTabWidget#editorTabs::pane     { border: none; background: #1e1e1e; }
QTabWidget#editorTabs > QTabBar::tab {
    background: #2d2d2d;
    color: #9d9d9d;
    border: none;
    border-right: 1px solid #252526;
    padding: 6px 14px;
    min-width: 80px;
}
QTabWidget#editorTabs > QTabBar::tab:selected {
    background: #1e1e1e;
    color: #ffffff;
    border-top: 1px solid #007acc;
}
QTabWidget#editorTabs > QTabBar::tab:hover:!selected {
    background: #383838;
    color: #cccccc;
}

/* ── Bottom Dock ────────────────────────────────────────────── */
QDockWidget#bottomDock  { background: #1e1e1e; color: #cccccc; }
QTabWidget#bottomTabs::pane  { border: none; background: #1e1e1e; }
QTabWidget#bottomTabs > QTabBar { background: #252526; }
QTabWidget#bottomTabs > QTabBar::tab {
    background: #252526;
    color: #9d9d9d;
    border: none;
    padding: 4px 12px;
}
QTabWidget#bottomTabs > QTabBar::tab:selected {
    background: #1e1e1e;
    color: #ffffff;
    border-top: 1px solid #007acc;
}
QTabWidget#bottomTabs > QTabBar::tab:hover:!selected {
    color: #cccccc;
    background: #2a2d2e;
}
QPlainTextEdit#outputPane,
QPlainTextEdit#terminalPane {
    background: #1e1e1e;
    color: #d4d4d4;
    border: none;
    font-family: Consolas, "Courier New", monospace;
    font-size: 11px;
}

/* ── VoIP Dock ──────────────────────────────────────────────── */
QDockWidget#voipDock  { color: #cccccc; }
QWidget#voipPanel     { background: #252526; }

/* ── Shared stub label ──────────────────────────────────────── */
QLabel#stubLabel { color: #555555; font-size: 11px; }

/* ── Status Bar ─────────────────────────────────────────────── */
QStatusBar {
    background: #007acc;
    color: #ffffff;
    font-size: 11px;
}
QStatusBar::item { border: none; }
QLabel#statusLabel { color: #ffffff; padding: 0 4px; }
    )");
}

void MainWindow::setSidePanelPage(int index)
{
    const QStringList titles = {"EXPLORER", "TASKS", "TEAM"};

    if (index == activeSidePanel && leftPanel->isVisible()) {
        leftPanel->setVisible(false);
        activeSidePanel = -1;
        btnFiles->setChecked(false);
        btnTasks->setChecked(false);
        btnTeam->setChecked(false);
        return;
    }

    activeSidePanel = index;
    leftPanel->setVisible(true);
    leftStack->setCurrentIndex(index);
    leftTitle->setText(titles.value(index, "PANEL"));

    btnFiles->setChecked(index == 0);
    btnTasks->setChecked(index == 1);
    btnTeam->setChecked(index == 2);
}

void MainWindow::updateWindowTitle()
{
    const QString name    = currentFilePath.isEmpty()
                            ? "Untitled"
                            : QFileInfo(currentFilePath).fileName();
    const QString dirty   = editor->isModified() ? " \u25cf" : "";
    setWindowTitle(name + dirty + " \u2014 TeamHub");
}

void MainWindow::onActivityButton(int page)
{
    setSidePanelPage(page);
}

void MainWindow::onCursorPositionUpdated(int line, int index)
{
    statusPosition->setText(
        QString("Ln %1, Col %2").arg(line + 1).arg(index + 1));
}

void MainWindow::onModificationChanged(bool modified)
{
    const QString name  = currentFilePath.isEmpty()
                          ? "Untitled"
                          : QFileInfo(currentFilePath).fileName();
    const QString dirty = modified ? " \u25cf" : "";

    setWindowTitle(name + dirty + " \u2014 TeamHub");

    const int idx = editorTabs->currentIndex();
    if (idx >= 0)
        editorTabs->setTabText(idx, name + dirty);
}

void MainWindow::onTabCloseRequested(int tabIndex)
{
    if (editorTabs->count() <= 1) return;

    CodeEditor* tabEditor = qobject_cast<CodeEditor*>(editorTabs->widget(tabIndex));
    if (!tabEditor) return;

    if (tabEditor->isModified()) {
        const auto btn = QMessageBox::question(
            this, "Unsaved Changes",
            "Save changes before closing this tab?",
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);

        if (btn == QMessageBox::Save) {
            editor = tabEditor;
            if (!saveFile()) return;
        } else if (btn == QMessageBox::Cancel) {
            return;
        }
    }

    editorTabs->removeTab(tabIndex);
}

void MainWindow::runFile()
{
    CodeEditor* currentEditor = qobject_cast<CodeEditor*>(
        editorTabs->currentWidget());
    if (!currentEditor) return;

    QString path = currentEditor->getFilePath();
    if (path.isEmpty()) {
        outputPane->appendPlainText("[TeamHub] Save file before running.");
        return;
    }

    QString projectDir = fileBrowser->rootPath();
    QString pythonvenv = fileBrowser->findFile("python.exe");
    QString pythonpath = !pythonvenv.isEmpty() ? pythonvenv : "python";
    currentEditor->saveFile(path);
    outputPane->clear();
    bottomTabs->setCurrentWidget(outputPane);
    bottomDock->setVisible(true);

    QProcess* proc = new QProcess(this);
    proc->setProcessChannelMode(QProcess::MergedChannels);

    connect(proc, &QProcess::readyReadStandardOutput, this, [this, proc]() {
        outputPane->appendPlainText(
            QString::fromLocal8Bit(proc->readAllStandardOutput()));
    });

    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, proc](int code, QProcess::ExitStatus) {
                outputPane->appendPlainText(
                    QString("\n[TeamHub] Exit code: %1").arg(code));
                proc->deleteLater();
            });

    proc->start(pythonpath, {path});
}

void MainWindow::newFile()
{
    if (editor->isModified()) {
        const auto btn = QMessageBox::question(
            this, "Unsaved Changes",
            "Save changes before creating a new file?",
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        if (btn == QMessageBox::Save)        { if (!saveFile()) return; }
        else if (btn == QMessageBox::Cancel) return;
    }

    editor->clear();
    editor->setModified(false);
    currentFilePath.clear();
    statusFile->setText("Untitled");
    editorTabs->setTabText(editorTabs->currentIndex(), "Untitled");
    updateWindowTitle();
}

void MainWindow::clearTabs(){
    while(editorTabs->count() > 0){
       QWidget *w = editorTabs->widget(0);
       editorTabs->removeTab(0);
       delete w;
    }
}

void MainWindow::openFile()
{
    const QString path = QFileDialog::getOpenFileName(
        this, "Open File", {},
        "Python Files (*.py);;All Files (*)");
    if (path.isEmpty()) return;

    CodeEditor* newEditor = new CodeEditor(editorTabs);
    newEditor->loadFile(path);
    connect(newEditor, &CodeEditor::cursorPositionUpdated,
            this, &MainWindow::onCursorPositionUpdated);
    connect(newEditor, &CodeEditor::modifyChanged,
            this, &MainWindow::onModificationChanged);
    connect(newEditor, &CodeEditor::charInserted,
            this, [this, newEditor](int pos, QChar ch) {
                if (!collabActive || newEditor->isApplyingRemote()) return;
                rgamanager->localInsert(pos, ch);
            });
    const QString name = QFileInfo(path).fileName();
    int index = editorTabs->addTab(newEditor, name);
    editorTabs->setCurrentIndex(index);

    currentFilePath = path;
    statusFile->setText(name);
    updateWindowTitle();
    outputPane->appendPlainText("[TeamHub] Opened: " + path);
}

void MainWindow::openFolder()
{
    const QString path = QFileDialog::getExistingDirectory(this, "Open Folder");
    if (path.isEmpty()) return;

    fileBrowser->setRootPath(path);
    clearTabs();

    editor = new CodeEditor(editorTabs);
    editorTabs->addTab(editor, "Untitled");

    currentFilePath.clear();
    setWindowTitle(QFileInfo(path).fileName() + " — TeamHub");
    outputPane->appendPlainText("[TeamHub] Opened folder: " + path);

    terminal->setWorkingDirectory(path);
}

bool MainWindow::saveFile()
{
    CodeEditor* activeEditor = qobject_cast<CodeEditor*>(
        editorTabs->currentWidget());
    if (!activeEditor) return false;

    QString path = activeEditor->getFilePath();

    if (path.isEmpty())
        return saveFileAs();

    activeEditor->saveFile(path);

    const QString name = QFileInfo(path).fileName();
    currentFilePath = path;
    statusFile->setText(name);
    editorTabs->setTabText(editorTabs->currentIndex(), name);
    updateWindowTitle();
    outputPane->appendPlainText("[TeamHub] Saved: " + path);
    return true;
}

bool MainWindow::saveFileAs()
{
    const QString path = QFileDialog::getSaveFileName(
        this, "Save File As", {},
        "Python Files (*.py);;All Files (*)");
    if (path.isEmpty()) return false;

    currentFilePath = path;
    return saveFile();
}

void MainWindow::toggleSidePanel()
{
    if (leftPanel->isVisible()) {
        leftPanel->setVisible(false);
        activeSidePanel = -1;
        btnFiles->setChecked(false);
        btnTasks->setChecked(false);
        btnTeam->setChecked(false);
    } else {
        setSidePanelPage(0);
    }
}

void MainWindow::toggleBottomDock()
{
    bottomDock->setVisible(!bottomDock->isVisible());
}

void MainWindow::toggleVoipDock()
{
    voipDock->setVisible(!voipDock->isVisible());
    btnVoip->setChecked(voipDock->isVisible());
}

void MainWindow::onCollabTextChanged(const QString& newText)
{
    if (!editor) return;
    editor->applyRemoteText(newText);
}

void MainWindow::startCollab()
{
    bool ok;
    QString room = QInputDialog::getText(
        this, "Join Session",
        "Room name:", QLineEdit::Normal,
        "room1", &ok);

    if (!ok || room.isEmpty()) return;

    QString url = "ws://localhost:8765/" + room;
    rgamanager->connectToServer(url);
    collabActive = true;

    QTimer::singleShot(500, this, [this]() {
        QString currentText = editor->text();
        for (int i = 0; i < currentText.length(); i++) {
            rgamanager->localInsert(i, currentText[i]);
        }
    });

    outputPane->appendPlainText("[Collab] Connecting to " + url);
}

void MainWindow::stopCollab()
{
    collabActive = false;
    outputPane->appendPlainText("[Collab] Session stopped.");
}

CodeEditor* MainWindow::currentEditor()
{
    return qobject_cast<CodeEditor*>(editorTabs->currentWidget());
}