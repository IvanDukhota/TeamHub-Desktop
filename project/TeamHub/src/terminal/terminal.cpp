#include "terminal.h"

#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QScrollBar>
#include <QTabBar>
#include <QTextCursor>
#include <QVBoxLayout>

TerminalEdit::TerminalEdit(const QString &workingDir, QWidget *parent)
    : QPlainTextEdit(parent)
    , process(new QProcess(this))
    , promptPos(0)
    , historyIdx(-1)
    , currentDir(QDir::toNativeSeparators(workingDir.isEmpty() ? QDir::homePath() : workingDir))
{
    QFont font("Consolas", 10);
    font.setFixedPitch(true);
    setFont(font);
    setUndoRedoEnabled(false);
    setLineWrapMode(QPlainTextEdit::WidgetWidth);
    setObjectName("terminalEdit");

    process->setProcessChannelMode(QProcess::MergedChannels);
    connect(process, &QProcess::readyReadStandardOutput, this, &TerminalEdit::onReadyRead);
    connect(process,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            &TerminalEdit::onProcessFinished);

    startShell();
}

TerminalEdit::~TerminalEdit()
{
    killProcess();
}

void TerminalEdit::killProcess()
{
    process->disconnect(this);
    if (process->state() != QProcess::NotRunning) {
        process->write("exit\r\n");
        if (!process->waitForFinished(500))
            process->kill();
    }
}

void TerminalEdit::startShell()
{
    process->setProcessEnvironment(QProcessEnvironment::systemEnvironment());
    process->setWorkingDirectory(currentDir);
    process->start("cmd.exe", {"/Q", "/K", "PROMPT $P$G"});
}

void TerminalEdit::appendOutput(const QString &text)
{
    QTextCursor c = textCursor();
    c.movePosition(QTextCursor::End);
    setTextCursor(c);
    insertPlainText(text);
    c.movePosition(QTextCursor::End);
    setTextCursor(c);
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
}

void TerminalEdit::setPromptPos()
{
    QTextCursor c = textCursor();
    c.movePosition(QTextCursor::End);
    promptPos = c.position();
}

void TerminalEdit::onReadyRead()
{
    QString text = QString::fromLocal8Bit(process->readAllStandardOutput());
    text.replace("\r\n", "\n");
    text.replace('\r', '\n');

    static const QRegularExpression promptRe(R"(([A-Za-z]:[^>\n]*)>)",
                                             QRegularExpression::MultilineOption);
    QRegularExpressionMatchIterator it = promptRe.globalMatch(text);
    QRegularExpressionMatch last;
    while (it.hasNext())
        last = it.next();
    if (last.hasMatch())
        currentDir = last.captured(1);

    QTextCursor userSel(document());
    userSel.setPosition(promptPos);
    userSel.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
    const QString userTyped = userSel.selectedText().replace(QChar(0x2029), '\n');
    if (!userTyped.isEmpty())
        userSel.removeSelectedText();

    QTextCursor c(document());
    c.movePosition(QTextCursor::End);
    c.insertText(text);
    QTextCursor endC(document());
    endC.movePosition(QTextCursor::End);
    promptPos = endC.position();

    if (!userTyped.isEmpty()) {
        QTextCursor restore(document());
        restore.movePosition(QTextCursor::End);
        restore.insertText(userTyped);
    }

    QTextCursor fin(document());
    fin.movePosition(QTextCursor::End);
    setTextCursor(fin);
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
}

void TerminalEdit::onProcessFinished(int, QProcess::ExitStatus)
{
    appendOutput("\n[shell exited — restarting]\n");
    startShell();
}

void TerminalEdit::ensureCursorInInputZone()
{
    if (textCursor().position() < promptPos) {
        QTextCursor c = textCursor();
        c.movePosition(QTextCursor::End);
        setTextCursor(c);
    }
}

QString TerminalEdit::currentInput() const
{
    QTextCursor c = textCursor();
    c.setPosition(promptPos);
    c.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
    return c.selectedText().replace(QChar(0x2029), '\n');
}

void TerminalEdit::clearCurrentInput()
{
    QTextCursor c = textCursor();
    c.setPosition(promptPos);
    c.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
    c.removeSelectedText();
    setTextCursor(c);
}

void TerminalEdit::submitCommand()
{
    QString cmd = currentInput();
    QTextCursor c = textCursor();
    c.movePosition(QTextCursor::End);
    setTextCursor(c);
    insertPlainText("\n");
    if (!cmd.trimmed().isEmpty()) {
        history.prepend(cmd);
        if (history.size() > 500)
            history.removeLast();
    }
    historyIdx = -1;

    if (cmd.trimmed().toLower() == "cls") {
        clear();
        insertPlainText(currentDir + ">");
        setPromptPos();
        return;
    }

    setPromptPos();

    process->write((cmd + "\r\n").toLocal8Bit());
}

void TerminalEdit::handleTab()
{
    QString input = currentInput();
    QString prefix = input.section(' ', -1);
    QString base = input.left(input.length() - prefix.length());

    QString searchDir = currentDir;
    QString filePrefix = prefix;
    const int lastSlash = prefix.lastIndexOf(QRegularExpression(R"([/\\])"));
    if (lastSlash >= 0) {
        const QString subDir = prefix.left(lastSlash + 1);
        searchDir = QFileInfo(subDir).isAbsolute() ? subDir : currentDir + "\\" + subDir;
        filePrefix = prefix.mid(lastSlash + 1);
    }

    QDir dir(searchDir);
    const QStringList matches = dir.entryList({filePrefix + "*"},
                                              QDir::AllEntries | QDir::NoDotAndDotDot);
    if (matches.isEmpty())
        return;

    if (matches.size() == 1) {
        QString completed = prefix.left(prefix.length() - filePrefix.length()) + matches.first();
        if (QFileInfo(searchDir + "/" + matches.first()).isDir())
            completed += "\\";
        clearCurrentInput();
        insertPlainText(base + completed);
    } else {
        QTextCursor c = textCursor();
        c.movePosition(QTextCursor::End);
        setTextCursor(c);
        insertPlainText("\n" + matches.join("    ") + "\n" + currentDir + ">");
        setPromptPos();
        insertPlainText(input);
    }
}

void TerminalEdit::historyUp()
{
    if (history.isEmpty())
        return;
    historyIdx = qMin(historyIdx + 1, history.size() - 1);
    clearCurrentInput();
    insertPlainText(history[historyIdx]);
}

void TerminalEdit::historyDown()
{
    if (historyIdx <= 0) {
        historyIdx = -1;
        clearCurrentInput();
        return;
    }
    --historyIdx;
    clearCurrentInput();
    insertPlainText(history[historyIdx]);
}

void TerminalEdit::keyPressEvent(QKeyEvent *e)
{
    ensureCursorInInputZone();
    switch (e->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        submitCommand();
        return;
    case Qt::Key_Tab:
        handleTab();
        return;
    case Qt::Key_Up:
        historyUp();
        return;
    case Qt::Key_Down:
        historyDown();
        return;
    case Qt::Key_Left:
    case Qt::Key_Backspace:
        if (textCursor().position() <= promptPos)
            return;
        QPlainTextEdit::keyPressEvent(e);
        return;
    case Qt::Key_Home: {
        QTextCursor c = textCursor();
        const bool shift = e->modifiers() & Qt::ShiftModifier;
        c.setPosition(promptPos, shift ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor);
        setTextCursor(c);
        return;
    }
    case Qt::Key_C:
        if (e->modifiers() == Qt::ControlModifier) {
            process->write("\x03");
            QTextCursor c = textCursor();
            c.movePosition(QTextCursor::End);
            setTextCursor(c);
            insertPlainText("^C\n");
            setPromptPos();
            return;
        }
        break;
    case Qt::Key_L:
        if (e->modifiers() == Qt::ControlModifier) {
            clear();
            insertPlainText(currentDir + ">");
            setPromptPos();
            return;
        }
        break;
    case Qt::Key_U:
        if (e->modifiers() == Qt::ControlModifier) {
            clearCurrentInput();
            return;
        }
        break;
    default:
        break;
    }
    QPlainTextEdit::keyPressEvent(e);
}

void TerminalEdit::mousePressEvent(QMouseEvent *e)
{
    QPlainTextEdit::mousePressEvent(e);
}
void TerminalEdit::mouseDoubleClickEvent(QMouseEvent *e)
{
    QPlainTextEdit::mouseDoubleClickEvent(e);
}
void TerminalEdit::contextMenuEvent(QContextMenuEvent *) {}

Terminal::Terminal(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    tabs = new QTabWidget(this);
    tabs->setObjectName("terminalTabs");
    tabs->setTabsClosable(true);
    tabs->setMovable(false);
    tabs->setDocumentMode(true);

    tabs->addTab(new QWidget(), "+");
    tabs->tabBar()->setTabButton(0, QTabBar::RightSide, nullptr);

    connect(tabs->tabBar(), &QTabBar::tabBarClicked, this, [this](int idx) {
        if (idx == tabs->count() - 1)
            addTerminal();
    });

    connect(tabs, &QTabWidget::tabCloseRequested, this, [this](int idx) {
        if (idx == tabs->count() - 1)
            return;
        auto *edit = qobject_cast<TerminalEdit *>(tabs->widget(idx));
        tabs->removeTab(idx);
        if (edit) {
            edit->killProcess();
            edit->deleteLater();
        }
    });

    layout->addWidget(tabs);
}

void Terminal::setWorkingDirectory(const QString &path)
{
    workingDir = path;
}

void Terminal::addTerminal()
{
    ++counter;
    auto *edit = new TerminalEdit(workingDir, this);
    const int plusIdx = tabs->count() - 1;
    tabs->insertTab(plusIdx, edit, QString("Terminal %1").arg(counter));
    tabs->tabBar()->setTabButton(tabs->count() - 1, QTabBar::RightSide, nullptr);
    tabs->setCurrentIndex(plusIdx);
    edit->setFocus();
}

int Terminal::terminalCount() const
{
    return qMax(0, tabs->count() - 1);
}

void Terminal::focusCurrent()
{
    if (auto *ed = qobject_cast<TerminalEdit *>(tabs->currentWidget()))
        ed->setFocus();
}

void Terminal::killAll()
{
    for (int i = 0; i < tabs->count() - 1; ++i) {
        if (auto *ed = qobject_cast<TerminalEdit *>(tabs->widget(i)))
            ed->killProcess();
    }
}