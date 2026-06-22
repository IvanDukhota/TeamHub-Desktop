#include "mainwindow.h"

#include "auth/authdialog.h"
#include "avatar/avatar.h"
#include "config/appconfig.h"
#include "db/projectdb.h"
#include "settings/settingsdialog.h"
#include "settings/settingsmanager.h"

#ifdef Q_OS_WIN
#include <dwmapi.h>
#include <windows.h>
#endif

#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QCloseEvent>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPixmap>
#include <QPointer>
#include <QProcess>
#include <QRadioButton>
#include <QRandomGenerator>
#include <QSizePolicy>
#include <QSplitter>
#include <QStatusBar>
#include <QTabBar>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <functional>

static QIcon loadIconTransparent(const QString &name, bool removeDark = false)
{
    QString path = QCoreApplication::applicationDirPath() + "/icons/" + name;
    QImage img(path);
    if (img.isNull())
        img = QImage(QString(TEAMHUB_ICONS_DIR) + name);
    if (img.isNull())
        return QIcon();
    img = img.convertToFormat(QImage::Format_ARGB32);

    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) {
            const QColor c(img.pixel(x, y));
            const bool isLight = c.red() > 230 && c.green() > 230 && c.blue() > 230;
            const bool isDark = removeDark && c.red() < 25 && c.green() < 25 && c.blue() < 25;
            if (isLight || isDark)
                img.setPixel(x, y, qRgba(0, 0, 0, 0));
        }

    return QIcon(QPixmap::fromImage(img));
}

static QString processedIcon(const QString &name)
{
    const QString runtimeDir = QCoreApplication::applicationDirPath() + "/icons/";
    const QString dest = runtimeDir + "_proc_" + name;
    if (QFile::exists(dest))
        return QString(dest).replace('\\', '/');
    QImage img(QString(TEAMHUB_ICONS_DIR) + name);
    if (img.isNull())
        img = QImage(runtimeDir + name);
    if (img.isNull())
        return (QString(TEAMHUB_ICONS_DIR) + name).replace('\\', '/');
    img = img.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) {
            const QColor c(img.pixel(x, y));
            if (c.red() > 230 && c.green() > 230 && c.blue() > 230)
                img.setPixel(x, y, qRgba(0, 0, 0, 0));
        }
    QDir().mkpath(runtimeDir);
    img.save(dest, "PNG");
    return QString(dest).replace('\\', '/');
}

static QString loadStyle(const QString &name)
{
    QFile f(QCoreApplication::applicationDirPath() + "/styles/" + name);
    if (!f.open(QIODevice::ReadOnly))
        f.setFileName(QString(TEAMHUB_STYLES_DIR) + name);
    if (f.isOpen() || f.open(QIODevice::ReadOnly)) {
        QString s = QString::fromUtf8(f.readAll());
        s.replace("${ICONS_DIR}arrow_down.png", processedIcon("arrow_down.png"));
        s.replace("${ICONS_DIR}arrow_up.png", processedIcon("arrow_up.png"));
        s.replace("${ICONS_DIR}", QString(TEAMHUB_ICONS_DIR));
        return s;
    }
    return {};
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , activeSidePanel(0)
{
    setWindowTitle("TeamHub");
    setWindowIcon(QIcon(QCoreApplication::applicationDirPath() + "/icons/th.ico"));
    resize(1400, 900);
    setMinimumSize(800, 500);

    SettingsManager::instance().load();
    ProjectDB::instance().open();

    voiceChat = new VoiceChat(this);

    setupMenuBar();
    setupMainToolBar();
    setupCentralWidget();
    setupBottomDock();
    setupStatusBar();
    applyTheme();

    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
        if (!now || !splitActive)
            return;
        if (editorTabs->isAncestorOf(now))
            focusedTabs = editorTabs;
        else if (editorTabs2 && editorTabs2->isAncestorOf(now))
            focusedTabs = editorTabs2;
    });

    connect(teamsPanel, &TeamsPanel::logMessage, outputPane, &QPlainTextEdit::appendPlainText);
    teamsPanel->setVoiceChat(voiceChat);

    if (gitPanel_ && !fileBrowser->rootPath().isEmpty())
        gitPanel_->setRepoPath(fileBrowser->rootPath());

    setSidePanelPage(0);
    btnFiles->setChecked(true);
    connect(editorTabs, &QTabWidget::currentChanged, this, [this] { updateRunCombo(); });
    connect(editorTabs2, &QTabWidget::currentChanged, this, [this] { updateRunCombo(); });
    connect(editorTabs, &QTabWidget::tabCloseRequested, this, [this] {
        QTimer::singleShot(0, this, &MainWindow::updateRunCombo);
    });
    connect(editorTabs2, &QTabWidget::tabCloseRequested, this, [this] {
        QTimer::singleShot(0, this, &MainWindow::updateRunCombo);
    });
    outputPane->appendPlainText("[TeamHub] Ready.");
    updateWindowTitle();
    setupAuthManager();

    const QString last = SettingsManager::instance().lastProjectPath();
    if (!last.isEmpty()) {
        if (QFileInfo::exists(last))
            openProjectFolder(last);
        else {
            SettingsManager::instance().setLastProjectPath("");
            SettingsManager::instance().save();
            ProjectDB::instance().removeProject(last);
        }
    }

    applySettings();
}

MainWindow::~MainWindow() = default;

void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
#ifdef Q_OS_WIN
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    const BOOL dark = TRUE;
    if (FAILED(
            DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark))))
        DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
    const COLORREF captionColor = RGB(0x1e, 0x1e, 0x1e);
    DwmSetWindowAttribute(hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &captionColor, sizeof(captionColor));
#endif
}

bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
    return QMainWindow::nativeEvent(eventType, message, result);
}

bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == outputPane && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        const bool processRunning = runProcess && runProcess->state() == QProcess::Running;

        if (!processRunning)
            return QMainWindow::eventFilter(obj, event);

        const int key = ke->key();

        if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            QTextCursor end(outputPane->document());
            end.movePosition(QTextCursor::End);
            end.setPosition(inputStartPos, QTextCursor::KeepAnchor);
            QTextCursor sel(outputPane->document());
            sel.setPosition(inputStartPos);
            sel.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
            QString inputText = sel.selectedText();
            inputText.replace(QChar(0x2029), '\n');
            inputText = inputText.trimmed();
            runProcess->write((inputText + "\n").toLocal8Bit());
            QTextCursor c = outputPane->textCursor();
            c.movePosition(QTextCursor::End);
            c.insertText("\n");
            inputStartPos = c.position();
            return true;
        }

        if (key == Qt::Key_Backspace) {
            QTextCursor c = outputPane->textCursor();
            if (!c.hasSelection() && c.position() <= inputStartPos)
                return true;
        }

        if (!ke->text().isEmpty()) {
            QTextCursor c = outputPane->textCursor();
            if (c.position() < inputStartPos) {
                c.movePosition(QTextCursor::End);
                outputPane->setTextCursor(c);
            }
        }
    }
    if (event->type() == QEvent::MouseButtonPress) {
        const QString path = obj->property("recentPath").toString();
        if (!path.isEmpty()) {
            if (QFileInfo::exists(path))
                openProjectFolder(path);
            else {
                ProjectDB::instance().removeProject(path);
                outputPane->appendPlainText("[TeamHub] Project not found: " + path);
                refreshWelcomeRecent();
            }
            return true;
        }
    }

    return QMainWindow::eventFilter(obj, event);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (editor && editor->isModified()) {
        const auto btn
            = QMessageBox::question(this,
                                    "Unsaved Changes",
                                    "The current file has unsaved changes.\nSave before closing?",
                                    QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                                    QMessageBox::Save);

        if (btn == QMessageBox::Save) {
            if (!saveFile()) {
                event->ignore();
                return;
            }
        } else if (btn == QMessageBox::Cancel) {
            event->ignore();
            return;
        }
    }
    saveSessionToDb();
    SettingsManager::instance().setLastProjectPath(currentProjectPath);
    SettingsManager::instance().save();
    event->accept();
}

CodeEditor *MainWindow::createTab(const QString &name)
{
    QTabWidget *target = focusedTabs ? focusedTabs : editorTabs;
    CodeEditor *ed = new CodeEditor(target);
    connect(ed, &CodeEditor::cursorPositionUpdated, this, &MainWindow::onCursorPositionUpdated);
    connect(ed, &CodeEditor::modifyChanged, this, &MainWindow::onModificationChanged);
    connect(ed, &CodeEditor::fileModified, this, [this, ed] {
        if (editor == ed)
            updateUndoRedoState();
    });
    target->addTab(ed, name);

    return ed;
}

void MainWindow::setupMenuBar()
{
    QMenu *fileMenu = menuBar()->addMenu("&File");
    fileMenu->addAction("&New File", QKeySequence::New, this, &MainWindow::newFile);
    fileMenu->addAction("&Open Folder", {}, this, &MainWindow::openFolder);
    fileMenu->addAction("&Open File...", QKeySequence::Open, this, &MainWindow::openFile);
    fileMenu->addAction("&Save", QKeySequence::Save, this, [this] { saveFile(); });
    fileMenu->addAction("Save &As...", QKeySequence::SaveAs, this, [this] { saveFileAs(); });
    fileMenu->addSeparator();
    fileMenu->addAction("Close &Tab", QKeySequence("Ctrl+W"), this, [this] {
        QTabWidget *tabs = focusedTabs ? focusedTabs : editorTabs;
        const int idx = tabs->currentIndex();
        if (idx < 0)
            return;
        CodeEditor *ed = qobject_cast<CodeEditor *>(tabs->widget(idx));
        if (!ed)
            return;
        if (session) {
            const QString relPath = toSessionKey(ed->getFilePath());
            if (session->hasActiveRGA(relPath)) {
                ed->clearRemoteCursors();
                session->releaseRGA(relPath);
            }
            markTabAsCollab(ed, false);
        }
        if (ed->isModified() && !(session && session->role() == CollabSession::Role::Guest)) {
            const auto btn = QMessageBox::question(this,
                                                   "Unsaved Changes",
                                                   "Save changes before closing this tab?",
                                                   QMessageBox::Save | QMessageBox::Discard
                                                       | QMessageBox::Cancel);
            if (btn == QMessageBox::Save) {
                editor = ed;
                if (!saveFile())
                    return;
            } else if (btn == QMessageBox::Cancel) {
                return;
            }
        }
        tabs->removeTab(idx);
        ed->deleteLater();
    });
    fileMenu->addSeparator();

    QMenu *recentMenu = fileMenu->addMenu("Recent &Projects");
    connect(recentMenu, &QMenu::aboutToShow, this, [this, recentMenu] {
        refreshRecentMenu(recentMenu);
    });

    fileMenu->addSeparator();
    fileMenu->addAction("&Settings...", QKeySequence("Ctrl+,"), this, &MainWindow::openSettings);
    fileMenu->addSeparator();
    fileMenu->addAction("E&xit", QKeySequence::Quit, qApp, &QApplication::quit);

    QMenu *editMenu = menuBar()->addMenu("&Edit");
    editMenu->addAction("&Undo", QKeySequence::Undo, this, [this] {
        if (editor)
            editor->undo();
    });
    editMenu->addAction("&Redo", QKeySequence::Redo, this, [this] {
        if (editor)
            editor->redo();
    });
    editMenu->addSeparator();
    editMenu->addAction("Cu&t", QKeySequence::Cut, this, [this] {
        if (editor)
            editor->cut();
    });
    editMenu->addAction("&Copy", QKeySequence::Copy, this, [this] {
        if (editor)
            editor->copy();
    });
    editMenu->addAction("&Paste", QKeySequence::Paste, this, [this] {
        if (editor)
            editor->paste();
    });
    editMenu->addAction("Select &All", QKeySequence::SelectAll, this, [this] {
        if (editor)
            editor->selectAll();
    });
    editMenu->addSeparator();
    auto *actFind = editMenu->addAction("&Find / Replace...", QKeySequence("Ctrl+F"));
    connect(actFind, &QAction::triggered, this, [this]() {
        QTabWidget *activeTabs = focusedTabs ? focusedTabs : editorTabs;
        CodeEditor *ed = qobject_cast<CodeEditor *>(activeTabs->currentWidget());
        if (ed)
            ed->showSearch();
    });

    QMenu *viewMenu = menuBar()->addMenu("&View");
    viewMenu->addAction("Toggle &Side Panel",
                        QKeySequence("Ctrl+B"),
                        this,
                        &MainWindow::toggleSidePanel);
    viewMenu->addAction("Toggle &Output Panel",
                        QKeySequence("Ctrl+J"),
                        this,
                        &MainWindow::toggleBottomDock);
    viewMenu->addAction("Toggle &Team Panel", {}, this, [this] { onActivityButton(2); });
    viewMenu->addSeparator();
    viewMenu->addAction("Split Editor Right", QKeySequence("Ctrl+\\"), this, [this] {
        QTabWidget *source = focusedTabs ? focusedTabs : editorTabs;
        CodeEditor *ed = qobject_cast<CodeEditor *>(source->currentWidget());
        if (ed && !ed->getFilePath().isEmpty())
            openInSplitPanel(ed->getFilePath(), source);
    });
    viewMenu->addSeparator();
    viewMenu->addAction("Zoom &In", QKeySequence::ZoomIn, this, [this] {
        if (editor)
            editor->zoomIn();
    });
    viewMenu->addAction("Zoom &Out", QKeySequence::ZoomOut, this, [this] {
        if (editor)
            editor->zoomOut();
    });
    viewMenu->addAction("Reset &Zoom", QKeySequence("Ctrl+0"), this, [this] {
        if (editor)
            editor->resetZoom();
    });
    viewMenu->addSeparator();
    QMenu *themeMenu = viewMenu->addMenu("&Theme");
    themeMenu->addAction("Dark", this, [this] {
        if (editor)
            editor->setTheme(CodeEditor::Theme::Dark);
    });
    themeMenu->addAction("Light", this, [this] {
        if (editor)
            editor->setTheme(CodeEditor::Theme::Light);
    });

    QMenu *gitMenu = menuBar()->addMenu("&Git");

    gitMenu->addAction("Init Repository", this, [this]() {
        if (!gitPanel_)
            return;
        const QString path = fileBrowser->rootPath().isEmpty()
                                 ? QFileDialog::getExistingDirectory(this, "Select Project Folder")
                                 : fileBrowser->rootPath();
        if (!path.isEmpty()) {
            gitPanel_->initRepo(path);
            bottomTabs->setCurrentWidget(gitPane);
            bottomDock->setVisible(true);
        }
    });

    gitMenu->addAction("Clone Repository...", this, &MainWindow::cloneRepo);

    gitMenu->addSeparator();

    auto *actRefresh = gitMenu->addAction("Refresh Status", this, [this]() {
        if (gitPanel_) {
            gitPanel_->refresh();
            bottomTabs->setCurrentWidget(gitPane);
            bottomDock->setVisible(true);
        }
    });
    actRefresh->setShortcut(QKeySequence("Ctrl+Shift+G"));

    gitMenu->addAction("Stage All", this, [this]() {
        if (gitPanel_) {
            gitPanel_->stageAll();
            bottomTabs->setCurrentWidget(gitPane);
            bottomDock->setVisible(true);
        }
    });

    gitMenu->addAction("Unstage All", this, [this]() {
        if (gitPanel_) {
            gitPanel_->unstageAll();
            bottomTabs->setCurrentWidget(gitPane);
            bottomDock->setVisible(true);
        }
    });

    gitMenu->addSeparator();

    auto *actCommit = gitMenu->addAction("Commit...", this, [this]() {
        if (gitPanel_) {
            bottomTabs->setCurrentWidget(gitPane);
            bottomDock->setVisible(true);
            gitPanel_->focusCommitMessage();
        }
    });
    actCommit->setShortcut(QKeySequence("Ctrl+K"));

    gitMenu->addSeparator();

    gitMenu->addAction("Pull", this, [this]() {
        if (gitPanel_) {
            gitPanel_->pull();
            bottomTabs->setCurrentWidget(outputPane);
            bottomDock->setVisible(true);
        }
    });

    gitMenu->addAction("Push", this, [this]() {
        if (gitPanel_) {
            gitPanel_->push();
            bottomTabs->setCurrentWidget(outputPane);
            bottomDock->setVisible(true);
        }
    });

    QMenu *teamMenu = menuBar()->addMenu("&Team");
    teamMenu->addAction("Connect to Server", this, &MainWindow::joinCollab);
    teamMenu->addSeparator();
    teamMenu->addAction("Voice Call", this, [this] { onActivityButton(2); });
}

void MainWindow::setupMainToolBar()
{
    mainToolBar = addToolBar("Main");
    auto *tb = mainToolBar;
    tb->setObjectName("mainToolBar");
    tb->setMovable(false);
    tb->setIconSize(QSize(16, 16));

    auto *actOpen = tb->addAction("Open Folder", this, &MainWindow::openFolder);
    actOpen->setIcon(loadIconTransparent("openfolder.png", true));
    actOpen->setToolTip("Open Folder");

    auto *actSave = tb->addAction("Save", this, [this] { saveFile(); });
    actSave->setIcon(loadIconTransparent("save.png", true));
    actSave->setToolTip("Save (Ctrl+S)");

    tb->addSeparator();

    actUndo = tb->addAction("Undo", this, [this] {
        if (auto *ed = qobject_cast<CodeEditor *>(
                (focusedTabs ? focusedTabs : editorTabs)->currentWidget()))
            ed->undo();
    });
    actUndo->setIcon(loadIconTransparent("undo.png"));
    actUndo->setToolTip("Undo (Ctrl+Z)");
    actUndo->setShortcut(QKeySequence::Undo);
    actUndo->setEnabled(false);

    actRedo = tb->addAction("Redo", this, [this] {
        if (auto *ed = qobject_cast<CodeEditor *>(
                (focusedTabs ? focusedTabs : editorTabs)->currentWidget()))
            ed->redo();
    });
    actRedo->setIcon(loadIconTransparent("redo.png"));
    actRedo->setToolTip("Redo (Ctrl+Y)");
    actRedo->setShortcut(QKeySequence::Redo);
    actRedo->setEnabled(false);

    tb->addSeparator();

    runFileCombo = new QComboBox(this);
    runFileCombo->setFixedWidth(160);
    runFileCombo->setToolTip("Select file to run");
    tb->addWidget(runFileCombo);

    auto *actRun = tb->addAction("Run", this, &MainWindow::runFile);
    actRun->setToolTip("Run (F5)");
    actRun->setShortcut(QKeySequence("F5"));
    const QIcon runIcon = loadIconTransparent("runcode.png");
    if (!runIcon.isNull())
        actRun->setIcon(runIcon);

    actDebugMain = tb->addAction("Debug", this, &MainWindow::startDebugging);
    actDebugMain->setToolTip("Start Debugging (F9)");
    actDebugMain->setShortcut(QKeySequence("F9"));
    const QIcon debugIcon = loadIconTransparent("debug.png");
    if (!debugIcon.isNull())
        actDebugMain->setIcon(debugIcon);

    actStop = tb->addAction(QString(QChar(0x25A0)), this, &MainWindow::stopRun);
    actStop->setToolTip("Stop (Shift+F5)");
    actStop->setShortcut(QKeySequence("Shift+F5"));
    actStop->setEnabled(false);
    QTimer::singleShot(0, this, [this, tb] {
        if (auto *btn = qobject_cast<QToolButton *>(tb->widgetForAction(actStop))) {
            btn->setToolButtonStyle(Qt::ToolButtonTextOnly);
            btn->setStyleSheet("QToolButton { color:#f44747; font-size:14px; font-weight:bold; }"
                               "QToolButton:disabled { color:#5a2020; }");
        }
    });

    auto *spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tb->addWidget(spacer);
}

void MainWindow::startCollab(const QString &room,
                             CollabSession::Mode mode,
                             const QStringList &selectedFiles)
{
    stopAllCollab();

    const int siteId = static_cast<int>(QRandomGenerator::global()->bounded(100000u, 999999u));

    session = new CollabSession(siteId, CollabSession::Role::Host, this);
    session->setCollabMode(mode);
    if (auth && auth->isLoggedIn()) {
        session->setUsername(auth->currentUser().username);
        session->setAvatarUrl(auth->currentUser().avatarUrl);
    }

    const QString projectRoot = fileBrowser->rootPath();

    QStringList allRelPaths;
    QMap<QString, QString> fileTexts;

    QDirIterator it(projectRoot,
                    {"*.py", "*.cpp", "*.h", "*.pro", "*.txt", "*.md", "*.json"},
                    QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString absPath = it.next();
        const QString relPath = QDir(projectRoot).relativeFilePath(absPath);
        allRelPaths.append(relPath);
        QFile f(absPath);
        if (f.open(QIODevice::ReadOnly | QIODevice::Text))
            fileTexts[relPath] = QString::fromUtf8(f.readAll());
    }

    for (QTabWidget *tabs : allTabWidgets()) {
        for (int i = 0; i < tabs->count(); ++i) {
            auto *ed = qobject_cast<CodeEditor *>(tabs->widget(i));
            if (!ed || ed->getFilePath().isEmpty())
                continue;
            const QString absPath = ed->getFilePath();
            if (!absPath.startsWith(projectRoot))
                continue;
            const QString relPath = QDir(projectRoot).relativeFilePath(absPath);
            if (!allRelPaths.contains(relPath))
                allRelPaths.append(relPath);
            fileTexts[relPath] = ed->text();
        }
    }

    if (!selectedFiles.isEmpty()) {
        QStringList filtered;
        for (const QString &p : std::as_const(allRelPaths))
            if (selectedFiles.contains(p))
                filtered.append(p);
        allRelPaths = filtered;
        for (auto jt = fileTexts.begin(); jt != fileTexts.end();)
            jt = selectedFiles.contains(jt.key()) ? ++jt : fileTexts.erase(jt);
    }

    for (auto it = fileTexts.cbegin(); it != fileTexts.cend(); ++it)
        session->initFileCache(it.key(), it.value());

    session->setProject(projectRoot, allRelPaths);

    for (QTabWidget *tabs : allTabWidgets()) {
        for (int i = 0; i < tabs->count(); ++i) {
            auto *ed = qobject_cast<CodeEditor *>(tabs->widget(i));
            if (!ed || ed->getFilePath().isEmpty())
                continue;
            const QString absPath = ed->getFilePath();
            if (!absPath.startsWith(projectRoot))
                continue;
            const QString relPath = QDir(projectRoot).relativeFilePath(absPath);
            wireEditorToSession(ed, relPath);
            markTabAsCollab(ed, true);
        }
    }

    connect(session, &CollabSession::rolesUpdated, this, [this](QMap<int, QString> roles) {
        for (auto it = roles.cbegin(); it != roles.cend(); ++it)
            peerRoles[it.key()] = it.value();
    });
    connect(session, &CollabSession::usersUpdated, this, &MainWindow::onCollabUsersUpdated);
    connect(session, &CollabSession::projectInitReceived, this, &MainWindow::onSessionProjectInit);
    connect(session, &CollabSession::runOutputReceived, this, &MainWindow::onSessionRunOutput);
    connect(session, &CollabSession::remoteFileCreated, this, &MainWindow::onSessionFileCreated);
    connect(session, &CollabSession::remoteFileDeleted, this, &MainWindow::onSessionFileDeleted);
    connect(session, &CollabSession::remoteFileRenamed, this, &MainWindow::onSessionFileRenamed);
    connect(session,
            &CollabSession::remoteFileFocusChanged,
            this,
            &MainWindow::onRemoteFileFocusChanged);
    connect(session, &CollabSession::sessionReportReady, this, &MainWindow::onSessionReportReady);
    connect(session,
            &CollabSession::sessionAiInsightsReady,
            this,
            &MainWindow::onSessionAiInsightsReady);
    connect(session, &CollabSession::peerRoleChanged, this, [this](int siteId, const QString &role) {
        peerRoles[siteId] = role;
        refreshCollabUsersList();
        if (session && siteId == session->siteId()) {
            outputPane->appendPlainText(QString("[Collab] Your role changed to: ")
                                        + (role == "read" ? "Read-only" : "Write"));
            const bool ro = (role == "read");
            for (QTabWidget *tabs : allTabWidgets()) {
                for (int i = 0; i < tabs->count(); ++i) {
                    if (auto *ed = qobject_cast<CodeEditor *>(tabs->widget(i)))
                        if (ed->collabActive)
                            ed->setReadOnly(ro);
                }
            }
        }
    });
    connect(session, &CollabSession::errorOccurred, this, [this](const QString &err) {
        outputPane->appendPlainText("[Collab] Error: " + err);
    });
    connect(session, &CollabSession::reconnecting, this, [this](int attempt, int maxAttempts) {
        const int delayS = 1 << (attempt - 1);
        const QString msg = QString("Reconnecting... attempt %1/%2 (in %3s)")
                                .arg(attempt)
                                .arg(maxAttempts)
                                .arg(delayS);
        outputPane->appendPlainText("[Collab] " + msg);
        if (collabStatusLabel)
            collabStatusLabel->setText(msg);
    });
    connect(session, &CollabSession::kicked, this, [this]() {
        QTimer::singleShot(0, this, [this]() {
            stopAllCollab();
            QMessageBox::information(this, "Collab", "You were kicked from the session.");
        });
    });

    connect(
        session,
        &CollabSession::connected,
        this,
        [this, fileTexts]() {
            session->sendAllSnapshots(fileTexts);
            outputPane->appendPlainText("[Collab] Project session started — you are host");
            if (collabStatusLabel)
                collabStatusLabel->setText("Hosting");
            if (collabNoSessionPane)
                collabNoSessionPane->hide();
            if (collabInSessionPane)
                collabInSessionPane->show();
            sessionStart = QDateTime::currentDateTime();
            if (!sessionTimer) {
                sessionTimer = new QTimer(this);
                connect(sessionTimer, &QTimer::timeout, this, [this]() {
                    const qint64 s = sessionStart.secsTo(QDateTime::currentDateTime());
                    if (sessionTimerLabel)
                        sessionTimerLabel->setText(QString("%1:%2:%3")
                                                       .arg(s / 3600, 2, 10, QChar('0'))
                                                       .arg((s % 3600) / 60, 2, 10, QChar('0'))
                                                       .arg(s % 60, 2, 10, QChar('0')));
                });
            }
            sessionTimer->start(1000);
            showToast("Collab session started", "success");
            const QString path = editor ? editor->getFilePath() : QString();
            if (!path.isEmpty()) {
                const QString relPath = toSessionKey(path);
                currentCollabFile = relPath;
                session->sendFileFocus(relPath);
                peerFiles[session->siteId()] = relPath;
                refreshCollabUsersList();
                refreshPresenceBar();
            }
        },
        Qt::SingleShotConnection);

    session->connectToServer(QString("%1/%2").arg(AppConfig::rgaServerUrl(), room));
    outputPane->appendPlainText("[Collab] Starting room: " + room);
}

void MainWindow::joinCollab()
{
    bool ok = false;
    const QString room = QInputDialog::getText(this,
                                               "Join Collaboration",
                                               "Enter room name:",
                                               QLineEdit::Normal,
                                               "",
                                               &ok)
                             .trimmed();
    if (!ok || room.isEmpty())
        return;

    stopAllCollab();

    const int siteId = static_cast<int>(QRandomGenerator::global()->bounded(100000u, 999999u));

    session = new CollabSession(siteId, CollabSession::Role::Guest, this);
    if (auth && auth->isLoggedIn()) {
        session->setUsername(auth->currentUser().username);
        session->setAvatarUrl(auth->currentUser().avatarUrl);
    }

    connect(session, &CollabSession::rolesUpdated, this, [this](QMap<int, QString> roles) {
        for (auto it = roles.cbegin(); it != roles.cend(); ++it)
            peerRoles[it.key()] = it.value();
    });
    connect(session, &CollabSession::projectInitReceived, this, &MainWindow::onSessionProjectInit);
    connect(session, &CollabSession::runOutputReceived, this, &MainWindow::onSessionRunOutput);
    connect(session, &CollabSession::usersUpdated, this, &MainWindow::onCollabUsersUpdated);
    connect(session, &CollabSession::remoteFileCreated, this, &MainWindow::onSessionFileCreated);
    connect(session, &CollabSession::remoteFileDeleted, this, &MainWindow::onSessionFileDeleted);
    connect(session, &CollabSession::remoteFileRenamed, this, &MainWindow::onSessionFileRenamed);
    connect(session,
            &CollabSession::remoteFileFocusChanged,
            this,
            &MainWindow::onRemoteFileFocusChanged);
    connect(session, &CollabSession::sessionReportReady, this, &MainWindow::onSessionReportReady);
    connect(session,
            &CollabSession::sessionAiInsightsReady,
            this,
            &MainWindow::onSessionAiInsightsReady);
    connect(session, &CollabSession::peerRoleChanged, this, [this](int siteId, const QString &role) {
        peerRoles[siteId] = role;
        refreshCollabUsersList();
        if (session && siteId == session->siteId()) {
            outputPane->appendPlainText(QString("[Collab] Your role changed to: ")
                                        + (role == "read" ? "Read-only" : "Write"));
            const bool ro = (role == "read");
            for (QTabWidget *tabs : allTabWidgets()) {
                for (int i = 0; i < tabs->count(); ++i) {
                    if (auto *ed = qobject_cast<CodeEditor *>(tabs->widget(i)))
                        if (ed->collabActive)
                            ed->setReadOnly(ro);
                }
            }
        }
    });
    connect(session, &CollabSession::errorOccurred, this, [this, room](const QString &err) {
        outputPane->appendPlainText("[Collab] Error: " + err);
        QTimer::singleShot(0, this, [this, room, err]() {
            stopAllCollab();
            if (err == "Room not found")
                QMessageBox::warning(this,
                                     "Join Collab",
                                     QString("Room \"%1\" does not exist.").arg(room));
        });
    });
    connect(session, &CollabSession::reconnecting, this, [this](int attempt, int maxAttempts) {
        const int delayS = 1 << (attempt - 1);
        const QString msg = QString("Reconnecting... attempt %1/%2 (in %3s)")
                                .arg(attempt)
                                .arg(maxAttempts)
                                .arg(delayS);
        outputPane->appendPlainText("[Collab] " + msg);
        if (collabStatusLabel)
            collabStatusLabel->setText(msg);
    });
    connect(session, &CollabSession::kicked, this, [this]() {
        QTimer::singleShot(0, this, [this]() {
            stopAllCollab();
            QMessageBox::information(this, "Collab", "You were kicked from the session.");
        });
    });
    connect(session, &CollabSession::disconnected, this, [this]() {
        if (pendingEndCollab)
            return;
        QTimer::singleShot(0, this, [this]() {
            stopAllCollab();
            outputPane->appendPlainText("[Collab] Host left — session ended.");
        });
    });
    connect(
        session,
        &CollabSession::connected,
        this,
        [this]() {
            outputPane->appendPlainText("[Collab] Connected — waiting for project info…");
            if (collabStatusLabel)
                collabStatusLabel->setText("Guest — connected");
            if (collabNoSessionPane)
                collabNoSessionPane->hide();
            if (collabInSessionPane)
                collabInSessionPane->show();
            sessionStart = QDateTime::currentDateTime();
            if (!sessionTimer) {
                sessionTimer = new QTimer(this);
                connect(sessionTimer, &QTimer::timeout, this, [this]() {
                    const qint64 s = sessionStart.secsTo(QDateTime::currentDateTime());
                    if (sessionTimerLabel)
                        sessionTimerLabel->setText(QString("%1:%2:%3")
                                                       .arg(s / 3600, 2, 10, QChar('0'))
                                                       .arg((s % 3600) / 60, 2, 10, QChar('0'))
                                                       .arg(s % 60, 2, 10, QChar('0')));
                });
            }
            sessionTimer->start(1000);
            showToast("Joined collab session", "success");
        },
        Qt::SingleShotConnection);

    session->connectToServer(QString("%1/%2").arg(AppConfig::rgaServerUrl(), room));
    outputPane->appendPlainText("[Collab] Joining room: " + room);
}

void MainWindow::wireEditorToManager(CodeEditor *ed, RGAManager *mgr)
{
    connect(ed, &CodeEditor::localInsert, mgr, &RGAManager::localInsert, Qt::UniqueConnection);
    connect(ed, &CodeEditor::localDelete, mgr, &RGAManager::localRemove, Qt::UniqueConnection);
    connect(ed, &CodeEditor::undoRequested, mgr, &RGAManager::undo, Qt::UniqueConnection);
    connect(ed, &CodeEditor::redoRequested, mgr, &RGAManager::redo, Qt::UniqueConnection);
    connect(ed, &CodeEditor::beginUndoGroup, mgr, &RGAManager::beginGroup, Qt::UniqueConnection);
    connect(ed, &CodeEditor::endUndoGroup, mgr, &RGAManager::endGroup, Qt::UniqueConnection);
    ed->collabActive = true;

    editorToMgr[ed] = mgr;
    connect(mgr, &RGAManager::undoAvailableChanged, this, [this, ed](bool) {
        if (editor == ed)
            updateUndoRedoState();
    });
    connect(mgr, &RGAManager::redoAvailableChanged, this, [this, ed](bool) {
        if (editor == ed)
            updateUndoRedoState();
    });

    connect(mgr, &RGAManager::remoteTextChanged, ed, [ed](const QString &newText) {
        ed->applyRemoteText(newText);
    });

    connect(ed, &CodeEditor::cursorPositionUpdated, mgr, [mgr, ed](int, int) {
        mgr->sendCursorPosition(ed->SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS));
    });

    connect(mgr,
            &RGAManager::remoteCursorMoved,
            ed,
            &CodeEditor::updateRemoteCursor,
            Qt::UniqueConnection);

    connect(mgr,
            &RGAManager::remoteCursorLeft,
            ed,
            &CodeEditor::removeRemoteCursor,
            Qt::UniqueConnection);
}

void MainWindow::wireEditorToSession(CodeEditor *ed, const QString &relPath)
{
    RGAManager *mgr = session->getOrCreateRGA(relPath);
    wireEditorToManager(ed, mgr);

    const auto cached = session->fileCursors(relPath);
    for (auto it = cached.cbegin(); it != cached.cend(); ++it)
        ed->updateRemoteCursor(it.key(), it.value());

    for (auto it = peerNames.cbegin(); it != peerNames.cend(); ++it)
        ed->setRemotePeerName(it.key(), it.value());

    if (session->role() == CollabSession::Role::Guest
        && session->collabMode() == CollabSession::Mode::ReadOnly)
        ed->setReadOnly(true);
}

QString MainWindow::toSessionKey(const QString &editorPath) const
{
    if (!session || session->role() == CollabSession::Role::Guest)
        return editorPath;
    const QString root = session->projectRoot();
    return root.isEmpty() ? editorPath : QDir(root).relativeFilePath(editorPath);
}

QMap<QString, QString> MainWindow::collectCurrentFileTexts() const
{
    QMap<QString, QString> texts;
    if (!session)
        return texts;

    const QString root = session->projectRoot();

    for (const QTabWidget *tabs : allTabWidgets()) {
        for (int i = 0; i < tabs->count(); ++i) {
            const auto *ed = qobject_cast<const CodeEditor *>(tabs->widget(i));
            if (!ed)
                continue;
            const QString abs = ed->getFilePath();
            if (abs.isEmpty())
                continue;
            const QString rel = root.isEmpty() ? abs : QDir(root).relativeFilePath(abs);
            texts[rel] = ed->text();
        }
    }

    for (const QString &f : session->fileList())
        if (!texts.contains(f) && session->hasTextCache(f))
            texts[f] = session->cachedText(f);

    return texts;
}

void MainWindow::stopAllCollab()
{
    pendingEndCollab = false;
    if (!session)
        return;

    const bool isGuest = (session->role() == CollabSession::Role::Guest);

    if (isGuest) {
        for (QTabWidget *tabs : allTabWidgets()) {
            for (int i = tabs->count() - 1; i >= 0; --i) {
                auto *ed = qobject_cast<CodeEditor *>(tabs->widget(i));
                if (ed && ed->collabActive) {
                    tabs->removeTab(i);
                    ed->deleteLater();
                }
            }
        }
    } else {
        for (QTabWidget *tabs : allTabWidgets()) {
            for (int i = 0; i < tabs->count(); ++i) {
                auto *ed = qobject_cast<CodeEditor *>(tabs->widget(i));
                if (!ed)
                    continue;
                ed->clearRemoteCursors();
                ed->collabActive = false;
                markTabAsCollab(ed, false);
            }
        }
    }

    session->disconnectFromServer();
    session->deleteLater();
    session = nullptr;

    peerFiles.clear();
    peerNames.clear();
    peerAvatars.clear();
    peerRoles.clear();
    currentCollabFile.clear();
    refreshPresenceBar();

    if (collabUsersList)
        collabUsersList->clear();
    if (collabStatusLabel)
        collabStatusLabel->setText("Not in collab");
    if (collabNoSessionPane)
        collabNoSessionPane->show();
    if (collabInSessionPane)
        collabInSessionPane->hide();
    if (sessionTimer)
        sessionTimer->stop();
    if (sessionTimerLabel)
        sessionTimerLabel->setText("00:00:00");
    showToast("Collab session ended", "info");

    fileBrowser->clearRemoteMode();

    outputPane->appendPlainText("[Collab] Session stopped.");
}

void MainWindow::markTabAsCollab(CodeEditor *ed, bool on)
{
    for (QTabWidget *tabs : allTabWidgets()) {
        const int idx = tabs->indexOf(ed);
        if (idx >= 0) {
            QString name = QFileInfo(ed->getFilePath()).fileName();
            if (name.isEmpty())
                name = "Untitled";
            tabs->setTabText(idx, on ? (QString::fromUtf8("◎ ") + name) : name);
            return;
        }
    }
}

QWidget *MainWindow::makeCollabUserRow(int id,
                                       const QString &label,
                                       const QString &avatarUrl,
                                       const QString &role)
{
    auto *row = new QWidget;
    row->setStyleSheet("background: transparent;");
    auto *h = new QHBoxLayout(row);
    h->setContentsMargins(6, 3, 6, 3);
    h->setSpacing(8);

    const int size = 24;
    auto *avatar = new QLabel;
    avatar->setFixedSize(size, size);
    const QPixmap fallback = Avatar::letterPixmap(Avatar::initialFor(label),
                                                  Avatar::colorForId(QString::number(id)),
                                                  size);
    Avatar::load(avatar, avatarUrl, fallback, size, [avatar](QPixmap pix) {
        avatar->setPixmap(pix);
    });
    h->addWidget(avatar);

    auto *nameLbl = new QLabel(label);
    nameLbl->setStyleSheet("color: #d4d4d4; font-size: 12px;");
    h->addWidget(nameLbl, 1);

    if (!role.isEmpty()) {
        const QString label_ = (role == "host") ? "Host" : (role == "write" ? "Write" : "Read");
        auto *pill = new QLabel(label_);
        pill->setStyleSheet("background: #3c3c3c; color: #aaaaaa; border-radius: 8px; "
                            "padding: 1px 8px; font-size: 10px;");
        h->addWidget(pill);
    }

    return row;
}

void MainWindow::refreshCollabUsersList()
{
    if (!collabUsersList || !session)
        return;
    collabUsersList->clear();
    for (auto it = peerNames.cbegin(); it != peerNames.cend(); ++it) {
        const int id = it.key();
        const QString name = it.value().isEmpty() ? QString("user_%1").arg(id) : it.value();
        QString label = name;
        if (id == session->siteId())
            label += " (you)";
        const QString f = peerFiles.value(id);
        if (!f.isEmpty())
            label += QString("  [%1]").arg(QFileInfo(f).fileName());
        auto *item = new QListWidgetItem;
        item->setSizeHint(QSize(0, 32));
        item->setData(Qt::UserRole, id);
        collabUsersList->addItem(item);
        collabUsersList->setItemWidget(item,
                                       makeCollabUserRow(id,
                                                         label,
                                                         peerAvatars.value(id),
                                                         peerRoles.value(id)));
    }
}

void MainWindow::onCollabUsersUpdated(QMap<int, QString> users)
{
    const QMap<int, QString> prevNames = peerNames;
    peerNames = users;
    if (session)
        peerAvatars = session->avatars();

    for (auto it = users.cbegin(); it != users.cend(); ++it) {
        const int id = it.key();
        if (!peerRoles.contains(id)) {
            if (session && id == session->siteId() && session->role() == CollabSession::Role::Host)
                peerRoles[id] = "host";
            else
                peerRoles[id] = (session && session->collabMode() == CollabSession::Mode::ReadOnly)
                                    ? "read"
                                    : "write";
        }
    }
    for (auto it = peerRoles.begin(); it != peerRoles.end();)
        it = users.contains(it.key()) ? ++it : peerRoles.erase(it);
    for (auto it = peerFiles.begin(); it != peerFiles.end();)
        it = users.contains(it.key()) ? ++it : peerFiles.erase(it);

    refreshCollabUsersList();
    refreshPresenceBar();

    for (QTabWidget *tabs : allTabWidgets()) {
        for (int i = 0; i < tabs->count(); ++i) {
            if (auto *ed = qobject_cast<CodeEditor *>(tabs->widget(i)))
                for (auto it = users.cbegin(); it != users.cend(); ++it)
                    ed->setRemotePeerName(it.key(), it.value());
        }
    }
    if (collabStatusLabel && !users.isEmpty()) {
        const QString role = (session && session->role() == CollabSession::Role::Host) ? "Hosting"
                                                                                       : "Guest";
        collabStatusLabel->setText(QString("%1 — %2 user(s)").arg(role).arg(users.size()));
    }

    if (!prevNames.isEmpty()) {
        for (auto it = users.cbegin(); it != users.cend(); ++it) {
            if (!prevNames.contains(it.key()))
                showToast(it.value() + " joined", "success");
        }
        for (auto it = prevNames.cbegin(); it != prevNames.cend(); ++it) {
            if (!users.contains(it.key()))
                showToast(it.value() + " left", "info");
        }
    }
}

void MainWindow::onRemoteFileFocusChanged(int siteId, const QString &file)
{
    peerFiles[siteId] = file;
    refreshCollabUsersList();
    refreshPresenceBar();
}

void MainWindow::refreshPresenceBar()
{
    if (!mainToolBar)
        return;

    for (QAction *a : presenceActions)
        mainToolBar->removeAction(a);
    qDeleteAll(presenceActions);
    presenceActions.clear();

    if (!session || !session->isConnected() || currentCollabFile.isEmpty())
        return;

    QList<int> onFile;
    for (auto it = peerFiles.cbegin(); it != peerFiles.cend(); ++it)
        if (it.value() == currentCollabFile)
            onFile.append(it.key());

    if (onFile.isEmpty())
        return;

    constexpr int S = 26;
    constexpr int inner = 22;

    for (const int id : onFile) {
        const QString name = (id == session->siteId())
                                 ? (auth ? auth->currentUser().username : QString("You"))
                                 : peerNames.value(id, QString("user_%1").arg(id));
        const QString url = (id == session->siteId())
                                ? (auth ? auth->currentUser().avatarUrl : QString())
                                : peerAvatars.value(id);

        auto *lbl = new QLabel(mainToolBar);
        lbl->setFixedSize(S, S);
        lbl->setToolTip(name);
        lbl->setAlignment(Qt::AlignCenter);

        const QPixmap fallback = Avatar::letterPixmap(Avatar::initialFor(name),
                                                      Avatar::colorForId(QString::number(id)),
                                                      inner);
        lbl->setPixmap(Avatar::circularPixmap(fallback, S));

        QPointer<QLabel> ptr = lbl;
        Avatar::load(lbl, url, fallback, inner, [ptr](const QPixmap &pix) {
            if (ptr)
                ptr->setPixmap(Avatar::circularPixmap(pix, 26));
        });

        auto *act = new QWidgetAction(mainToolBar);
        act->setDefaultWidget(lbl);
        mainToolBar->addAction(act);
        presenceActions.append(act);
    }
}

void MainWindow::onTabCloseRequested(int tabIndex)
{
    QTabWidget *tabs = qobject_cast<QTabWidget *>(sender());
    if (!tabs)
        tabs = editorTabs;
    closeTabAt(tabs, tabIndex);
}

void MainWindow::setSidePanelPage(int index)
{
    const QStringList titles = {"EXPLORER", "COLLAB", "TEAM"};

    if (index == activeSidePanel && leftPanel->isVisible()) {
        leftPanel->setVisible(false);
        activeSidePanel = -1;
        btnFiles->setChecked(false);
        btnCollab->setChecked(false);
        btnTeam->setChecked(false);
        return;
    }

    activeSidePanel = index;
    leftPanel->setVisible(true);
    leftStack->setCurrentIndex(index);
    leftTitle->setText(titles.value(index, "PANEL"));

    btnFiles->setChecked(index == 0);
    btnCollab->setChecked(index == 1);
    btnTeam->setChecked(index == 2);
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
    centralSplitter->addWidget(editorStack);
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

    auto makeBtn = [](const QString &tip, const QString &iconName) {
        auto *btn = new QToolButton;
        btn->setToolTip(tip);
        btn->setCheckable(true);
        btn->setFixedSize(48, 48);
        btn->setObjectName("activityBtn");
        const QIcon ic = loadIconTransparent(iconName);
        if (!ic.isNull()) {
            btn->setIcon(ic);
            btn->setIconSize(QSize(24, 24));
            btn->setToolButtonStyle(Qt::ToolButtonIconOnly);
        }
        return btn;
    };

    btnFiles = makeBtn("Explorer  (Ctrl+B)", "files.png");
    btnCollab = makeBtn("Collaboration", "partnership.png");
    btnTeam = makeBtn("Team", "team.png");

    vbox->addWidget(btnFiles);
    vbox->addWidget(btnCollab);
    vbox->addWidget(btnTeam);
    vbox->addStretch(1);

    btnProfile = new QToolButton;
    btnProfile->setToolTip("Sign in");
    btnProfile->setCheckable(false);
    btnProfile->setFixedSize(48, 48);
    btnProfile->setObjectName("activityBtnProfile");
    {
        const QIcon userIc = loadIconTransparent("user.png");
        if (!userIc.isNull()) {
            btnProfile->setIcon(userIc);
            btnProfile->setIconSize(QSize(24, 24));
            btnProfile->setToolButtonStyle(Qt::ToolButtonIconOnly);
        } else {
            btnProfile->setText("User");
        }
    }
    vbox->addWidget(btnProfile);

    auto requireAuth = [this](const QString &feature) {
        if (auth && auth->isLoggedIn())
            return true;
        QMessageBox::information(this, "Sign in required", "Sign in to use " + feature + ".");
        return false;
    };

    connect(btnFiles, &QToolButton::clicked, this, [this] { onActivityButton(0); });
    connect(btnCollab, &QToolButton::clicked, this, [this, requireAuth] {
        if (!requireAuth("collaboration")) {
            btnCollab->setChecked(false);
            return;
        }
        onActivityButton(1);
    });
    connect(btnTeam, &QToolButton::clicked, this, [this, requireAuth] {
        if (!requireAuth("the team panel")) {
            btnTeam->setChecked(false);
            return;
        }
        onActivityButton(2);
    });
    connect(btnProfile, &QToolButton::clicked, this, [this] {
        if (!auth || !auth->isLoggedIn()) {
            AuthDialog dlg(auth, this);
            if (dlg.exec() == QDialog::Accepted)
                updateProfileButton();
            return;
        }
        QMenu menu(this);
        const QString uname = auth->currentUser().username;
        menu.addAction(uname, this, [this] { openProfileDialog(); });
        menu.addSeparator();
        menu.addAction("Sign out", this, [this] { auth->logout(); });
        menu.exec(btnProfile->mapToGlobal(btnProfile->rect().topRight()));
    });
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
    leftStack->addWidget(fileBrowser);

    teamsPanel = new TeamsPanel;

    vbox->addWidget(leftStack, 1);
    auto *collabPane = new QWidget;
    auto *vl = new QVBoxLayout(collabPane);
    vl->setContentsMargins(4, 4, 4, 4);
    vl->setSpacing(6);

    collabStatusLabel = new QLabel("Not in collab");
    collabStatusLabel->setObjectName("stubLabel");
    collabStatusLabel->setWordWrap(true);
    vl->addWidget(collabStatusLabel);

    collabNoSessionPane = new QWidget;
    auto *noVl = new QVBoxLayout(collabNoSessionPane);
    noVl->setContentsMargins(0, 4, 0, 0);
    noVl->setSpacing(4);

    btnStartCollab = new QPushButton("Start Collab");
    btnStartCollab->setObjectName("primaryBtn");
    connect(btnStartCollab, &QPushButton::clicked, this, [this]() { showStartCollabDialog(); });
    noVl->addWidget(btnStartCollab);

    btnJoinCollab = new QPushButton("Join Collab");
    btnJoinCollab->setObjectName("primaryBtn");
    connect(btnJoinCollab, &QPushButton::clicked, this, &MainWindow::joinCollab);
    noVl->addWidget(btnJoinCollab);

    noVl->addStretch(1);
    vl->addWidget(collabNoSessionPane, 1);

    collabInSessionPane = new QWidget;
    auto *inVl = new QVBoxLayout(collabInSessionPane);
    inVl->setContentsMargins(0, 4, 0, 0);
    inVl->setSpacing(4);

    auto *usersLbl = new QLabel("PARTICIPANTS");
    usersLbl->setObjectName("stubLabel");
    inVl->addWidget(usersLbl);

    collabUsersList = new QListWidget;
    collabUsersList->setObjectName("taskList");
    collabUsersList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(collabUsersList,
            &QListWidget::customContextMenuRequested,
            this,
            &MainWindow::onCollabUserContextMenu);
    inVl->addWidget(collabUsersList, 1);

    sessionTimerLabel = new QLabel("00:00:00");
    sessionTimerLabel->setObjectName("stubLabel");
    sessionTimerLabel->setAlignment(Qt::AlignCenter);
    inVl->addWidget(sessionTimerLabel);

    btnSessionReport = new QPushButton("Session Report");
    btnSessionReport->setObjectName("primaryBtn");
    connect(btnSessionReport, &QPushButton::clicked, this, [this]() {
        if (!session)
            return;
        session->sendFinalStates(collectCurrentFileTexts());
        session->requestSessionReport();
    });
    inVl->addWidget(btnSessionReport);

    btnStopAllCollab = new QPushButton("End Collab");
    btnStopAllCollab->setObjectName("dangerBtn");
    connect(btnStopAllCollab, &QPushButton::clicked, this, &MainWindow::onEndCollabRequested);
    inVl->addWidget(btnStopAllCollab);

    collabInSessionPane->hide();
    vl->addWidget(collabInSessionPane, 1);

    leftStack->addWidget(collabPane);
    leftStack->addWidget(teamsPanel);

    connect(fileBrowser, &FileBrowser::fileDoubleClicked, this, &MainWindow::openFileFromBrowser);
    connect(fileBrowser, &FileBrowser::openFolderRequested, this, &MainWindow::openFolder);
    connect(fileBrowser, &FileBrowser::cloneRepoRequested, this, &MainWindow::cloneRepo);
}

void MainWindow::openFileFromBrowser(const QString &path)
{
    QTabWidget *target = focusedTabs ? focusedTabs : editorTabs;

    for (int i = 0; i < target->count(); i++) {
        CodeEditor *ed = qobject_cast<CodeEditor *>(target->widget(i));
        if (ed && ed->getFilePath() == path) {
            target->setCurrentIndex(i);
            return;
        }
    }

    CodeEditor *newEditor = new CodeEditor(target);

    const bool isGuest = session && session->role() == CollabSession::Role::Guest;

    if (isGuest) {
        newEditor->setFilePath(path);
        const QString cached = session->cachedText(path);
        if (!cached.isEmpty())
            newEditor->applyRemoteText(cached);
    } else if (session) {
        const QString relPath = toSessionKey(path);
        newEditor->setFilePath(path);
        const QString cached = session->cachedText(relPath);
        if (!cached.isEmpty())
            newEditor->applyRemoteText(cached);
        else
            newEditor->loadFile(path);
    } else {
        newEditor->loadFile(path);
    }

    connect(newEditor,
            &CodeEditor::cursorPositionUpdated,
            this,
            &MainWindow::onCursorPositionUpdated);
    connect(newEditor, &CodeEditor::modifyChanged, this, &MainWindow::onModificationChanged);
    connect(newEditor, &CodeEditor::fileModified, this, [this, newEditor] {
        if (editor == newEditor)
            updateUndoRedoState();
    });

    auto syncSiblings = [this, newEditor]() {
        const QString filePath = newEditor->getFilePath();
        if (filePath.isEmpty())
            return;
        const QString text = newEditor->text();
        for (QTabWidget *tabs : allTabWidgets()) {
            for (int i = 0; i < tabs->count(); i++) {
                auto *sibling = qobject_cast<CodeEditor *>(tabs->widget(i));
                if (sibling && sibling != newEditor && sibling->getFilePath() == filePath)
                    sibling->applyRemoteText(text);
            }
        }
    };
    connect(newEditor, &CodeEditor::localInsert, this, [syncSiblings](int, QChar) {
        syncSiblings();
    });
    connect(newEditor, &CodeEditor::localDelete, this, [syncSiblings](int) { syncSiblings(); });

    const QString name = QFileInfo(path).fileName();
    const int tabIdx = target->addTab(newEditor, name);

    target->setCurrentIndex(tabIdx);
    updateWelcomeVisibility();
    updateRunCombo();

    if (session) {
        const QString relPath = toSessionKey(path);
        if (!isGuest && !session->hasTextCache(relPath))
            session->initFileCache(relPath, newEditor->text());

        wireEditorToSession(newEditor, relPath);

        if (!isGuest) {
            RGAManager *mgr = session->getOrCreateRGA(relPath);
            if (mgr->getText().isEmpty()) {
                mgr->buildFromText(newEditor->text());
                mgr->sendInitText(newEditor->text(), relPath);
            }
        }
        markTabAsCollab(newEditor, true);
    }

    currentFilePath = path;
    updateWindowTitle();
    if (!isGuest)
        outputPane->appendPlainText("[TeamHub] Opened: " + path);
}

void MainWindow::setupEditorArea()
{
    editorSplitter = new QSplitter(Qt::Horizontal);
    editorSplitter->setObjectName("editorSplitter");
    editorSplitter->setChildrenCollapsible(false);

    editorTabs = new QTabWidget;
    editorTabs->setObjectName("editorTabs");
    editorTabs->setTabsClosable(true);
    editorTabs->setMovable(true);
    editorTabs->setDocumentMode(true);
    editor = nullptr;
    connect(editorTabs, &QTabWidget::tabCloseRequested, this, &MainWindow::onTabCloseRequested);
    connect(editorTabs, &QTabWidget::currentChanged, this, &MainWindow::onTabChanged);
    editorTabs->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(editorTabs->tabBar(),
            &QTabBar::customContextMenuRequested,
            this,
            [this](const QPoint &pos) { showTabContextMenu(editorTabs, pos); });

    editorTabs2 = new QTabWidget;
    editorTabs2->setObjectName("editorTabs");
    editorTabs2->setTabsClosable(true);
    editorTabs2->setMovable(true);
    editorTabs2->setDocumentMode(true);
    editorTabs2->hide();
    connect(editorTabs2, &QTabWidget::tabCloseRequested, this, &MainWindow::onTabCloseRequested);
    connect(editorTabs2, &QTabWidget::currentChanged, this, &MainWindow::onTabChanged);
    editorTabs2->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(editorTabs2->tabBar(),
            &QTabBar::customContextMenuRequested,
            this,
            [this](const QPoint &pos) { showTabContextMenu(editorTabs2, pos); });

    editorSplitter->addWidget(editorTabs);
    editorSplitter->addWidget(editorTabs2);
    editorSplitter->setStretchFactor(0, 1);
    editorSplitter->setStretchFactor(1, 1);

    focusedTabs = editorTabs;

    welcomeWidget = new QWidget;
    welcomeWidget->setObjectName("welcomeWidget");
    auto *wl = new QVBoxLayout(welcomeWidget);
    wl->setContentsMargins(0, 0, 0, 0);
    wl->setSpacing(0);
    wl->setAlignment(Qt::AlignCenter);

    auto *centerBox = new QWidget;
    centerBox->setFixedWidth(420);
    auto *cl = new QVBoxLayout(centerBox);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(0);

    auto *titleLabel = new QLabel("TeamHub");
    titleLabel->setObjectName("welcomeTitle");
    titleLabel->setAlignment(Qt::AlignCenter);
    cl->addWidget(titleLabel);

    auto *subtitleLabel = new QLabel("Collaborative Code Editor");
    subtitleLabel->setObjectName("welcomeSubtitle");
    subtitleLabel->setAlignment(Qt::AlignCenter);
    cl->addWidget(subtitleLabel);

    cl->addSpacing(40);

    auto *recentHeader = new QLabel("RECENT PROJECTS");
    recentHeader->setObjectName("welcomeSectionHeader");
    recentHeader->setAlignment(Qt::AlignLeft);
    cl->addWidget(recentHeader);
    cl->addSpacing(10);

    auto *recentContainer = new QWidget;
    recentProjectsLayout = new QVBoxLayout(recentContainer);
    recentProjectsLayout->setContentsMargins(0, 0, 0, 0);
    recentProjectsLayout->setSpacing(1);
    cl->addWidget(recentContainer);

    cl->addSpacing(32);

    auto *btnOpenFile = new QPushButton("Open File...");
    btnOpenFile->setObjectName("welcomeOpenFile");
    btnOpenFile->setCursor(Qt::PointingHandCursor);
    btnOpenFile->setFixedWidth(160);
    connect(btnOpenFile, &QPushButton::clicked, this, &MainWindow::openFile);
    cl->addWidget(btnOpenFile, 0, Qt::AlignCenter);

    cl->addSpacing(40);

    const QString shortcutRow = "<table cellspacing='0' cellpadding='0' align='center'>"
                                "<tr>"
                                "<td align='right'><code style='color:#569cd6'>Ctrl+O</code></td>"
                                "<td width='8'></td>"
                                "<td><span style='color:#858585'>Open File</span></td>"
                                "<td width='28'></td>"
                                "<td align='right'><code style='color:#569cd6'>Ctrl+B</code></td>"
                                "<td width='8'></td>"
                                "<td><span style='color:#858585'>Toggle Panel</span></td>"
                                "</tr>"
                                "<tr><td height='5'></td></tr>"
                                "<tr>"
                                "<td align='right'><code style='color:#569cd6'>Ctrl+\\</code></td>"
                                "<td width='8'></td>"
                                "<td><span style='color:#858585'>Split Editor</span></td>"
                                "<td width='28'></td>"
                                "<td align='right'><code style='color:#569cd6'>Ctrl+J</code></td>"
                                "<td width='8'></td>"
                                "<td><span style='color:#858585'>Toggle Output</span></td>"
                                "</tr>"
                                "</table>";
    auto *shortcutsLabel = new QLabel(shortcutRow);
    shortcutsLabel->setObjectName("welcomeShortcuts");
    shortcutsLabel->setTextFormat(Qt::RichText);
    shortcutsLabel->setAlignment(Qt::AlignCenter);
    cl->addWidget(shortcutsLabel, 0, Qt::AlignCenter);

    wl->addWidget(centerBox, 0, Qt::AlignCenter);

    editorStack = new QStackedWidget;
    editorStack->addWidget(welcomeWidget);
    editorStack->addWidget(editorSplitter);
    editorStack->setCurrentIndex(0);

    refreshWelcomeRecent();
}

void MainWindow::setupBottomDock()
{
    bottomDock = new QDockWidget("Panel", this);
    bottomDock->setObjectName("bottomDock");
    bottomDock->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable);
    bottomDock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
    auto *dockTitleBar = new QWidget;
    auto *dockTitleLayout = new QHBoxLayout(dockTitleBar);
    dockTitleLayout->setContentsMargins(0, 0, 4, 0);
    dockTitleLayout->setSpacing(0);
    dockTitleLayout->addStretch();
    auto *dockCloseBtn = new QToolButton(dockTitleBar);
    dockCloseBtn->setText(QString(QChar(0x00D7)));
    dockCloseBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    dockCloseBtn->setToolTip("Close Panel");
    dockCloseBtn->setStyleSheet(
        "QToolButton { color: rgba(255,255,255,0.5); font-size:16px; border:none;"
        "              background:transparent; padding:0 4px; }"
        "QToolButton:hover { color: white; }");
    connect(dockCloseBtn, &QToolButton::clicked, this, [this] {
        if (terminal)
            terminal->killAll();
        bottomDock->hide();
    });
    dockTitleLayout->addWidget(dockCloseBtn);
    bottomDock->setTitleBarWidget(dockTitleBar);

    bottomTabs = new QTabWidget;
    bottomTabs->setObjectName("bottomTabs");
    bottomTabs->setTabPosition(QTabWidget::South);

    outputPane = new QPlainTextEdit;
    outputPane->setObjectName("outputPane");
    outputPane->setReadOnly(true);
    outputPane->setPlaceholderText("Build and run output will appear here...");
    outputPane->installEventFilter(this);
    bottomTabs->addTab(outputPane, "Output");

    terminal = new Terminal(this);
    terminal->setObjectName("terminal");
    terminal->setWorkingDirectory(fileBrowser->rootPath());
    bottomTabs->addTab(terminal, "Terminal");

    connect(bottomTabs, &QTabWidget::currentChanged, this, [this](int idx) {
        if (bottomTabs->widget(idx) == terminal && terminal->terminalCount() == 0)
            terminal->addTerminal();
    });

    auto *actTerminal = new QAction(this);
    actTerminal->setShortcut(QKeySequence(Qt::ALT | Qt::Key_F12));
    addAction(actTerminal);
    connect(actTerminal, &QAction::triggered, this, [this] {
        if (terminal->terminalCount() == 0)
            terminal->addTerminal();
        bottomTabs->setCurrentWidget(terminal);
        bottomDock->setVisible(true);
        terminal->focusCurrent();
    });

    gitPanel_ = new GitPanel(this);
    gitPane = gitPanel_;
    connect(gitPanel_, &GitPanel::logMessage, this, [this](const QString &msg) {
        outputPane->appendPlainText(msg);
    });
    connect(gitPanel_, &GitPanel::diffRequested, this, &MainWindow::openDiffTab);
    bottomTabs->addTab(gitPane, "Git");

    setupDebugPanel();

    bottomDock->setWidget(bottomTabs);
    addDockWidget(Qt::BottomDockWidgetArea, bottomDock);
    resizeDocks({bottomDock}, {180}, Qt::Vertical);
}

void MainWindow::setupDebugPanel()
{
    debugPane = new QWidget;
    debugPane->setObjectName("debugPane");

    auto *root = new QVBoxLayout(debugPane);
    root->setContentsMargins(0, 2, 0, 0);
    root->setSpacing(2);

    auto *dbgBar = new QToolBar;
    dbgBar->setMovable(false);
    {
        const QString s = loadStyle("debugbar.qss");
        if (!s.isEmpty())
            dbgBar->setStyleSheet(s);
    }

    actDebugContinue = dbgBar->addAction(u8"▶  Continue", this, [this] {
        if (debugAdapter)
            debugAdapter->continueExec();
    });
    actDebugContinue->setShortcut(QKeySequence("F5"));
    actDebugContinue->setToolTip("Continue (F5)");

    actDebugStepOver = dbgBar->addAction(u8"↪  Step Over", this, [this] {
        if (debugAdapter)
            debugAdapter->stepOver();
    });
    actDebugStepOver->setShortcut(QKeySequence("F10"));
    actDebugStepOver->setToolTip("Step Over (F10)");

    actDebugStepIn = dbgBar->addAction(u8"↓  Step Into", this, [this] {
        if (debugAdapter)
            debugAdapter->stepIn();
    });
    actDebugStepIn->setShortcut(QKeySequence("F11"));
    actDebugStepIn->setToolTip("Step Into (F11)");

    actDebugStepOut = dbgBar->addAction(u8"↑  Step Out", this, [this] {
        if (debugAdapter)
            debugAdapter->stepOut();
    });
    actDebugStepOut->setShortcut(QKeySequence("Shift+F11"));
    actDebugStepOut->setToolTip("Step Out (Shift+F11)");

    dbgBar->addSeparator();

    actDebugStop = dbgBar->addAction(QString(QChar(0x25A0)) + "  Stop",
                                     this,
                                     &MainWindow::stopDebugging);
    actDebugStop->setShortcut(QKeySequence("Shift+F9"));
    actDebugStop->setToolTip("Stop Debugging (Shift+F9)");

    for (auto *a :
         {actDebugContinue, actDebugStepOver, actDebugStepIn, actDebugStepOut, actDebugStop})
        a->setEnabled(false);

    if (auto *btn = qobject_cast<QToolButton *>(dbgBar->widgetForAction(actDebugContinue)))
        btn->setStyleSheet("QToolButton { color: #4ec9b0; } "
                           "QToolButton:hover { color: #6fdfc8; } "
                           "QToolButton:disabled { color: #2a5a52; }");
    if (auto *btn = qobject_cast<QToolButton *>(dbgBar->widgetForAction(actDebugStop)))
        btn->setStyleSheet("QToolButton { color: #f48771; } "
                           "QToolButton:hover { color: #ff9f8f; } "
                           "QToolButton:disabled { color: #5a2a22; }");

    root->addWidget(dbgBar);

    auto *splitter = new QSplitter(Qt::Horizontal);

    debugCallStack = new QTreeWidget;
    debugCallStack->setObjectName("debugCallStack");
    debugCallStack->setHeaderLabel("Call Stack");
    debugCallStack->setRootIsDecorated(false);
    connect(debugCallStack, &QTreeWidget::itemClicked, this, &MainWindow::onDebugCallStackClicked);
    splitter->addWidget(debugCallStack);

    debugVariables = new QTreeWidget;
    debugVariables->setObjectName("debugVariables");
    debugVariables->setColumnCount(2);
    debugVariables->setHeaderLabels({"Name", "Value"});
    debugVariables->setRootIsDecorated(true);
    connect(debugVariables, &QTreeWidget::itemExpanded, this, &MainWindow::onDebugVariableExpanded);
    splitter->addWidget(debugVariables);

    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);

    root->addWidget(splitter, 1);

    bottomTabs->addTab(debugPane, "Debug");
}

void MainWindow::startDebugging()
{
    if (debugAdapter && debugAdapter->isRunning()) {
        stopDebugging();
        return;
    }

    CodeEditor *ed = qobject_cast<CodeEditor *>(
        (focusedTabs ? focusedTabs : editorTabs)->currentWidget());
    if (!ed)
        return;

    const QString path = ed->getFilePath();
    if (path.isEmpty()) {
        outputPane->appendPlainText("[Debug] Save the file before debugging.");
        bottomTabs->setCurrentWidget(outputPane);
        return;
    }

    if (!path.endsWith(".py", Qt::CaseInsensitive)) {
        outputPane->appendPlainText("[Debug] Debugger supports Python (.py) files only.");
        bottomTabs->setCurrentWidget(outputPane);
        return;
    }

    delete debugAdapter;
    debugAdapter = new DebugAdapter(this);

    connect(debugAdapter, &DebugAdapter::logMessage, this, [this](const QString &txt) {
        outputPane->appendPlainText(txt.trimmed());
    });
    connect(debugAdapter, &DebugAdapter::stopped, this, &MainWindow::onDebugStopped);
    connect(debugAdapter, &DebugAdapter::continued, this, &MainWindow::onDebugContinued);
    connect(debugAdapter, &DebugAdapter::terminated, this, &MainWindow::onDebugTerminated);
    connect(debugAdapter, &DebugAdapter::callStackReady, this, &MainWindow::onDebugCallStackReady);
    connect(debugAdapter, &DebugAdapter::variablesReady, this, &MainWindow::onDebugVariablesReady);
    connect(debugAdapter,
            &DebugAdapter::subVariablesReady,
            this,
            &MainWindow::onDebugSubVariablesReady);
    connect(ed, &CodeEditor::breakpointsChanged, debugAdapter, &DebugAdapter::updateBreakpoints);

    for (auto *a :
         {actDebugContinue, actDebugStepOver, actDebugStepIn, actDebugStepOut, actDebugStop})
        a->setEnabled(false);

    actDebugStop->setEnabled(true);
    actDebugMain->setText("Stop Debug");

    outputPane->appendPlainText(QString("[Debug] Starting: %1").arg(path));
    outputPane->appendPlainText("[Debug] Install debugpy if missing:  pip install debugpy");
    bottomTabs->setCurrentWidget(debugPane);
    bottomDock->setVisible(true);

    debugAdapter->startDebugging(path, ed->breakpoints());
}

void MainWindow::stopDebugging()
{
    if (debugAdapter)
        debugAdapter->stop();
}

void MainWindow::onDebugStopped(const QString &filePath, int line, const QString &)
{
    clearDebugHighlights();

    auto normPath = [](const QString &p) {
        return QDir::cleanPath(QDir::fromNativeSeparators(p)).toLower();
    };
    const QString stoppedNorm = normPath(filePath);

    bool debugFound = false;
    for (QTabWidget *tabs : allTabWidgets()) {
        if (debugFound)
            break;
        for (int i = 0; i < tabs->count(); ++i) {
            auto *ed = qobject_cast<CodeEditor *>(tabs->widget(i));
            if (!ed)
                continue;
            if (normPath(ed->getFilePath()) == stoppedNorm) {
                tabs->setCurrentIndex(i);
                focusedTabs = tabs;
                ed->setDebugLine(line);
                debugFound = true;
                break;
            }
        }
    }

    for (auto *a : {actDebugContinue, actDebugStepOver, actDebugStepIn, actDebugStepOut})
        a->setEnabled(true);

    bottomTabs->setCurrentWidget(debugPane);
}

void MainWindow::onDebugContinued()
{
    clearDebugHighlights();
    for (auto *a : {actDebugContinue, actDebugStepOver, actDebugStepIn, actDebugStepOut})
        a->setEnabled(false);
}

void MainWindow::onDebugTerminated()
{
    clearDebugHighlights();
    for (auto *a :
         {actDebugContinue, actDebugStepOver, actDebugStepIn, actDebugStepOut, actDebugStop})
        a->setEnabled(false);

    actDebugMain->setText("Debug");
    debugCallStack->clear();
    debugVariables->clear();
    outputPane->appendPlainText("[Debug] Session ended.");
    bottomTabs->setCurrentWidget(outputPane);
}

void MainWindow::onDebugCallStackReady(const QList<DebugAdapter::FrameInfo> &frames)
{
    debugCallStack->clear();
    for (const auto &f : frames) {
        auto *item = new QTreeWidgetItem(debugCallStack, {f.label});
        item->setData(0, Qt::UserRole, f.frameId);
    }
}

void MainWindow::onDebugCallStackClicked(QTreeWidgetItem *item, int)
{
    if (!debugAdapter || !debugAdapter->isRunning())
        return;
    const QVariant d = item->data(0, Qt::UserRole);
    if (!d.isValid())
        return;
    const int frameId = d.toInt();
    if (frameId >= 0)
        debugAdapter->requestFrameVariables(frameId);
}

static QTreeWidgetItem *dbgFindByRef(QTreeWidgetItem *parent, int ref)
{
    for (int i = 0; i < parent->childCount(); ++i) {
        auto *it = parent->child(i);
        if (it->data(0, Qt::UserRole).toInt() == ref)
            return it;
        auto *found = dbgFindByRef(it, ref);
        if (found)
            return found;
    }
    return nullptr;
}

static QTreeWidgetItem *dbgMakeVarItem(const DebugAdapter::Var &v, QTreeWidgetItem *parent)
{
    auto *item = new QTreeWidgetItem(parent, {v.name, v.value});
    item->setData(0, Qt::UserRole, v.ref);
    if (v.ref > 0)
        new QTreeWidgetItem(item);
    return item;
}

void MainWindow::onDebugVariablesReady(const QList<DebugAdapter::Var> &vars)
{
    debugVariables->clear();

    static const QStringList kCollapsed = {"special variables",
                                           "function variables",
                                           "protected variables",
                                           "class variables"};

    for (const auto &v : vars) {
        auto *item = dbgMakeVarItem(v, debugVariables->invisibleRootItem());
        debugVariables->addTopLevelItem(item);
        if (kCollapsed.contains(v.name.toLower()))
            item->setExpanded(false);
    }
    debugVariables->resizeColumnToContents(0);
}

void MainWindow::onDebugSubVariablesReady(int parentRef, const QList<DebugAdapter::Var> &vars)
{
    auto *parent = dbgFindByRef(debugVariables->invisibleRootItem(), parentRef);
    if (!parent)
        return;

    qDeleteAll(parent->takeChildren());

    for (const auto &v : vars)
        dbgMakeVarItem(v, parent);

    debugVariables->resizeColumnToContents(0);
}

void MainWindow::onDebugVariableExpanded(QTreeWidgetItem *item)
{
    const int ref = item->data(0, Qt::UserRole).toInt();
    if (ref <= 0)
        return;

    if (item->childCount() == 1 && item->child(0)->text(0).isEmpty()) {
        if (debugAdapter)
            debugAdapter->requestSubVariables(ref);
    }
}

void MainWindow::clearDebugHighlights()
{
    for (QTabWidget *tabs : allTabWidgets()) {
        for (int i = 0; i < tabs->count(); ++i) {
            auto *ed = qobject_cast<CodeEditor *>(tabs->widget(i));
            if (ed)
                ed->clearDebugLine();
        }
    }
}

void MainWindow::openDiffTab(const QString &relPath, bool staged)
{
    if (!gitPanel_)
        return;

    const QString workdir = gitPanel_->manager()->workdir();
    const QString absPath = QDir::cleanPath(workdir + relPath);
    const QString tabName = QFileInfo(relPath).fileName() + " (diff)";

    auto applyDiff = [&](CodeEditor *ed) {
        const QString raw = staged ? gitPanel_->manager()->diffStaged(relPath)
                                   : gitPanel_->manager()->diffUnstaged(relPath);
        ed->applyDiffText(raw);
    };

    for (int i = 0; i < editorTabs->count(); ++i) {
        auto *ed = qobject_cast<CodeEditor *>(editorTabs->widget(i));
        if (ed && ed->property("diffPath").toString() == absPath
            && ed->property("diffStaged").toBool() == staged) {
            applyDiff(ed);
            editorTabs->setCurrentIndex(i);
            return;
        }
    }

    CodeEditor *ed = new CodeEditor(editorTabs);
    ed->setProperty("diffPath", absPath);
    ed->setProperty("diffStaged", staged);
    applyDiff(ed);

    const int idx = editorTabs->addTab(ed, tabName);

    editorTabs->setCurrentIndex(idx);
}

void MainWindow::setupStatusBar()
{
    statusFile = new QLabel("Untitled");
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

void MainWindow::updateWelcomeVisibility()
{
    const bool empty = (editorTabs->count() == 0 && !splitActive);
    if (empty)
        refreshWelcomeRecent();
    editorStack->setCurrentIndex(empty ? 0 : 1);
}

void MainWindow::refreshWelcomeRecent()
{
    while (recentProjectsLayout->count())
        delete recentProjectsLayout->takeAt(0)->widget();

    const auto projects = ProjectDB::instance().recentProjects(8);
    if (projects.isEmpty()) {
        auto *lbl = new QLabel("No recent projects.");
        lbl->setObjectName("stubLabel");
        recentProjectsLayout->addWidget(lbl);
        return;
    }
    const QIcon folderIc = QApplication::style()->standardIcon(QStyle::SP_DirIcon);

    for (const auto &p : projects) {
        auto *row = new QFrame;
        row->setObjectName("welcomeRecentRow");
        row->setCursor(Qt::PointingHandCursor);
        row->setProperty("recentPath", p.path);

        auto *rl = new QHBoxLayout(row);
        rl->setContentsMargins(12, 8, 12, 8);
        rl->setSpacing(12);

        auto *iconLbl = new QLabel;
        iconLbl->setPixmap(folderIc.pixmap(18, 18));
        iconLbl->setFixedSize(18, 18);
        rl->addWidget(iconLbl);

        auto *textCol = new QVBoxLayout;
        textCol->setContentsMargins(0, 0, 0, 0);
        textCol->setSpacing(2);

        auto *nameLbl = new QLabel(p.name);
        nameLbl->setObjectName("welcomeRecentName");

        auto *pathLbl = new QLabel(p.path);
        pathLbl->setObjectName("welcomeRecentPath");
        pathLbl->setMaximumWidth(370);

        textCol->addWidget(nameLbl);
        textCol->addWidget(pathLbl);
        rl->addLayout(textCol, 1);

        row->installEventFilter(this);
        recentProjectsLayout->addWidget(row);
    }
}

void MainWindow::showToast(const QString &msg, const QString &type)
{
    static const int kW = 300, kH = 38, kMargin = 16, kGap = 6;

    auto *toast = new QFrame(this);
    toast->setFixedSize(kW, kH);
    toast->setAttribute(Qt::WA_DeleteOnClose);
    toast->setStyleSheet("QFrame { background:#2d2d30; border:none; border-radius:4px; }"
                         "QLabel { color:#cccccc; font-size:12px; background:transparent; }");

    const QString accentColor = (type == "success")   ? "#4ec9b0"
                                : (type == "warning") ? "#e2c08d"
                                : (type == "error")   ? "#f14c4c"
                                                      : "#007acc";

    auto *hl = new QHBoxLayout(toast);
    hl->setContentsMargins(0, 0, 12, 0);
    hl->setSpacing(10);

    auto *bar = new QWidget(toast);
    bar->setFixedWidth(3);
    bar->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    bar->setStyleSheet(QString("background:%1; border-radius:2px;").arg(accentColor));
    hl->addWidget(bar);

    auto *lbl = new QLabel(msg, toast);
    lbl->setWordWrap(false);
    hl->addWidget(lbl, 1);

    int yOffset = 0;
    for (auto *t : activeToasts)
        yOffset += t->height() + kGap;
    toast->move(width() - kW - kMargin, height() - kH - 36 - yOffset);
    toast->raise();
    toast->show();
    activeToasts.append(toast);

    QTimer::singleShot(3500, this, [this, toast]() {
        activeToasts.removeOne(toast);
        toast->close();
    });
}

void MainWindow::updateUndoRedoState()
{
    bool canUndo = false, canRedo = false;
    if (editor) {
        if (editor->collabActive) {
            RGAManager *mgr = editorToMgr.value(editor, nullptr);
            if (mgr) {
                canUndo = mgr->canUndo();
                canRedo = mgr->canRedo();
            }
        } else {
            canUndo = editor->isUndoAvailable();
            canRedo = editor->isRedoAvailable();
        }
    }
    if (actUndo)
        actUndo->setEnabled(canUndo);
    if (actRedo)
        actRedo->setEnabled(canRedo);
}

void MainWindow::onTabChanged(int index)
{
    QTabWidget *sourceTabs = qobject_cast<QTabWidget *>(sender());
    if (!sourceTabs)
        sourceTabs = editorTabs;
    focusedTabs = sourceTabs;
    CodeEditor *activeEditor = qobject_cast<CodeEditor *>(sourceTabs->widget(index));
    if (!activeEditor) {
        editor = nullptr;
        currentFilePath.clear();
        statusFile->setText("");
        updateWindowTitle();
        return;
    }
    editor = activeEditor;
    QString path = activeEditor->getFilePath();
    currentFilePath = path;
    QString filename;
    if (path.isEmpty()) {
        filename = "Untitled";
        statusFile->setText(filename);
    } else {
        filename = QFileInfo(path).fileName();
    }
    statusFile->setText(filename);
    statusPosition->setText(QString("Ln %1, Col %2")
                                .arg(activeEditor->currentLine() + 1)
                                .arg(activeEditor->currentColumn() + 1));

    static const QMap<QString, QString> extToLang = {
        {"py", "Python"},
        {"cpp", "C++"},
        {"cxx", "C++"},
        {"cc", "C++"},
        {"c", "C"},
        {"h", "C/C++ Header"},
        {"hpp", "C++ Header"},
        {"js", "JavaScript"},
        {"ts", "TypeScript"},
        {"json", "JSON"},
        {"md", "Markdown"},
        {"txt", "Plain Text"},
        {"pro", "Qt Project"},
        {"qml", "QML"},
        {"xml", "XML"},
        {"html", "HTML"},
        {"css", "CSS"},
    };
    const QString suffix = QFileInfo(path).suffix().toLower();
    statusLanguage->setText(
        extToLang.value(suffix, suffix.isEmpty() ? "Plain Text" : suffix.toUpper()));

    updateUndoRedoState();
    updateWindowTitle();

    if (session && session->isConnected()) {
        if (!currentCollabFile.isEmpty())
            session->sendCursorLeave(currentCollabFile);

        const QString relPath = toSessionKey(path);
        if (!path.isEmpty()) {
            currentCollabFile = relPath;
            session->sendFileFocus(relPath);
            peerFiles[session->siteId()] = relPath;
            refreshCollabUsersList();
            refreshPresenceBar();
        } else {
            currentCollabFile.clear();
            refreshPresenceBar();
        }
    }
}

void MainWindow::applyTheme()
{
    const QString style = loadStyle("dark.qss");
    if (!style.isEmpty())
        setStyleSheet(style);
}

void MainWindow::updateWindowTitle()
{
    const QString name = currentFilePath.isEmpty() ? (editor ? "Untitled" : "TeamHub")
                                                   : QFileInfo(currentFilePath).fileName();
    const QString dirty = (editor && editor->isModified()) ? " \u25cf" : "";
    setWindowTitle(name + dirty);
}

void MainWindow::onActivityButton(int page)
{
    setSidePanelPage(page);
}

void MainWindow::onCursorPositionUpdated(int line, int index)
{
    statusPosition->setText(QString("Ln %1, Col %2").arg(line + 1).arg(index + 1));
}

void MainWindow::onModificationChanged(bool modified)
{
    const QString name = currentFilePath.isEmpty() ? "Untitled"
                                                   : QFileInfo(currentFilePath).fileName();
    const QString dirty = modified ? " \u25cf" : "";

    setWindowTitle(name + dirty);

    QTabWidget *activeTabs = focusedTabs ? focusedTabs : editorTabs;
    const int idx = activeTabs->currentIndex();
    if (idx >= 0)
        activeTabs->setTabText(idx, name + dirty);
}

void MainWindow::updateRunCombo()
{
    if (!runFileCombo)
        return;
    const QString current = runFileCombo->currentText();
    runFileCombo->blockSignals(true);
    runFileCombo->clear();
    runFileCombo->addItem("Current File", QString());
    for (QTabWidget *tabs : allTabWidgets()) {
        for (int i = 0; i < tabs->count(); ++i) {
            if (auto *ed = qobject_cast<CodeEditor *>(tabs->widget(i))) {
                const QString fp = ed->getFilePath();
                if (!fp.isEmpty())
                    runFileCombo->addItem(QFileInfo(fp).fileName(), fp);
            }
        }
    }
    const int idx = runFileCombo->findText(current);
    if (idx >= 0)
        runFileCombo->setCurrentIndex(idx);
    runFileCombo->blockSignals(false);
}

void MainWindow::stopRun()
{
    if (runProcess && runProcess->state() != QProcess::NotRunning) {
        runProcess->kill();
        runProcess = nullptr;
    }
    if (debugAdapter && debugAdapter->isRunning())
        stopDebugging();
    if (actStop)
        actStop->setEnabled(false);
    outputPane->setReadOnly(true);
}

void MainWindow::runFile()
{
    if (session && session->role() == CollabSession::Role::Guest) {
        bottomTabs->setCurrentWidget(outputPane);
        bottomDock->setVisible(true);
        outputPane->appendPlainText("[Collab] Code execution is controlled by the host.");
        return;
    }

    QString path;
    if (runFileCombo && runFileCombo->currentIndex() >= 0)
        path = runFileCombo->currentData().toString();
    if (path.isEmpty()) {
        if (auto *ed = qobject_cast<CodeEditor *>(
                (focusedTabs ? focusedTabs : editorTabs)->currentWidget()))
            path = ed->getFilePath();
    }
    if (path.isEmpty()) {
        outputPane->appendPlainText("[TeamHub] Save file before running.");
        return;
    }

    if (QFileInfo(path).isRelative() && !fileBrowser->rootPath().isEmpty())
        path = QDir(fileBrowser->rootPath()).absoluteFilePath(path);

    for (QTabWidget *tabs : allTabWidgets()) {
        for (int i = 0; i < tabs->count(); ++i) {
            if (auto *ed = qobject_cast<CodeEditor *>(tabs->widget(i))) {
                const QString fp = ed->getFilePath();
                if (!fp.isEmpty() && ed->isModified())
                    ed->saveFile(fp);
            }
        }
    }

    if (runProcess && runProcess->state() != QProcess::NotRunning)
        runProcess->kill();

    const QString pythonvenv = fileBrowser->findFile("python.exe");
    const QString pythonpath = !pythonvenv.isEmpty() ? pythonvenv : "python";
    outputPane->clear();
    outputPane->setReadOnly(false);
    inputStartPos = 0;
    bottomTabs->setCurrentWidget(outputPane);
    bottomDock->setVisible(true);

    runProcess = new QProcess(this);
    runProcess->setProcessChannelMode(QProcess::MergedChannels);

    connect(runProcess, &QProcess::readyReadStandardOutput, this, [this]() {
        const QString chunk = QString::fromLocal8Bit(runProcess->readAllStandardOutput());

        QTextCursor userSel(outputPane->document());
        userSel.setPosition(inputStartPos);
        userSel.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
        const QString userTyped = userSel.selectedText().replace(QChar(0x2029), '\n');
        if (!userTyped.isEmpty())
            userSel.removeSelectedText();

        QTextCursor c(outputPane->document());
        c.movePosition(QTextCursor::End);
        c.insertText(chunk);
        QTextCursor endC(outputPane->document());
        endC.movePosition(QTextCursor::End);
        inputStartPos = endC.position();

        if (!userTyped.isEmpty()) {
            QTextCursor restore(outputPane->document());
            restore.movePosition(QTextCursor::End);
            restore.insertText(userTyped);
        }

        QTextCursor fin(outputPane->document());
        fin.movePosition(QTextCursor::End);
        outputPane->setTextCursor(fin);
        outputPane->ensureCursorVisible();

        if (session)
            session->broadcastRunOutput(chunk);
    });

    connect(runProcess,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            [this](int code, QProcess::ExitStatus) {
                const QString msg = QString("\n[TeamHub] Exit code: %1").arg(code);
                outputPane->appendPlainText(msg);
                if (session)
                    session->broadcastRunOutput(msg);
                runProcess = nullptr;
                outputPane->setReadOnly(true);
                if (actStop)
                    actStop->setEnabled(false);
            });

    runProcess->start(pythonpath, {path});
    if (actStop)
        actStop->setEnabled(true);
}

void MainWindow::newFile()
{
    if (editorTabs->count() == 0) {
        editor = createTab("Untitled");
        editorTabs->setCurrentIndex(0);
        currentFilePath.clear();
        statusFile->setText("Untitled");
        updateWindowTitle();
        return;
    }

    if (editor && editor->isModified()) {
        const auto btn = QMessageBox::question(this,
                                               "Unsaved Changes",
                                               "Save changes before creating a new file?",
                                               QMessageBox::Save | QMessageBox::Discard
                                                   | QMessageBox::Cancel);
        if (btn == QMessageBox::Save) {
            if (!saveFile())
                return;
        } else if (btn == QMessageBox::Cancel)
            return;
    }

    editor->clear();
    editor->setModified(false);
    currentFilePath.clear();
    statusFile->setText("Untitled");
    editorTabs->setTabText(editorTabs->currentIndex(), "Untitled");
    updateWindowTitle();
}

void MainWindow::clearTabs()
{
    while (editorTabs->count() > 0) {
        QWidget *w = editorTabs->widget(0);
        editorTabs->removeTab(0);
        delete w;
    }
    while (editorTabs2 && editorTabs2->count() > 0) {
        QWidget *w = editorTabs2->widget(0);
        editorTabs2->removeTab(0);
        w->deleteLater();
    }
    editor = nullptr;
}

void MainWindow::openFile()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        "Open File",
        {},
        "All Files (*);;Python Files (*.py);;C/C++ Files (*.cpp *.c *.h *.hpp);;"
        "Web Files (*.js *.ts *.html *.css);;JSON (*.json);;Markdown (*.md)");
    if (path.isEmpty())
        return;

    openFileFromBrowser(path);
}

void MainWindow::openFolder()
{
    const QString path = QFileDialog::getExistingDirectory(this, "Open Folder");
    if (path.isEmpty())
        return;
    openProjectFolder(path);
}

void MainWindow::openProjectFolder(const QString &path)
{
    fileBrowser->setRootPath(path);
    clearTabs();

    const QString name = QFileInfo(path).fileName();
    setWindowTitle(name);
    outputPane->appendPlainText("[TeamHub] Opened folder: " + path);
    terminal->setWorkingDirectory(path);
    if (gitPanel_)
        gitPanel_->setRepoPath(path);

    currentProjectPath = path;
    currentProjectId = ProjectDB::instance().upsertProject(path, name);

    const auto files = ProjectDB::instance().loadOpenFiles(currentProjectId);
    if (files.isEmpty())
        return;

    int activeIdx = 0;
    for (const auto &f : files) {
        if (!QFileInfo::exists(f.path))
            continue;
        openFileFromBrowser(f.path);
        if (f.isActive)
            activeIdx = editorTabs->currentIndex();
    }
    if (editorTabs->count() > 0) {
        editorTabs->setCurrentIndex(activeIdx);
        editor = qobject_cast<CodeEditor *>(editorTabs->currentWidget());
    }
}

void MainWindow::cloneRepo()
{
    QDialog dlg(this);
    dlg.setWindowTitle("Clone Repository");
    dlg.setMinimumWidth(420);

    auto *layout = new QVBoxLayout(&dlg);

    auto *urlEdit = new QLineEdit(&dlg);
    urlEdit->setPlaceholderText("https://github.com/user/repo.git");
    layout->addWidget(new QLabel("Repository URL:", &dlg));
    layout->addWidget(urlEdit);

    auto *pathRow = new QHBoxLayout;
    auto *pathEdit = new QLineEdit(&dlg);
    pathEdit->setPlaceholderText("Select destination folder...");
    auto *browseBtn = new QPushButton("Browse...", &dlg);
    pathRow->addWidget(pathEdit, 1);
    pathRow->addWidget(browseBtn);
    layout->addWidget(new QLabel("Destination folder:", &dlg));
    layout->addLayout(pathRow);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    layout->addWidget(buttons);

    connect(browseBtn, &QPushButton::clicked, &dlg, [&]() {
        const QString dir = QFileDialog::getExistingDirectory(&dlg, "Select Destination Folder");
        if (!dir.isEmpty())
            pathEdit->setText(dir);
    });
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted)
        return;

    const QString url = urlEdit->text().trimmed();
    const QString dest = pathEdit->text().trimmed();
    if (url.isEmpty() || dest.isEmpty())
        return;

    outputPane->appendPlainText("[Git] Cloning " + url + " into " + dest + "...");
    bottomDock->setVisible(true);

    auto *proc = new QProcess(this);
    proc->setWorkingDirectory(dest);
    proc->setProcessChannelMode(QProcess::MergedChannels);

    connect(proc, &QProcess::readyReadStandardOutput, this, [this, proc]() {
        outputPane->appendPlainText(QString::fromLocal8Bit(proc->readAllStandardOutput()).trimmed());
    });

    connect(proc,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            [this, proc, url, dest](int code, QProcess::ExitStatus) {
                if (code == 0) {
                    const QString repoName = url.section('/', -1).remove(".git");
                    const QString clonedPath = dest + "/" + repoName;
                    outputPane->appendPlainText("[Git] Clone successful.");
                    openProjectFolder(clonedPath);
                } else {
                    outputPane->appendPlainText("[Git] Clone failed (exit code "
                                                + QString::number(code) + ").");
                }
                proc->deleteLater();
            });

    proc->start("git", {"clone", url, dest + "/" + url.section('/', -1).remove(".git")});
}

bool MainWindow::saveFile()
{
    QTabWidget *activeTabs = focusedTabs ? focusedTabs : editorTabs;
    CodeEditor *activeEditor = qobject_cast<CodeEditor *>(activeTabs->currentWidget());
    if (!activeEditor)
        return false;

    QString path = activeEditor->getFilePath();

    if (path.isEmpty())
        return saveFileAs();

    activeEditor->saveFile(path);

    const QString name = QFileInfo(path).fileName();
    currentFilePath = path;
    statusFile->setText(name);
    activeTabs->setTabText(activeTabs->currentIndex(), name);
    updateWindowTitle();
    outputPane->appendPlainText("[TeamHub] Saved: " + path);
    fileBrowser->refreshGitStatus();
    return true;
}

bool MainWindow::saveFileAs()
{
    const QString path = QFileDialog::getSaveFileName(this,
                                                      "Save File As",
                                                      {},
                                                      "Python Files (*.py);;All Files (*)");
    if (path.isEmpty())
        return false;

    currentFilePath = path;
    return saveFile();
}

void MainWindow::toggleSidePanel()
{
    if (leftPanel->isVisible()) {
        leftPanel->setVisible(false);
        activeSidePanel = -1;
        btnFiles->setChecked(false);
        btnCollab->setChecked(false);
        btnTeam->setChecked(false);
    } else {
        setSidePanelPage(0);
    }
}

void MainWindow::toggleBottomDock()
{
    bottomDock->setVisible(!bottomDock->isVisible());
}

void MainWindow::onSessionProjectInit(int hostSiteId, const QStringList &files)
{
    if (hostSiteId > 0 && !peerRoles.contains(hostSiteId))
        peerRoles[hostSiteId] = "host";

    if (session) {
        const qint64 epoch = session->sessionStartEpoch();
        if (epoch > 0)
            sessionStart = QDateTime::fromSecsSinceEpoch(epoch);
    }

    const bool readOnly = session && session->collabMode() == CollabSession::Mode::ReadOnly;
    outputPane->appendPlainText(QString("[Collab] Project received — %1 file(s)%2")
                                    .arg(files.size())
                                    .arg(readOnly ? " (read-only)" : ""));

    if (collabStatusLabel) {
        collabStatusLabel->setText(
            QString("Guest — %1 file(s)%2").arg(files.size()).arg(readOnly ? " · read-only" : ""));
    }

    fileBrowser->setRemoteFiles(files);
    setSidePanelPage(0);
    btnFiles->setChecked(true);
}

void MainWindow::onSessionRunOutput(const QString &text)
{
    outputPane->appendPlainText(text);
    bottomTabs->setCurrentWidget(outputPane);
    bottomDock->setVisible(true);
}

void MainWindow::onSessionFileCreated(const QString &relPath)
{
    outputPane->appendPlainText("[Collab] File created: " + relPath);
    showToast("Created: " + relPath, "success");

    if (session && session->role() == CollabSession::Role::Guest)
        fileBrowser->setRemoteFiles(session->fileList());
}

void MainWindow::onSessionFileDeleted(const QString &relPath)
{
    outputPane->appendPlainText("[Collab] File deleted: " + relPath);
    showToast("Deleted: " + relPath, "warning");

    if (session && session->role() == CollabSession::Role::Guest)
        fileBrowser->setRemoteFiles(session->fileList());
}

void MainWindow::onSessionFileRenamed(const QString &oldPath, const QString &newPath)
{
    outputPane->appendPlainText(QString("[Collab] File renamed: %1 → %2").arg(oldPath, newPath));
    if (session && session->role() == CollabSession::Role::Guest)
        fileBrowser->setRemoteFiles(session->fileList());
}

bool MainWindow::showStartCollabDialog()
{
    const QString projectRoot = fileBrowser->rootPath();

    if (projectRoot.isEmpty()) {
        QMessageBox::warning(
            this,
            "No Project Open",
            "You need to open a project folder before starting a collaboration session.");
        return false;
    }

    QDialog dlg(this);
    dlg.setWindowTitle("Start Collaboration");
    dlg.setMinimumWidth(400);
    auto *mainVl = new QVBoxLayout(&dlg);
    mainVl->setSpacing(10);

    auto *roomRow = new QHBoxLayout;
    roomRow->addWidget(new QLabel("Room name:"));
    auto *roomEdit = new QLineEdit;
    roomEdit->setPlaceholderText("e.g. my-project");
    roomRow->addWidget(roomEdit);
    mainVl->addLayout(roomRow);

    auto *permGroup = new QGroupBox("Guest permissions");
    auto *permHl = new QHBoxLayout(permGroup);
    auto *rbReadWrite = new QRadioButton("Read && Write");
    auto *rbReadOnly = new QRadioButton("Read-only");
    rbReadWrite->setChecked(true);
    permHl->addWidget(rbReadWrite);
    permHl->addWidget(rbReadOnly);
    mainVl->addWidget(permGroup);

    auto *filesGroup = new QGroupBox("Files to share");
    auto *filesVl = new QVBoxLayout(filesGroup);
    auto *rbAllFiles = new QRadioButton("All files");
    auto *rbSelFiles = new QRadioButton("Select files/folders");
    rbAllFiles->setChecked(true);
    filesVl->addWidget(rbAllFiles);
    filesVl->addWidget(rbSelFiles);

    auto *fileTree = new QTreeWidget;
    fileTree->setHeaderHidden(true);
    fileTree->setMinimumHeight(180);
    fileTree->setVisible(false);
    fileTree->setSelectionMode(QAbstractItemView::NoSelection);

    if (!projectRoot.isEmpty()) {
        std::function<void(QTreeWidgetItem *, const QString &, const QString &)> populate;
        populate = [&populate](QTreeWidgetItem *parent,
                               const QString &absPath,
                               const QString &relPath) {
            const QStringList filters = {"*.py", "*.cpp", "*.h", "*.pro", "*.txt", "*.md", "*.json"};
            QDir dir(absPath);
            for (const QString &sub : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
                const QString subRel = relPath.isEmpty() ? sub : relPath + "/" + sub;
                auto *item = new QTreeWidgetItem(parent, {sub});
                item->setCheckState(0, Qt::Checked);
                populate(item, absPath + "/" + sub, subRel);
            }
            for (const QString &file : dir.entryList(filters, QDir::Files, QDir::Name)) {
                const QString fileRel = relPath.isEmpty() ? file : relPath + "/" + file;
                auto *item = new QTreeWidgetItem(parent, {file});
                item->setCheckState(0, Qt::Checked);
                item->setData(0, Qt::UserRole, fileRel);
            }
        };

        auto *rootItem = new QTreeWidgetItem(fileTree, {QFileInfo(projectRoot).fileName()});
        rootItem->setCheckState(0, Qt::Checked);
        fileTree->addTopLevelItem(rootItem);
        populate(rootItem, projectRoot, "");
        rootItem->setExpanded(true);

        connect(fileTree,
                &QTreeWidget::itemChanged,
                fileTree,
                [fileTree](QTreeWidgetItem *item, int col) {
                    if (col != 0 || !item->data(0, Qt::UserRole).toString().isEmpty())
                        return;
                    fileTree->blockSignals(true);
                    std::function<void(QTreeWidgetItem *, Qt::CheckState)> cascade;
                    cascade = [&cascade](QTreeWidgetItem *p, Qt::CheckState s) {
                        for (int i = 0; i < p->childCount(); ++i) {
                            p->child(i)->setCheckState(0, s);
                            cascade(p->child(i), s);
                        }
                    };
                    cascade(item,
                            item->checkState(0) != Qt::Unchecked ? Qt::Checked : Qt::Unchecked);
                    fileTree->blockSignals(false);
                });
    } else {
        rbSelFiles->setEnabled(false);
    }

    filesVl->addWidget(fileTree);
    connect(rbSelFiles, &QRadioButton::toggled, fileTree, &QWidget::setVisible);
    mainVl->addWidget(filesGroup);

    auto *btnRow = new QHBoxLayout;
    btnRow->addStretch();
    auto *btnCancel = new QPushButton("Cancel");
    auto *btnStart = new QPushButton("Start");
    btnStart->setDefault(true);
    btnStart->setObjectName("primaryBtn");
    btnRow->addWidget(btnCancel);
    btnRow->addWidget(btnStart);
    mainVl->addLayout(btnRow);

    connect(btnCancel, &QPushButton::clicked, &dlg, &QDialog::reject);
    connect(btnStart, &QPushButton::clicked, &dlg, [&dlg, roomEdit]() {
        if (!roomEdit->text().trimmed().isEmpty())
            dlg.accept();
    });

    if (dlg.exec() != QDialog::Accepted)
        return false;

    const QString room = roomEdit->text().trimmed();
    const CollabSession::Mode mode = rbReadOnly->isChecked() ? CollabSession::Mode::ReadOnly
                                                             : CollabSession::Mode::ReadWrite;

    QStringList selectedFiles;
    if (rbSelFiles->isChecked()) {
        std::function<void(QTreeWidgetItem *)> collect;
        collect = [&collect, &selectedFiles](QTreeWidgetItem *item) {
            const QString rel = item->data(0, Qt::UserRole).toString();
            if (!rel.isEmpty() && item->checkState(0) == Qt::Checked)
                selectedFiles.append(rel);
            for (int i = 0; i < item->childCount(); ++i)
                collect(item->child(i));
        };
        for (int i = 0; i < fileTree->topLevelItemCount(); ++i)
            collect(fileTree->topLevelItem(i));
    }

    startCollab(room, mode, selectedFiles);
    return true;
}

void MainWindow::onCollabUserContextMenu(const QPoint &pos)
{
    if (!session)
        return;

    QListWidgetItem *item = collabUsersList->itemAt(pos);
    if (!item)
        return;

    const int targetSiteId = item->data(Qt::UserRole).toInt();
    if (targetSiteId == session->siteId())
        return;

    QMenu menu(this);

    const QString relPath = peerFiles.value(targetSiteId);
    const QString peerName = peerNames.value(targetSiteId, QString("user_%1").arg(targetSiteId));
    QAction *gotoAct = menu.addAction(QString("Go to %1").arg(peerName));
    gotoAct->setEnabled(!relPath.isEmpty());
    connect(gotoAct, &QAction::triggered, this, [this, targetSiteId, relPath]() {
        if (!session || relPath.isEmpty())
            return;

        const bool isGuest = session->role() == CollabSession::Role::Guest;
        const QString openPath = isGuest ? relPath
                                         : QDir(session->projectRoot()).absoluteFilePath(relPath);

        setSidePanelPage(0);

        if (!isGuest)
            fileBrowser->revealFile(openPath);

        openFileFromBrowser(openPath);

        QTabWidget *activeTabs = focusedTabs ? focusedTabs : editorTabs;
        CodeEditor *targetEd = qobject_cast<CodeEditor *>(activeTabs->currentWidget());
        if (targetEd) {
            const int scintillaPos = targetEd->remoteCursorPos(targetSiteId);
            if (scintillaPos >= 0)
                targetEd->goToScintillaPos(scintillaPos);
        }
    });

    if (session->role() == CollabSession::Role::Host) {
        menu.addSeparator();

        QMenu *roleMenu = menu.addMenu("Change role");
        const QString currentRole = peerRoles.value(targetSiteId, "write");
        for (const auto &pair :
             std::initializer_list<std::pair<const char *, const char *>>{{"write", "Write"},
                                                                          {"read", "Read"}}) {
            const QString key = pair.first;
            const QString lbl = pair.second;
            QAction *act = roleMenu->addAction(lbl);
            act->setCheckable(true);
            act->setChecked(currentRole == key);
            connect(act, &QAction::triggered, this, [this, targetSiteId, key]() {
                if (!session)
                    return;
                peerRoles[targetSiteId] = key;
                session->sendRoleChange(targetSiteId, key);
                refreshCollabUsersList();
            });
        }

        QAction *kickAct = menu.addAction(QString("Kick %1").arg(peerName));
        connect(kickAct, &QAction::triggered, this, [this, targetSiteId, peerName]() {
            if (!session)
                return;
            session->kickUser(targetSiteId);
            outputPane->appendPlainText(QString("[Collab] Kicked %1").arg(peerName));
        });
    }

    menu.exec(collabUsersList->viewport()->mapToGlobal(pos));
}

void MainWindow::onEndCollabRequested()
{
    if (!session) {
        stopAllCollab();
        return;
    }
    pendingEndCollab = true;
    session->sendFinalStates(collectCurrentFileTexts());
    session->endSession();
    QTimer::singleShot(3000, this, [this]() {
        if (pendingEndCollab && (!reportDialog || !reportDialog->isVisible()))
            stopAllCollab();
    });
}

void MainWindow::onSessionReportReady(const SessionReportData &report)
{
    if (reportDialog) {
        reportDialog->close();
        reportDialog->deleteLater();
        reportDialog = nullptr;
    }

    pendingEndCollab = true;
    reportDialog = new SessionReportDialog(report, true, this);

    connect(reportDialog, &SessionReportDialog::sessionEnded, this, &MainWindow::stopAllCollab);
    connect(reportDialog, &QDialog::rejected, this, &MainWindow::stopAllCollab);

    connect(reportDialog, &QDialog::finished, this, [this]() {
        pendingEndCollab = false;
        reportDialog = nullptr;
    });

    reportDialog->show();
    outputPane->appendPlainText("[Collab] Session report received.");
}

void MainWindow::onSessionAiInsightsReady(const AiInsights &ai)
{
    if (reportDialog)
        reportDialog->updateAiSection(ai);
}

/* ── Auth ────────────────────────────────────────────────────── */

void MainWindow::setupAuthManager()
{
    auth = new AuthManager(this);
    teamsPanel->setAuthManager(auth);
    teamsPanel->clear();
    updateCollabAccess();

    connect(auth, &AuthManager::sessionRestored, this, [this](const AuthManager::UserInfo &) {
        updateProfileButton();
        teamsPanel->refresh();
        updateCollabAccess();
    });
    connect(auth, &AuthManager::loginSuccess, this, [this](const AuthManager::UserInfo &) {
        updateProfileButton();
        teamsPanel->refresh();
        updateCollabAccess();
    });
    connect(auth, &AuthManager::logoutFinished, this, [this] {
        updateProfileButton();
        teamsPanel->clear();
        stopAllCollab();
        updateCollabAccess();
    });
    connect(auth, &AuthManager::profileUpdated, this, [this](const AuthManager::UserInfo &) {
        updateProfileButton();
        teamsPanel->refresh();
        outputPane->appendPlainText("[Auth] Profile updated");
    });
    connect(auth, &AuthManager::profileUpdateFailed, this, [this](const QString &err) {
        outputPane->appendPlainText("[Auth] Failed to update profile: " + err);
        QMessageBox::warning(this, "Edit Profile", "Failed to update profile: " + err);
    });

    auth->loadSavedSession();
}

void MainWindow::updateProfileButton()
{
    if (!btnProfile)
        return;

    const bool loggedIn = auth && auth->isLoggedIn();
    if (loggedIn) {
        const AuthManager::UserInfo user = auth->currentUser();
        const QString seed = user.email.isEmpty() ? QString::number(user.id) : user.email;
        const int size = 30;

        btnProfile->setToolButtonStyle(Qt::ToolButtonIconOnly);
        btnProfile->setIconSize(QSize(size, size));
        btnProfile->setText(QString());

        const QPixmap fallback = Avatar::letterPixmap(Avatar::initialFor(user.username),
                                                      Avatar::colorForId(seed),
                                                      size);
        Avatar::load(btnProfile, user.avatarUrl, fallback, size, [this](QPixmap pix) {
            btnProfile->setIcon(QIcon(pix));
        });

        btnProfile->setToolTip(user.username + "\n\nClick for profile options");
        btnProfile->setProperty("loggedIn", true);
    } else {
        const QIcon userIc = loadIconTransparent("user.png");
        if (!userIc.isNull()) {
            btnProfile->setToolButtonStyle(Qt::ToolButtonIconOnly);
            btnProfile->setIcon(userIc);
            btnProfile->setIconSize(QSize(24, 24));
            btnProfile->setText(QString());
        } else {
            btnProfile->setToolButtonStyle(Qt::ToolButtonTextOnly);
            btnProfile->setIcon(QIcon());
            btnProfile->setText("User");
        }
        btnProfile->setToolTip("Sign in");
        btnProfile->setProperty("loggedIn", false);
    }
    btnProfile->style()->unpolish(btnProfile);
    btnProfile->style()->polish(btnProfile);
}

void MainWindow::openProfileDialog()
{
    if (!auth || !auth->isLoggedIn())
        return;

    const AuthManager::UserInfo user = auth->currentUser();
    const int avatarSize = 64;

    QDialog dlg(this);
    dlg.setWindowTitle("Edit Profile");
    dlg.setFixedWidth(280);

    auto *vl = new QVBoxLayout(&dlg);

    auto *avatarPreview = new QLabel;
    avatarPreview->setFixedSize(avatarSize, avatarSize);
    const QString seed = user.email.isEmpty() ? QString::number(user.id) : user.email;
    const QPixmap fallback = Avatar::letterPixmap(Avatar::initialFor(user.username),
                                                  Avatar::colorForId(seed),
                                                  avatarSize);
    Avatar::load(avatarPreview, user.avatarUrl, fallback, avatarSize, [avatarPreview](QPixmap pix) {
        avatarPreview->setPixmap(pix);
    });

    auto *avatarRow = new QHBoxLayout;
    avatarRow->addStretch(1);
    avatarRow->addWidget(avatarPreview);
    avatarRow->addStretch(1);
    vl->addLayout(avatarRow);

    auto *btnChoosePhoto = new QPushButton("Choose Photo...");
    vl->addWidget(btnChoosePhoto);

    QString chosenAvatarPath;
    connect(btnChoosePhoto, &QPushButton::clicked, &dlg, [&]() {
        const QString path = QFileDialog::getOpenFileName(&dlg,
                                                          "Choose Avatar",
                                                          QString(),
                                                          "Images (*.png *.jpg *.jpeg)");
        if (path.isEmpty())
            return;
        chosenAvatarPath = path;
        QPixmap photo;
        if (photo.load(path))
            avatarPreview->setPixmap(Avatar::circularPixmap(photo, avatarSize));
    });

    vl->addWidget(new QLabel("Username:"));
    auto *nameEdit = new QLineEdit(user.username);
    vl->addWidget(nameEdit);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    vl->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted)
        return;

    const QString newName = nameEdit->text().trimmed();
    if (newName == user.username && chosenAvatarPath.isEmpty())
        return;

    auth->updateProfile(newName, chosenAvatarPath);
}

void MainWindow::updateCollabAccess()
{
    const bool loggedIn = auth && auth->isLoggedIn();
    const QString tip = loggedIn ? QString() : "Sign in to use collaboration";

    if (btnStartCollab) {
        btnStartCollab->setEnabled(loggedIn);
        btnStartCollab->setToolTip(tip);
    }
    if (btnJoinCollab) {
        btnJoinCollab->setEnabled(loggedIn);
        btnJoinCollab->setToolTip(tip);
    }
}

void MainWindow::openSettings()
{
    auto *dlg = new SettingsDialog(this);
    connect(dlg, &SettingsDialog::settingsApplied, this, &MainWindow::applySettings);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->exec();
}

void MainWindow::applySettings()
{
    auto &s = SettingsManager::instance();
    const QFont font(s.fontFamily(), s.fontSize());
    const int tabW = s.tabWidth();
    const auto theme = (s.theme() == "dark") ? CodeEditor::Theme::Dark : CodeEditor::Theme::Light;

    for (QTabWidget *tabs : allTabWidgets()) {
        for (int i = 0; i < tabs->count(); ++i) {
            if (auto *ed = qobject_cast<CodeEditor *>(tabs->widget(i))) {
                ed->applyEditorFont(font);
                ed->setTabWidth(tabW);
                ed->setTheme(theme);
            }
        }
    }
}

void MainWindow::refreshRecentMenu(QMenu *menu)
{
    menu->clear();
    const auto projects = ProjectDB::instance().recentProjects(10);
    if (projects.isEmpty()) {
        menu->addAction("(no recent projects)")->setEnabled(false);
        return;
    }
    for (const auto &p : projects) {
        const QString label = p.name + "  \t" + p.path;
        auto *act = menu->addAction(label);
        connect(act, &QAction::triggered, this, [this, path = p.path] {
            if (QFileInfo::exists(path)) {
                openProjectFolder(path);
            } else {
                ProjectDB::instance().removeProject(path);
                if (SettingsManager::instance().lastProjectPath() == path) {
                    SettingsManager::instance().setLastProjectPath("");
                    SettingsManager::instance().save();
                }
                outputPane->appendPlainText("[TeamHub] Project not found and removed from history: "
                                            + path);
            }
        });
    }
}

void MainWindow::saveSessionToDb()
{
    if (currentProjectId < 0)
        return;

    QList<ProjectDB::FileState> files;
    const int current = editorTabs->currentIndex();
    for (int i = 0; i < editorTabs->count(); ++i) {
        auto *ed = qobject_cast<CodeEditor *>(editorTabs->widget(i));
        if (!ed)
            continue;
        const QString fp = ed->getFilePath();
        if (fp.isEmpty() || !QFileInfo(fp).isAbsolute())
            continue;
        files.append({fp, i, i == current});
    }
    ProjectDB::instance().saveOpenFiles(currentProjectId, files);
}

QList<QTabWidget *> MainWindow::allTabWidgets() const
{
    QList<QTabWidget *> list;
    list.append(editorTabs);
    if (splitActive && editorTabs2)
        list.append(editorTabs2);
    return list;
}

void MainWindow::openInSplitPanel(const QString &path, QTabWidget *sourcePanel)
{
    QTabWidget *other = (sourcePanel == editorTabs) ? editorTabs2 : editorTabs;

    for (int i = 0; i < other->count(); i++) {
        if (auto *ed = qobject_cast<CodeEditor *>(other->widget(i))) {
            if (ed->getFilePath() == path) {
                other->setCurrentIndex(i);
                focusedTabs = other;
                return;
            }
        }
    }

    if (!splitActive) {
        splitActive = true;
        editorTabs2->show();
        editorSplitter->setSizes({editorSplitter->width() / 2, editorSplitter->width() / 2});
    }

    focusedTabs = other;
    openFileFromBrowser(path);
}

void MainWindow::closeTabAt(QTabWidget *tabs, int index)
{
    CodeEditor *tabEditor = qobject_cast<CodeEditor *>(tabs->widget(index));
    if (!tabEditor)
        return;

    editorToMgr.remove(tabEditor);

    if (session) {
        const QString relPath = toSessionKey(tabEditor->getFilePath());
        if (session->hasActiveRGA(relPath)) {
            tabEditor->clearRemoteCursors();

            bool fileOpenElsewhere = false;
            for (QTabWidget *tw : allTabWidgets()) {
                for (int i = 0; i < tw->count(); ++i) {
                    auto *other = qobject_cast<CodeEditor *>(tw->widget(i));
                    if (other && other != tabEditor
                        && toSessionKey(other->getFilePath()) == relPath) {
                        fileOpenElsewhere = true;
                        break;
                    }
                }
                if (fileOpenElsewhere)
                    break;
            }

            if (!fileOpenElsewhere)
                session->releaseRGA(relPath);
        }
        markTabAsCollab(tabEditor, false);
    }

    if (tabEditor->isModified() && !(session && session->role() == CollabSession::Role::Guest)) {
        const auto btn = QMessageBox::question(this,
                                               "Unsaved Changes",
                                               "Save changes before closing this tab?",
                                               QMessageBox::Save | QMessageBox::Discard
                                                   | QMessageBox::Cancel);
        if (btn == QMessageBox::Save) {
            editor = tabEditor;
            if (!saveFile())
                return;
        } else if (btn == QMessageBox::Cancel) {
            return;
        }
    }

    tabs->blockSignals(true);
    tabs->removeTab(index);
    tabs->blockSignals(false);
    tabEditor->deleteLater();

    updateWelcomeVisibility();

    if (tabs == editorTabs2 && editorTabs2->count() == 0) {
        splitActive = false;
        editorTabs2->hide();
        focusedTabs = editorTabs;
        if (editorTabs->count() > 0)
            onTabChanged(editorTabs->currentIndex());
        return;
    }

    if (tabs == editorTabs && editorTabs->count() == 0 && splitActive) {
        editorTabs2->blockSignals(true);
        while (editorTabs2->count() > 0) {
            QWidget *w = editorTabs2->widget(0);
            const QString label = editorTabs2->tabText(0);
            editorTabs2->removeTab(0);
            editorTabs->addTab(w, label);
        }
        editorTabs2->blockSignals(false);
        splitActive = false;
        editorTabs2->hide();
        focusedTabs = editorTabs;
        if (editorTabs->count() > 0)
            editorTabs->setCurrentIndex(editorTabs->count() - 1);
    }
}

void MainWindow::showTabContextMenu(QTabWidget *tabs, const QPoint &pos)
{
    const int tabIndex = tabs->tabBar()->tabAt(pos);
    if (tabIndex < 0)
        return;

    QMenu menu(this);
    menu.addAction("Close Tab", [this, tabs, tabIndex]() { closeTabAt(tabs, tabIndex); });

    if (tabs->count() > 1) {
        menu.addAction("Close Others", [this, tabs, tabIndex]() {
            for (int i = tabs->count() - 1; i > tabIndex; --i)
                closeTabAt(tabs, i);
            for (int i = tabIndex - 1; i >= 0; --i)
                closeTabAt(tabs, i);
        });
    }

    auto *ed = qobject_cast<CodeEditor *>(tabs->widget(tabIndex));
    if (ed && !ed->getFilePath().isEmpty()) {
        menu.addAction("Split Tab", [this, tabs, tabIndex]() {
            auto *ed = qobject_cast<CodeEditor *>(tabs->widget(tabIndex));
            if (ed)
                openInSplitPanel(ed->getFilePath(), tabs);
        });
    }

    menu.exec(tabs->tabBar()->mapToGlobal(pos));
}
