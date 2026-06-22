#include "codeeditor.h"

#include <Qsci/qscicommand.h>
#include <Qsci/qscicommandset.h>

#include <QClipboard>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTextCursor>
#include <QTextStream>

class RemoteCursorOverlay : public QWidget
{
public:
    explicit RemoteCursorOverlay(CodeEditor *editor)
        : QWidget(editor->viewport())
        , ed(editor)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setAutoFillBackground(false);
        resize(editor->viewport()->size());
        show();
        raise();
    }

protected:
    void paintEvent(QPaintEvent *) override { ed->paintRemoteCursors(this); }

private:
    CodeEditor *ed;
};

static const QList<QPair<QChar, QChar>> kAutoPairs = {
    {'(', ')'},
    {'[', ']'},
    {'{', '}'},
    {'"', '"'},
    {'\'', '\''},
};
const QColor CodeEditor::kCursorColors[4] = {
    QColor("#007acc"),
    QColor("#f44747"),
    QColor("#4ec9b0"),
    QColor("#d7ba7d"),
};

CodeEditor::CodeEditor(QWidget *parent)
    : QsciScintilla(parent)
    , lexer(nullptr)
    , theme(Theme::Dark)
    , zoomLevel(0)
    , lintProcess(nullptr)
    , lintTimer(nullptr)
{
    setupLexer();
    setupFonts();
    setupMargins();
    setupEditor();
    setupAutoComplete();
    setupLinter();
    textSearch = new TextSearch(this);
    textSearch->hide();

    connect(textSearch, &TextSearch::findNext, this, &CodeEditor::onFindNext);
    connect(textSearch, &TextSearch::findPrev, this, &CodeEditor::onFindPrev);
    connect(textSearch, &TextSearch::replaceOne, this, &CodeEditor::onReplaceOne);
    connect(textSearch, &TextSearch::replaceAllText, this, &CodeEditor::onReplaceAll);
    connect(textSearch, &TextSearch::closed, this, &CodeEditor::onSearchClosed);
    connect(textSearch, &TextSearch::searchTextChanged, this, &CodeEditor::updateSearchHighlights);

    connect(this, SIGNAL(textChanged()), this, SIGNAL(fileModified()));
    connect(this, SIGNAL(cursorPositionChanged(int, int)), this, SLOT(onCursorChanged(int, int)));
    connect(this, SIGNAL(modificationChanged(bool)), this, SLOT(onModified(bool)));
    connect(this,
            SIGNAL(marginClicked(int, int, Qt::KeyboardModifiers)),
            this,
            SLOT(onMarginClicked(int, int, Qt::KeyboardModifiers)));
}

void CodeEditor::setupLexer()
{
    lexer = new QsciLexerPython(this);
    applyDarkTheme();
    setLexer(lexer);
    setUtf8(true);
}

void CodeEditor::setupFonts()
{
    QFont regular("Consolas", 11);
    regular.setFixedPitch(true);
    regular.setStyleHint(QFont::Monospace);
    regular.setBold(false);

    QFont italic = regular;
    italic.setItalic(true);

    setFont(regular);
    lexer->setDefaultFont(regular);

    static const int allStyles[] = {
        QsciLexerPython::Default,
        QsciLexerPython::Comment,
        QsciLexerPython::Number,
        QsciLexerPython::DoubleQuotedString,
        QsciLexerPython::SingleQuotedString,
        QsciLexerPython::Keyword,
        QsciLexerPython::TripleSingleQuotedString,
        QsciLexerPython::TripleDoubleQuotedString,
        QsciLexerPython::ClassName,
        QsciLexerPython::FunctionMethodName,
        QsciLexerPython::Operator,
        QsciLexerPython::Identifier,
        QsciLexerPython::CommentBlock,
        QsciLexerPython::UnclosedString,
        QsciLexerPython::HighlightedIdentifier,
        QsciLexerPython::Decorator,
        QsciLexerPython::DoubleQuotedFString,
        QsciLexerPython::SingleQuotedFString,
        QsciLexerPython::TripleSingleQuotedFString,
        QsciLexerPython::TripleDoubleQuotedFString,
    };
    for (int s : allStyles)
        lexer->setFont(regular, s);

    lexer->setFont(italic, QsciLexerPython::Comment);
    lexer->setFont(italic, QsciLexerPython::CommentBlock);
}

void CodeEditor::setupMargins()
{
    setMarginType(0, QsciScintilla::NumberMargin);
    setMarginWidth(0, "9999");
    setMarginLineNumbers(0, true);

    setMarginType(1, QsciScintilla::SymbolMargin);
    setMarginWidth(1, 16);
    setMarginSensitivity(1, true);
    setMarginMarkerMask(1, (1 << MARKER_BREAKPOINT) | (1 << MARKER_DEBUG_LINE));

    markerDefine(QsciScintilla::Circle, MARKER_BREAKPOINT);
    setMarkerBackgroundColor(QColor("#c51500"), MARKER_BREAKPOINT);
    setMarkerForegroundColor(QColor("#ffffff"), MARKER_BREAKPOINT);

    markerDefine(QsciScintilla::RightArrow, MARKER_DEBUG_LINE);
    setMarkerBackgroundColor(QColor("#ffcc00"), MARKER_DEBUG_LINE);
    setMarkerForegroundColor(QColor("#1e1e1e"), MARKER_DEBUG_LINE);

    markerDefine(QsciScintilla::Background, MARKER_DEBUG_BG);
    setMarkerBackgroundColor(QColor("#2d2800"), MARKER_DEBUG_BG);

    markerDefine(QsciScintilla::Background, MARKER_DIFF_ADDED);
    setMarkerBackgroundColor(QColor("#1a4d1a"), MARKER_DIFF_ADDED);

    markerDefine(QsciScintilla::Background, MARKER_DIFF_REMOVED);
    setMarkerBackgroundColor(QColor("#4d1a1a"), MARKER_DIFF_REMOVED);

    markerDefine(QsciScintilla::Background, MARKER_DIFF_HUNK);
    setMarkerBackgroundColor(QColor("#1e2a3a"), MARKER_DIFF_HUNK);

    indicatorDefine(QsciScintilla::FullBoxIndicator, INDIC_DIFF_CHARS_ADDED);
    setIndicatorForegroundColor(QColor("#3fb950"), INDIC_DIFF_CHARS_ADDED);
    setIndicatorDrawUnder(true, INDIC_DIFF_CHARS_ADDED);
    SendScintilla(SCI_INDICSETALPHA, (unsigned long) INDIC_DIFF_CHARS_ADDED, (long) 100);
    SendScintilla(SCI_INDICSETOUTLINEALPHA, (unsigned long) INDIC_DIFF_CHARS_ADDED, (long) 220);

    indicatorDefine(QsciScintilla::FullBoxIndicator, INDIC_DIFF_CHARS_REMOVED);
    setIndicatorForegroundColor(QColor("#f85149"), INDIC_DIFF_CHARS_REMOVED);
    setIndicatorDrawUnder(true, INDIC_DIFF_CHARS_REMOVED);
    SendScintilla(SCI_INDICSETALPHA, (unsigned long) INDIC_DIFF_CHARS_REMOVED, (long) 100);
    SendScintilla(SCI_INDICSETOUTLINEALPHA, (unsigned long) INDIC_DIFF_CHARS_REMOVED, (long) 220);
}

void CodeEditor::setupEditor()
{
    setFrameShape(QFrame::NoFrame);
    setIndentationsUseTabs(false);
    setIndentationWidth(4);
    setTabWidth(4);
    setTabIndents(true);
    setAutoIndent(true);
    setBackspaceUnindents(true);

    setCaretLineVisible(true);
    setCaretLineBackgroundColor(QColor("#2a2a2a"));
    setCaretWidth(2);

    setBraceMatching(QsciScintilla::SloppyBraceMatch);
    setMatchedBraceBackgroundColor(QColor("#3a3a3a"));
    setMatchedBraceForegroundColor(QColor("#ffd700"));
    setUnmatchedBraceBackgroundColor(QColor("#3a2020"));
    setUnmatchedBraceForegroundColor(QColor("#f44747"));

    setFolding(QsciScintilla::BoxedTreeFoldStyle, 2);
    setFoldMarginColors(QColor("#1e1e1e"), QColor("#1e1e1e"));

    setScrollWidth(1);
    setScrollWidthTracking(true);
    setEolMode(QsciScintilla::EolUnix);
    setWrapMode(QsciScintilla::WrapNone);

    setWhitespaceVisibility(QsciScintilla::WsInvisible);
    setWhitespaceForegroundColor(QColor("#3c3c3c"));

    SendScintilla(SCI_SETMULTIPLESELECTION, 1);
    SendScintilla(SCI_SETADDITIONALSELECTIONTYPING, 1);

    cursorOverlay = new RemoteCursorOverlay(this);

    connect(verticalScrollBar(),
            &QScrollBar::valueChanged,
            cursorOverlay,
            QOverload<>::of(&QWidget::update));
    connect(horizontalScrollBar(),
            &QScrollBar::valueChanged,
            cursorOverlay,
            QOverload<>::of(&QWidget::update));
}

void CodeEditor::onCursorChanged(int line, int index)
{
    emit cursorPositionUpdated(line, index);
}

void CodeEditor::onModified(bool modified)
{
    emit modifyChanged(modified);
}

void CodeEditor::keyPressEvent(QKeyEvent *event)
{
    if (applyingRemote) {
        QsciScintilla::keyPressEvent(event);
        return;
    }

    if (collabActive && isReadOnly()) {
        QsciScintilla::keyPressEvent(event);
        return;
    }

    if (handleBackspaceInPair(event))
        return;
    if (skipClosingChar(event))
        return;
    if (autoCloseChar(event))
        return;

    const Qt::KeyboardModifiers mod = event->modifiers();
    const int key = event->key();

    if (mod == Qt::ControlModifier) {
        if (key == Qt::Key_Z) {
            undo();
            return;
        }
        if (key == Qt::Key_Y) {
            redo();
            return;
        }
        if (mod == Qt::ControlModifier && key == Qt::Key_F) {
            showSearch();
            return;
        }
        if (key == Qt::Key_S) {
            if (!filePath.isEmpty())
                saveFile(filePath);
            return;
        }
        if (key == Qt::Key_Plus || key == Qt::Key_Equal) {
            zoomIn();
            return;
        }
        if (key == Qt::Key_Minus) {
            zoomOut();
            return;
        }
        if (key == Qt::Key_0) {
            resetZoom();
            return;
        }
        if (key == Qt::Key_X) {
            deleteSelection();
            return;
        }
        if (key == Qt::Key_V) {
            if (!selectedText().isEmpty())
                deleteSelection();
            const int startPos = (int) SendScintilla(SCI_GETCURRENTPOS);
            QsciScintilla::keyPressEvent(event);
            const QString clip = QGuiApplication::clipboard()->text();
            emit beginUndoGroup();
            int byteOff = startPos;
            for (QChar c : clip) {
                emit localInsert(byteOff, c);
                byteOff += QString(c).toUtf8().size();
            }
            emit endUndoGroup();
            return;
        }
    }

    if (mod == Qt::NoModifier || mod == Qt::ShiftModifier) {
        QString txt = event->text();
        if (!txt.isEmpty()) {
            QChar ch = txt.at(0);
            if (ch.isPrint() || ch == '\n' || ch == '\r') {
                if (!selectedText().isEmpty())
                    deleteSelection();
                int pos = SendScintilla(SCI_GETCURRENTPOS);
                QChar sendChar = (ch == '\r') ? QChar('\n') : ch;
                suppressLocalInsert = true;
                QsciScintilla::keyPressEvent(event);
                suppressLocalInsert = false;
                emit localInsert(pos, sendChar);

                if (sendChar == '\n') {
                    int newPos = SendScintilla(SCI_GETCURRENTPOS);
                    int indentLen = newPos - (pos + 1);
                    shiftRemoteCursors(pos, 1, /*repaint=*/false);
                    for (int i = 0; i < indentLen; i++) {
                        emit localInsert(pos + 1 + i, QChar(' '));
                        shiftRemoteCursors(pos + 1 + i, 1, /*repaint=*/false);
                    }
                    if (cursorOverlay)
                        cursorOverlay->update();
                } else {
                    shiftRemoteCursors(pos, QString(sendChar).toUtf8().size());
                }
                return;
            }
        }

        if (key == Qt::Key_Backspace && mod == Qt::NoModifier) {
            if (!selectedText().isEmpty()) {
                deleteSelection();
                return;
            }
            int pos = SendScintilla(SCI_GETCURRENTPOS);
            if (pos > 0) {
                const int prevPos = (int) SendScintilla(SCI_POSITIONBEFORE, (ulong) pos);
                const int lenBefore = (int) SendScintilla(SCI_GETLENGTH);
                QsciScintilla::keyPressEvent(event);
                const int newPos = (int) SendScintilla(SCI_GETCURRENTPOS);
                const int bytesDeleted = lenBefore - (int) SendScintilla(SCI_GETLENGTH);

                if (bytesDeleted == pos - prevPos) {
                    emit localDelete(prevPos);
                    shiftRemoteCursors(prevPos, -bytesDeleted);
                } else if (bytesDeleted > 0) {
                    emit beginUndoGroup();
                    for (int i = 0; i < bytesDeleted; ++i)
                        emit localDelete(newPos);
                    emit endUndoGroup();
                    shiftRemoteCursors(newPos, -bytesDeleted);
                }
                return;
            }
        }

        if (key == Qt::Key_Tab && mod == Qt::NoModifier) {
            const int pos = (int) SendScintilla(SCI_GETCURRENTPOS);
            const int lenBefore = (int) SendScintilla(SCI_GETLENGTH);
            suppressLocalInsert = true;
            QsciScintilla::keyPressEvent(event);
            suppressLocalInsert = false;
            const int inserted = (int) SendScintilla(SCI_GETLENGTH) - lenBefore;
            if (inserted > 0) {
                emit beginUndoGroup();
                for (int i = 0; i < inserted; ++i) {
                    emit localInsert(pos + i, QChar(' '));
                    shiftRemoteCursors(pos + i, 1, false);
                }
                emit endUndoGroup();
                if (cursorOverlay)
                    cursorOverlay->update();
            }
            return;
        }

        if (key == Qt::Key_Tab && mod == Qt::ShiftModifier) {
            const int lineNo = (int) SendScintilla(SCI_LINEFROMPOSITION,
                                                   (ulong) SendScintilla(SCI_GETCURRENTPOS));
            const int lineStart = (int) SendScintilla(SCI_POSITIONFROMLINE, (ulong) lineNo);
            const int lenBefore = (int) SendScintilla(SCI_GETLENGTH);
            suppressLocalInsert = true;
            QsciScintilla::keyPressEvent(event);
            suppressLocalInsert = false;
            const int removed = lenBefore - (int) SendScintilla(SCI_GETLENGTH);
            if (removed > 0) {
                emit beginUndoGroup();
                for (int i = 0; i < removed; ++i)
                    emit localDelete(lineStart);
                emit endUndoGroup();
                shiftRemoteCursors(lineStart, -removed);
            }
            return;
        }

        if (key == Qt::Key_Delete && mod == Qt::NoModifier) {
            if (!selectedText().isEmpty()) {
                deleteSelection();
                return;
            }
            int pos = SendScintilla(SCI_GETCURRENTPOS);
            const int nextPos = (int) SendScintilla(SCI_POSITIONAFTER, (ulong) pos);
            const int byteLen = nextPos - pos;
            QsciScintilla::keyPressEvent(event);
            emit localDelete(pos);
            shiftRemoteCursors(pos, -byteLen);
            return;
        }
    }

    QsciScintilla::keyPressEvent(event);
}

bool CodeEditor::autoCloseChar(QKeyEvent *event)
{
    if (event->modifiers() & ~Qt::ShiftModifier)
        return false;

    const QChar ch = event->text().isEmpty() ? QChar() : event->text().at(0);
    if (ch.isNull())
        return false;

    for (const auto &[open, close] : kAutoPairs) {
        if (ch != open)
            continue;

        if (open == close) {
            int line, col;
            getCursorPosition(&line, &col);
            const QString lineText = text(line);
            if (col < lineText.length() && lineText.at(col) == open)
                return false;
        }

        const QString selected = selectedText();
        if (!selected.isEmpty()) {
            const int selStart = (int) SendScintilla(SCI_GETSELECTIONSTART);
            const int openByteLen = QString(open).toUtf8().size();
            const int selByteLen = selected.toUtf8().size();
            const int closeByteLen = QString(close).toUtf8().size();
            replaceSelectedText(QString(open) + selected + QString(close));
            emit localInsert(selStart, open);
            emit localInsert(selStart + openByteLen + selByteLen, close);
            shiftRemoteCursors(selStart, openByteLen);
            shiftRemoteCursors(selStart + openByteLen + selByteLen, closeByteLen);
        } else {
            int pos = SendScintilla(SCI_GETCURRENTPOS);
            suppressLocalInsert = true;
            QsciScintilla::keyPressEvent(event);
            insert(QString(close));
            suppressLocalInsert = false;

            emit localInsert(pos, open);
            emit localInsert(pos + 1, close);
            shiftRemoteCursors(pos, 1);
            shiftRemoteCursors(pos + 1, 1);
        }
        return true;
    }
    return false;
}

bool CodeEditor::skipClosingChar(QKeyEvent *event)
{
    if (event->modifiers() != Qt::NoModifier)
        return false;

    const QChar ch = event->text().isEmpty() ? QChar() : event->text().at(0);
    if (ch.isNull())
        return false;

    for (const auto &[open, close] : kAutoPairs) {
        if (ch != close || open == close)
            continue;

        int line, col;
        getCursorPosition(&line, &col);
        const QString lineText = text(line);
        if (col < lineText.length() && lineText.at(col) == close) {
            setCursorPosition(line, col + 1);
            return true;
        }
    }
    return false;
}

bool CodeEditor::handleBackspaceInPair(QKeyEvent *event)
{
    if (event->key() != Qt::Key_Backspace || event->modifiers() != Qt::NoModifier)
        return false;
    if (!selectedText().isEmpty())
        return false;

    int line, col;
    getCursorPosition(&line, &col);
    if (col == 0)
        return false;

    const QString lineText = text(line);
    const QChar before = lineText.at(col - 1);
    const QChar after = col < lineText.length() ? lineText.at(col) : QChar();

    for (const auto &[open, close] : kAutoPairs) {
        if (before == open && after == close) {
            int pos = SendScintilla(SCI_GETCURRENTPOS);
            setSelection(line, col - 1, line, col + 1);
            removeSelectedText();
            emit localDelete(pos);
            emit localDelete(pos - 1);
            shiftRemoteCursors(pos - 1, -2);
            return true;
        }
    }
    return false;
}

void CodeEditor::setupAutoComplete()
{
    QsciAPIs *apis = new QsciAPIs(lexer);

    const QStringList keywords = {
        "False",        "None",       "True",         "and",        "as",          "assert",
        "async",        "await",      "break",        "class",      "continue",    "def",
        "del",          "elif",       "else",         "except",     "finally",     "for",
        "from",         "global",     "if",           "import",     "in",          "is",
        "lambda",       "nonlocal",   "not",          "or",         "pass",        "raise",
        "return",       "try",        "while",        "with",       "yield",       "abs",
        "all",          "any",        "bool",         "breakpoint", "callable",    "chr",
        "dict",         "dir",        "divmod",       "enumerate",  "eval",        "exec",
        "filter",       "float",      "format",       "frozenset",  "getattr",     "globals",
        "hasattr",      "hash",       "help",         "hex",        "id",          "input",
        "int",          "isinstance", "issubclass",   "iter",       "len",         "list",
        "locals",       "map",        "max",          "min",        "next",        "object",
        "oct",          "open",       "ord",          "pow",        "print",       "property",
        "range",        "repr",       "reversed",     "round",      "set",         "setattr",
        "slice",        "sorted",     "staticmethod", "str",        "sum",         "super",
        "tuple",        "type",       "vars",         "zip",        "self",        "cls",
        "__init__",     "__str__",    "__repr__",     "__len__",    "__getitem__", "__setitem__",
        "__contains__", "__iter__",   "__next__",     "__enter__",  "__exit__",    "__call__",
        "__del__",
    };

    for (const auto &kw : keywords)
        apis->add(kw);

    apis->prepare();
    lexer->setAPIs(apis);

    setAutoCompletionSource(QsciScintilla::AcsAPIs);
    setAutoCompletionThreshold(2);
    setAutoCompletionCaseSensitivity(false);
    setAutoCompletionReplaceWord(true);
    setAutoCompletionUseSingle(QsciScintilla::AcusExplicit);
    setCallTipsStyle(QsciScintilla::CallTipsNone);
}

void CodeEditor::setupLinter()
{
    indicatorDefine(QsciScintilla::SquiggleIndicator, ErrorIndicator);
    setIndicatorForegroundColor(QColor("#f44747"), ErrorIndicator);

    indicatorDefine(QsciScintilla::BoxIndicator, SEARCH_INDICATOR);
    setIndicatorForegroundColor(QColor("#d7ba7d"), SEARCH_INDICATOR);
    setIndicatorOutlineColor(QColor("#d7ba7d"), SEARCH_INDICATOR);

    lintTimer = new QTimer(this);
    lintTimer->setSingleShot(true);
    lintTimer->setInterval(800);

    connect(lintTimer, &QTimer::timeout, this, &CodeEditor::checkSyntax);

    connect(this, SIGNAL(textChanged()), lintTimer, SLOT(start()));
}

void CodeEditor::checkSyntax()
{
    if (lintProcess) {
        if (lintProcess->state() != QProcess::NotRunning) {
            lintProcess->kill();
            lintProcess->waitForFinished(300);
        }
        delete lintProcess;
        lintProcess = nullptr;
    }

    clearIndicatorRange(0, 0, lines(), 0, ErrorIndicator);

    QString tmpPath = QDir::tempPath() + "/teamhub_lint_tmp.py";
    QFile tmpFile(tmpPath);
    if (!tmpFile.open(QIODevice::WriteOnly | QIODevice::Text))
        return;
    QTextStream stream(&tmpFile);
    stream.setEncoding(QStringConverter::Utf8);
    stream << text();
    tmpFile.close();

    lintProcess = new QProcess(this);
    lintProcess->setProcessChannelMode(QProcess::SeparateChannels);

    connect(lintProcess,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            &CodeEditor::onLintFinished);

    lintProcess->start("python",
                       {"-m", "ruff", "check", "--output-format=concise", "--select=E,F,W", tmpPath});

    if (!lintProcess->waitForStarted(3000)) {
        delete lintProcess;
        lintProcess = nullptr;
        QFile::remove(tmpPath);
        return;
    }
}

void CodeEditor::onLintFinished(int exitCode, QProcess::ExitStatus status)
{
    Q_UNUSED(status)
    if (!lintProcess)
        return;

    const QString out = QString::fromUtf8(lintProcess->readAllStandardOutput()).trimmed();
    const QString err = QString::fromUtf8(lintProcess->readAllStandardError()).trimmed();

    lintProcess->deleteLater();
    lintProcess = nullptr;

    QString tmpPath = QDir::tempPath() + "/teamhub_lint_tmp.py";
    QFile::remove(tmpPath);

    QString combined;
    if (!out.isEmpty() && !err.isEmpty())
        combined = out + "\n" + err;
    else
        combined = out.isEmpty() ? err : out;

    errorList.clear();

    if (combined.isEmpty())
        return;

    static const QRegularExpression re(R"([^:]+:(\d+):(\d+):\s*([\w-]+(?:\d*):\s*.+))");

    for (const QString &rawLine : combined.split('\n')) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty())
            continue;

        const auto match = re.match(line);
        if (!match.hasMatch())
            continue;

        const int ln = match.captured(1).toInt() - 1;
        const int col = std::max(0, match.captured(2).toInt() - 1);
        const QString msg = match.captured(3).trimmed();

        if (ln < 0 || ln >= lines())
            continue;

        const int len = std::max(1, lineLength(ln) - col - 1);
        fillIndicatorRange(ln, col, ln, col + len, ErrorIndicator);

        errorList.append({ln, col, msg});
    }
}

void CodeEditor::mouseMoveEvent(QMouseEvent *event)
{
    QsciScintilla::mouseMoveEvent(event);

    const int pos = SendScintilla(SCI_POSITIONFROMPOINT, event->pos().x(), event->pos().y());

    const int indicators = SendScintilla(SCI_INDICATORALLONFOR, pos);

    if (indicators & (1 << ErrorIndicator)) {
        int line, col;
        lineIndexFromPosition(pos, &line, &col);

        const ErrorInfo *closest = nullptr;
        int minDist = INT_MAX;

        for (const ErrorInfo &err : std::as_const(errorList)) {
            if (err.line != line)
                continue;

            int dist = std::abs(col - err.col);
            if (dist < minDist) {
                minDist = dist;
                closest = &err;
            }
        }

        if (closest) {
            QToolTip::showText(event->globalPosition().toPoint(), closest->message, this);
            return;
        }
    }

    QToolTip::hideText();
}

void CodeEditor::loadFile(const QString &filepath)
{
    QFile file(filepath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return;

    QTextStream in(&file);
    in.setEncoding(QStringConverter::Utf8);
    setText(in.readAll());
    file.close();

    filePath = filepath;
    setModified(false);
}

void CodeEditor::saveFile(const QString &filepath)
{
    QFile file(filepath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return;

    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    out << text();
    file.close();

    filePath = filepath;
    setModified(false);
    emit fileSaved();
}

void CodeEditor::setFilePath(const QString &filepath)
{
    filePath = filepath;
}
QString CodeEditor::getFilePath() const
{
    return filePath;
}

void CodeEditor::applyEditorFont(const QFont &font)
{
    setFont(font);
    setMarginsFont(font);
    if (lexer) {
        lexer->setFont(font);
        setLexer(lexer);
    }
}

void CodeEditor::setTheme(Theme t)
{
    theme = t;
    if (theme == Theme::Dark)
        applyDarkTheme();
    else
        applyLightTheme();
    setupFonts();
}

void CodeEditor::applyDarkTheme()
{
    if (!lexer)
        return;

    const QColor bg("#1e1e1e");
    const QColor fg("#d4d4d4");

    lexer->setDefaultPaper(bg);
    lexer->setDefaultColor(fg);
    static const int allStyles[] = {
        QsciLexerPython::Default,
        QsciLexerPython::Comment,
        QsciLexerPython::Number,
        QsciLexerPython::DoubleQuotedString,
        QsciLexerPython::SingleQuotedString,
        QsciLexerPython::Keyword,
        QsciLexerPython::TripleSingleQuotedString,
        QsciLexerPython::TripleDoubleQuotedString,
        QsciLexerPython::ClassName,
        QsciLexerPython::FunctionMethodName,
        QsciLexerPython::Operator,
        QsciLexerPython::Identifier,
        QsciLexerPython::CommentBlock,
        QsciLexerPython::UnclosedString,
        QsciLexerPython::HighlightedIdentifier,
        QsciLexerPython::Decorator,
        QsciLexerPython::DoubleQuotedFString,
        QsciLexerPython::SingleQuotedFString,
        QsciLexerPython::TripleSingleQuotedFString,
        QsciLexerPython::TripleDoubleQuotedFString,
    };
    for (int s : allStyles)
        lexer->setPaper(bg, s);

    lexer->setColor(fg, QsciLexerPython::Default);
    lexer->setColor(QColor("#569cd6"), QsciLexerPython::Keyword);
    lexer->setColor(QColor("#ce9178"), QsciLexerPython::SingleQuotedString);
    lexer->setColor(QColor("#ce9178"), QsciLexerPython::DoubleQuotedString);
    lexer->setColor(QColor("#ce9178"), QsciLexerPython::TripleSingleQuotedString);
    lexer->setColor(QColor("#ce9178"), QsciLexerPython::TripleDoubleQuotedString);
    lexer->setColor(QColor("#ce9178"), QsciLexerPython::UnclosedString);
    lexer->setColor(QColor("#ce9178"), QsciLexerPython::SingleQuotedFString);
    lexer->setColor(QColor("#ce9178"), QsciLexerPython::DoubleQuotedFString);
    lexer->setColor(QColor("#ce9178"), QsciLexerPython::TripleSingleQuotedFString);
    lexer->setColor(QColor("#ce9178"), QsciLexerPython::TripleDoubleQuotedFString);
    lexer->setColor(QColor("#6a9955"), QsciLexerPython::Comment);
    lexer->setColor(QColor("#6a9955"), QsciLexerPython::CommentBlock);
    lexer->setColor(QColor("#b5cea8"), QsciLexerPython::Number);
    lexer->setColor(QColor("#dcdcaa"), QsciLexerPython::FunctionMethodName);
    lexer->setColor(QColor("#4ec9b0"), QsciLexerPython::ClassName);
    lexer->setColor(fg, QsciLexerPython::Operator);
    lexer->setColor(fg, QsciLexerPython::Identifier);
    lexer->setColor(QColor("#c586c0"), QsciLexerPython::Decorator);
    lexer->setColor(QColor("#9cdcfe"), QsciLexerPython::HighlightedIdentifier);

    setPaper(bg);
    setColor(fg);
    setCaretForegroundColor(QColor("#aeafad"));
    setCaretLineBackgroundColor(QColor("#282828"));
    setSelectionBackgroundColor(QColor("#264f78"));
    setSelectionForegroundColor(QColor("#ffffff"));
    setMarginsBackgroundColor(QColor("#1e1e1e"));
    setMarginsForegroundColor(QColor("#858585"));
    setFoldMarginColors(QColor("#1e1e1e"), QColor("#1e1e1e"));
    setMatchedBraceBackgroundColor(QColor("#3a3a3a"));
    setMatchedBraceForegroundColor(QColor("#ffd700"));
    setUnmatchedBraceBackgroundColor(QColor("#3a2020"));
    setUnmatchedBraceForegroundColor(QColor("#f44747"));
}

void CodeEditor::applyLightTheme()
{
    if (!lexer)
        return;

    const QColor bg("#ffffff");
    const QColor fg("#000000");

    lexer->setDefaultPaper(bg);
    lexer->setDefaultColor(fg);
    static const int allStyles[] = {
        QsciLexerPython::Default,
        QsciLexerPython::Comment,
        QsciLexerPython::Number,
        QsciLexerPython::DoubleQuotedString,
        QsciLexerPython::SingleQuotedString,
        QsciLexerPython::Keyword,
        QsciLexerPython::TripleSingleQuotedString,
        QsciLexerPython::TripleDoubleQuotedString,
        QsciLexerPython::ClassName,
        QsciLexerPython::FunctionMethodName,
        QsciLexerPython::Operator,
        QsciLexerPython::Identifier,
        QsciLexerPython::CommentBlock,
        QsciLexerPython::UnclosedString,
        QsciLexerPython::HighlightedIdentifier,
        QsciLexerPython::Decorator,
        QsciLexerPython::DoubleQuotedFString,
        QsciLexerPython::SingleQuotedFString,
        QsciLexerPython::TripleSingleQuotedFString,
        QsciLexerPython::TripleDoubleQuotedFString,
    };
    for (int s : allStyles)
        lexer->setPaper(bg, s);

    lexer->setColor(fg, QsciLexerPython::Default);
    lexer->setColor(QColor("#0000ff"), QsciLexerPython::Keyword);
    lexer->setColor(QColor("#a31515"), QsciLexerPython::SingleQuotedString);
    lexer->setColor(QColor("#a31515"), QsciLexerPython::DoubleQuotedString);
    lexer->setColor(QColor("#a31515"), QsciLexerPython::TripleSingleQuotedString);
    lexer->setColor(QColor("#a31515"), QsciLexerPython::TripleDoubleQuotedString);
    lexer->setColor(QColor("#a31515"), QsciLexerPython::UnclosedString);
    lexer->setColor(QColor("#a31515"), QsciLexerPython::SingleQuotedFString);
    lexer->setColor(QColor("#a31515"), QsciLexerPython::DoubleQuotedFString);
    lexer->setColor(QColor("#a31515"), QsciLexerPython::TripleSingleQuotedFString);
    lexer->setColor(QColor("#a31515"), QsciLexerPython::TripleDoubleQuotedFString);
    lexer->setColor(QColor("#008000"), QsciLexerPython::Comment);
    lexer->setColor(QColor("#008000"), QsciLexerPython::CommentBlock);
    lexer->setColor(QColor("#098658"), QsciLexerPython::Number);
    lexer->setColor(QColor("#795e26"), QsciLexerPython::FunctionMethodName);
    lexer->setColor(QColor("#267f99"), QsciLexerPython::ClassName);
    lexer->setColor(fg, QsciLexerPython::Operator);
    lexer->setColor(fg, QsciLexerPython::Identifier);
    lexer->setColor(QColor("#af00db"), QsciLexerPython::Decorator);
    lexer->setColor(QColor("#001080"), QsciLexerPython::HighlightedIdentifier);

    setPaper(bg);
    setColor(fg);
    setCaretForegroundColor(QColor("#000000"));
    setCaretLineBackgroundColor(QColor("#f0f0f0"));
    setSelectionBackgroundColor(QColor("#add6ff"));
    setSelectionForegroundColor(QColor("#000000"));
    setMarginsBackgroundColor(QColor("#f3f3f3"));
    setMarginsForegroundColor(QColor("#237893"));
    setFoldMarginColors(QColor("#f3f3f3"), QColor("#f3f3f3"));
    setMatchedBraceBackgroundColor(QColor("#e8e8e8"));
    setMatchedBraceForegroundColor(QColor("#0000cc"));
    setUnmatchedBraceBackgroundColor(QColor("#ffe0e0"));
    setUnmatchedBraceForegroundColor(QColor("#cc0000"));
}

int CodeEditor::currentLine() const
{
    int line, col;
    getCursorPosition(&line, &col);
    return line;
}

int CodeEditor::currentColumn() const
{
    int line, col;
    getCursorPosition(&line, &col);
    return col;
}

bool CodeEditor::isModified() const
{
    return QsciScintilla::isModified();
}

void CodeEditor::resetZoom()
{
    zoomTo(0);
    zoomLevel = 0;
}

void CodeEditor::undo()
{
    if (collabActive)
        emit undoRequested();
    else
        QsciScintilla::undo();
}

void CodeEditor::redo()
{
    if (collabActive)
        emit redoRequested();
    else
        QsciScintilla::redo();
}

void CodeEditor::onCharAdded(int ch)
{
    if (applyingRemote || suppressLocalInsert)
        return;

    int pos = SendScintilla(SCI_GETCURRENTPOS);

    if (ch == 0)
        return;

    QChar addedChar = QChar(ch);

    if (addedChar.isPrint()) {
        emit localInsert(pos, addedChar);
    }
}

void CodeEditor::applyRemoteText(const QString &newText)
{
    applyingRemote = true;
    blockSignals(true);

    const int firstVisLine = (int) SendScintilla(SCI_GETFIRSTVISIBLELINE);

    const int oldBytePos = (int) SendScintilla(SCI_GETCURRENTPOS);

    const QByteArray oldBytes = text().toUtf8();
    const QByteArray newBytes = newText.toUtf8();
    const int oldLen = oldBytes.size();
    const int newLen = newBytes.size();

    int changeAt = qMin(oldLen, newLen);
    for (int i = 0, n = changeAt; i < n; ++i) {
        if ((unsigned char) oldBytes.at(i) != (unsigned char) newBytes.at(i)) {
            changeAt = i;
            break;
        }
    }

    const int byteDelta = newLen - oldLen;
    int newBytePos;
    if (oldBytePos <= changeAt) {
        newBytePos = oldBytePos;
    } else if (byteDelta >= 0) {
        newBytePos = oldBytePos + byteDelta;
    } else {
        newBytePos = qMax(changeAt, oldBytePos + byteDelta);
    }
    newBytePos = qBound(0, newBytePos, newLen);

    setText(newText);

    SendScintilla(SCI_SETCURRENTPOS, (ulong) newBytePos);
    SendScintilla(SCI_SETANCHOR, (ulong) newBytePos);

    SendScintilla(SCI_SETFIRSTVISIBLELINE, (ulong) firstVisLine);

    if (cursorOverlay)
        cursorOverlay->update();

    blockSignals(false);
    applyingRemote = false;
}

void CodeEditor::showSearch()
{
    repositionSearch();
    textSearch->show();
    textSearch->raise();
    textSearch->focusFind();
}

void CodeEditor::hideSearch()
{
    textSearch->hide();
    setFocus();
    clearIndicatorRange(0, 0, lines(), 0, SEARCH_INDICATOR);
    searchMatches.clear();
}

void CodeEditor::repositionSearch()
{
    const int margin = 8;
    const int w = 520;
    const int h = textSearch->sizeHint().height() + 10;
    textSearch->setFixedWidth(w);
    int x = width() - w - margin;
    int y = margin;
    textSearch->setGeometry(x, y, w, h);
}

void CodeEditor::resizeEvent(QResizeEvent *event)
{
    QsciScintilla::resizeEvent(event);
    if (cursorOverlay)
        cursorOverlay->resize(viewport()->size());
    if (textSearch->isVisible())
        repositionSearch();
}

void CodeEditor::onSearchClosed()
{
    setFocus();
    clearIndicatorRange(0, 0, lines(), 0, SEARCH_INDICATOR);
    searchMatches.clear();
}

void CodeEditor::updateSearchHighlights(const QString &searchText)
{
    clearIndicatorRange(0, 0, lines(), 0, SEARCH_INDICATOR);

    searchMatches.clear();

    if (searchText.isEmpty())
        return;

    const QString src = text();

    int pos = 0;

    while ((pos = src.indexOf(searchText, pos, Qt::CaseInsensitive)) != -1) {
        searchMatches.append(pos);
        pos += searchText.length();
    }

    for (int p : searchMatches) {
        int line, col;
        lineIndexFromPosition(p, &line, &col);

        int eline, ecol;
        lineIndexFromPosition(p + searchText.length(), &eline, &ecol);

        fillIndicatorRange(line, col, eline, ecol, SEARCH_INDICATOR);
    }

    searchCurrentIndex = -1;
}

void CodeEditor::onFindNext(const QString &searchText)
{
    if (searchText.isEmpty())
        return;

    if (searchMatches.isEmpty())
        updateSearchHighlights(searchText);

    if (searchMatches.isEmpty())
        return;

    searchCurrentIndex = (searchCurrentIndex + 1) % searchMatches.size();

    selectCurrentMatch(searchText);
}

void CodeEditor::onFindPrev(const QString &searchText)
{
    if (searchText.isEmpty())
        return;

    if (searchMatches.isEmpty())
        updateSearchHighlights(searchText);

    if (searchMatches.isEmpty())
        return;

    searchCurrentIndex = (searchCurrentIndex - 1 + searchMatches.size()) % searchMatches.size();

    selectCurrentMatch(searchText);
}

void CodeEditor::onReplaceOne(const QString &findText, const QString &replaceText)
{
    if (findText.isEmpty())
        return;
    if (selectedText().compare(findText, Qt::CaseInsensitive) == 0)
        replaceSelectedText(replaceText);
    onFindNext(findText);
}

void CodeEditor::onReplaceAll(const QString &findText, const QString &replaceText)
{
    if (findText.isEmpty())
        return;
    QString src = text();
    src.replace(findText, replaceText, Qt::CaseInsensitive);
    setText(src);
    clearIndicatorRange(0, 0, lines(), 0, SEARCH_INDICATOR);
    searchMatches.clear();
}

void CodeEditor::selectCurrentMatch(const QString &searchText)
{
    if (searchMatches.isEmpty())
        return;

    int pos = searchMatches[searchCurrentIndex];

    int line, col;
    lineIndexFromPosition(pos, &line, &col);

    int eline, ecol;
    lineIndexFromPosition(pos + searchText.length(), &eline, &ecol);

    setSelection(line, col, eline, ecol);

    ensureLineVisible(line);

    textSearch->updateMatchLabel(searchCurrentIndex + 1, searchMatches.size());
}

void CodeEditor::deleteSelection()
{
    const QString sel = selectedText();
    if (sel.isEmpty())
        return;
    const int selStart = (int) SendScintilla(SCI_GETSELECTIONSTART);
    const int selEnd = (int) SendScintilla(SCI_GETSELECTIONEND);
    emit beginUndoGroup();
    removeSelectedText();
    for (int i = 0; i < sel.size(); ++i)
        emit localDelete(selStart);
    emit endUndoGroup();
    shiftRemoteCursors(selStart, -(selEnd - selStart));
}

void CodeEditor::shiftRemoteCursors(int fromBytePos, int byteDelta, bool repaint)
{
    if (remoteCursorPositions.isEmpty())
        return;
    bool changed = false;
    for (auto it = remoteCursorPositions.begin(); it != remoteCursorPositions.end(); ++it) {
        if (byteDelta > 0) {
            if (it.value() >= fromBytePos) {
                it.value() += byteDelta;
                changed = true;
            }
        } else {
            if (it.value() > fromBytePos) {
                it.value() = qMax(fromBytePos, it.value() + byteDelta);
                changed = true;
            }
        }
    }
    if (changed && repaint && cursorOverlay)
        cursorOverlay->update();
}

void CodeEditor::updateRemoteCursor(int siteId, int scintillaPos)
{
    remoteCursorPositions[siteId] = scintillaPos;
    if (cursorOverlay)
        cursorOverlay->update();
}

void CodeEditor::removeRemoteCursor(int siteId)
{
    remoteCursorPositions.remove(siteId);
    if (cursorOverlay)
        cursorOverlay->update();
}

void CodeEditor::clearRemoteCursors()
{
    remoteCursorPositions.clear();
    remoteCursorNames.clear();
    if (cursorOverlay)
        cursorOverlay->update();
}

void CodeEditor::setRemotePeerName(int siteId, const QString &name)
{
    remoteCursorNames[siteId] = name;
    if (cursorOverlay)
        cursorOverlay->update();
}

int CodeEditor::remoteCursorPos(int siteId) const
{
    return remoteCursorPositions.value(siteId, -1);
}

void CodeEditor::goToScintillaPos(int pos)
{
    if (pos < 0)
        return;
    int line = (int) SendScintilla(SCI_LINEFROMPOSITION, (ulong) pos);
    int col = (int) SendScintilla(SCI_GETCOLUMN, (ulong) pos);
    setCursorPosition(line, col);
    ensureLineVisible(line);
}

void CodeEditor::paintRemoteCursors(QWidget *overlay)
{
    if (remoteCursorPositions.isEmpty())
        return;

    QPainter painter(overlay);
    painter.setRenderHint(QPainter::Antialiasing, false);

    QFont labelFont = painter.font();
    labelFont.setPointSize(7);
    painter.setFont(labelFont);
    QFontMetrics fm(labelFont);

    const int docLen = (int) SendScintilla(SCI_GETLENGTH);

    for (auto it = remoteCursorPositions.constBegin(); it != remoteCursorPositions.constEnd();
         ++it) {
        const int siteId = it.key();
        const int sciPos = qBound(0, it.value(), docLen);
        const QColor color = kCursorColors[std::abs(siteId) % 4];

        const int x = (int) SendScintilla(SCI_POINTXFROMPOSITION, 0UL, (long) sciPos);
        const int y = (int) SendScintilla(SCI_POINTYFROMPOSITION, 0UL, (long) sciPos);
        const int line = (int) SendScintilla(SCI_LINEFROMPOSITION, (ulong) sciPos);
        const int lineHeight = (int) SendScintilla(SCI_TEXTHEIGHT, (ulong) line);

        if (y + lineHeight < 0 || y > overlay->height())
            continue;

        painter.setPen(QPen(color, 2));
        painter.drawLine(x, y, x, y + lineHeight);

        const QString rawName = remoteCursorNames.value(siteId);
        const QString label = rawName.isEmpty() ? QString("U%1").arg(siteId) : rawName.left(12);
        const int labelW = fm.horizontalAdvance(label) + 6;
        const int labelH = fm.height() + 2;
        const int labelY = y - labelH;

        if (labelY >= 0) {
            painter.fillRect(x, labelY, labelW, labelH, color);
            painter.setPen(Qt::white);
            painter.drawText(QRect(x, labelY, labelW, labelH), Qt::AlignCenter, label);
        }
    }
}

void CodeEditor::onMarginClicked(int margin, int line, Qt::KeyboardModifiers)
{
    if (margin == 1)
        toggleBreakpoint(line);
}

void CodeEditor::toggleBreakpoint(int line)
{
    if (breakpointSet.contains(line)) {
        markerDelete(line, MARKER_BREAKPOINT);
        breakpointSet.remove(line);
    } else {
        if (text(line).trimmed().isEmpty())
            return;
        markerAdd(line, MARKER_BREAKPOINT);
        breakpointSet.insert(line);
    }
    emit breakpointsChanged(breakpointSet);
}

void CodeEditor::setDebugLine(int line)
{
    clearDebugLine();
    debugLine = line;
    markerAdd(line, MARKER_DEBUG_LINE);
    markerAdd(line, MARKER_DEBUG_BG);
    ensureLineVisible(line);
}

void CodeEditor::clearDebugLine()
{
    if (debugLine >= 0) {
        markerDelete(debugLine, MARKER_DEBUG_LINE);
        markerDelete(debugLine, MARKER_DEBUG_BG);
        debugLine = -1;
    }
}

void CodeEditor::clearDiffMarkers()
{
    markerDeleteAll(MARKER_DIFF_ADDED);
    markerDeleteAll(MARKER_DIFF_REMOVED);
    markerDeleteAll(MARKER_DIFF_HUNK);
    SendScintilla(SCI_SETINDICATORCURRENT, (unsigned long) INDIC_DIFF_CHARS_ADDED);
    SendScintilla(SCI_INDICATORCLEARRANGE, 0UL, (long) length());
    SendScintilla(SCI_SETINDICATORCURRENT, (unsigned long) INDIC_DIFF_CHARS_REMOVED);
    SendScintilla(SCI_INDICATORCLEARRANGE, 0UL, (long) length());
}

void CodeEditor::applyDiffText(const QString &raw)
{
    setReadOnly(false);
    setText(raw);
    setReadOnly(true);
    clearDiffMarkers();

    const QStringList diffLines = raw.split('\n');
    QStringList pendingDels;
    QList<int> pendingDelLineNos;

    auto pairWithAdd = [&](int addedLineNo, const QString &newText) {
        if (pendingDels.isEmpty())
            return;
        const QString oldText = pendingDels.takeFirst();
        const int delLineNo = pendingDelLineNos.takeFirst();

        int colStart = 0;
        const int minLen = qMin(oldText.size(), newText.size());
        while (colStart < minLen && oldText[colStart] == newText[colStart])
            ++colStart;

        int oldSuf = 0, newSuf = 0;
        while (newSuf < (newText.size() - colStart) && oldSuf < (oldText.size() - colStart)
               && oldText[oldText.size() - 1 - oldSuf] == newText[newText.size() - 1 - newSuf]) {
            ++oldSuf;
            ++newSuf;
        }

        const int remStart = colStart + 1;
        const int remEnd = oldText.size() - oldSuf + 1;
        const int addStart = colStart + 1;
        const int addEnd = newText.size() - newSuf + 1;

        if (remStart < remEnd)
            fillIndicatorRange(delLineNo, remStart, delLineNo, remEnd, INDIC_DIFF_CHARS_REMOVED);
        if (addStart < addEnd)
            fillIndicatorRange(addedLineNo, addStart, addedLineNo, addEnd, INDIC_DIFF_CHARS_ADDED);
    };

    for (int i = 0; i < diffLines.size(); ++i) {
        const QString &ln = diffLines[i];
        if (ln.isEmpty())
            continue;
        const QChar ch = ln[0];
        if (ln.startsWith("@@")) {
            pendingDels.clear();
            pendingDelLineNos.clear();
            markerAdd(i, MARKER_DIFF_HUNK);
        } else if (ch == '+' && !ln.startsWith("+++")) {
            markerAdd(i, MARKER_DIFF_ADDED);
            pairWithAdd(i, ln.mid(1));
        } else if (ch == '-' && !ln.startsWith("---")) {
            markerAdd(i, MARKER_DIFF_REMOVED);
            pendingDels.append(ln.mid(1));
            pendingDelLineNos.append(i);
        } else if (ch == ' ') {
            pendingDels.clear();
            pendingDelLineNos.clear();
        }
    }
}