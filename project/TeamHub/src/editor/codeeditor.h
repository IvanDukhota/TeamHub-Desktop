#ifndef CODEEDITOR_H
#define CODEEDITOR_H

#include <Qsci/qscilexerpython.h>
#include <Qsci/qsciscintilla.h>

#include <QKeyEvent>
#include <QMouseEvent>
#include <QProcess>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QToolTip>
#include <QWidget>

#include "lspclient.h"
#include "textsearch.h"

class CodeEditor : public QsciScintilla
{
    Q_OBJECT

public:
    enum class Theme { Dark, Light };

    static constexpr int MARKER_BREAKPOINT = 0;
    static constexpr int MARKER_DEBUG_LINE = 1;
    static constexpr int MARKER_DEBUG_BG = 2;
    static constexpr int MARKER_DIFF_ADDED = 3;
    static constexpr int MARKER_DIFF_REMOVED = 4;
    static constexpr int MARKER_DIFF_HUNK = 5;
    static constexpr int INDIC_DIFF_CHARS_ADDED = 8;
    static constexpr int INDIC_DIFF_CHARS_REMOVED = 9;

    explicit CodeEditor(QWidget *parent = nullptr);

    void loadFile(const QString &filepath);
    void saveFile(const QString &filepath);
    void setFilePath(const QString &filepath);
    QString getFilePath() const;

    void setTheme(Theme theme);
    void applyEditorFont(const QFont &font);

    bool isModified() const;
    int currentLine() const;
    int currentColumn() const;
    void applyRemoteText(const QString &text);

    void showSearch();
    void hideSearch();

    void toggleBreakpoint(int line);
    void setDebugLine(int line);
    void clearDebugLine();
    void applyDiffText(const QString &raw);
    void clearDiffMarkers();
    const QSet<int> &breakpoints() const { return breakpointSet; }

    bool applyingRemote = false;
    bool collabActive = false;
    bool suppressLocalInsert = false;

public slots:
    void setLspClient(LspClient *client);
    void resetZoom();
    void undo();
    void redo();
    void onCursorChanged(int line, int index);
    void applyDarkTheme();
    void applyLightTheme();
    void onModified(bool modified);
    void onCharAdded(int ch);

    void onMarginClicked(int margin, int line, Qt::KeyboardModifiers state);

    void onFindNext(const QString &text);
    void onFindPrev(const QString &text);
    void onReplaceOne(const QString &find, const QString &replace);
    void onReplaceAll(const QString &find, const QString &replace);
    void onSearchClosed();
    void repositionSearch();

    void updateRemoteCursor(int siteId, int scintillaPos);
    void removeRemoteCursor(int siteId);
    void clearRemoteCursors();
    void setRemotePeerName(int siteId, const QString &name);
    void paintRemoteCursors(QWidget *overlay);

    int remoteCursorPos(int siteId) const;
    void goToScintillaPos(int pos);

signals:
    void fileModified();
    void fileSaved();
    void cursorPositionUpdated(int line, int index);
    void modifyChanged(bool modified);
    void localInsert(int position, QChar ch);
    void localDelete(int position);
    void undoRequested();
    void redoRequested();
    void beginUndoGroup();
    void endUndoGroup();
    void breakpointsChanged(const QSet<int> &lines);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

private slots:
    void onAutoCompleted(const char *sel, int pos, int ch, int method);

private:
    struct ErrorInfo
    {
        int line;
        int col;
        QString message;
    };
    QList<ErrorInfo> errorList;

    QMap<int, int> remoteCursorPositions;
    QMap<int, QString> remoteCursorNames;
    QWidget *cursorOverlay = nullptr;
    static const QColor kCursorColors[4];

    void setupLexer();
    void setupFonts();
    void setupMargins();
    void setupEditor();
    void setupAutoComplete();
    void setupLinter();

    bool autoCloseChar(QKeyEvent *event);
    bool skipClosingChar(QKeyEvent *event);
    bool handleBackspaceInPair(QKeyEvent *event);

    void deleteSelection();
    void shiftRemoteCursors(int fromBytePos, int byteDelta, bool repaint = true);

    void onCompletionReady(const QList<LspCompletionItem> &items);
    void onHoverReady(const QString &content);
    void onHoverTimeout();

    void notifyLspChange();
    void requestLspCompletion();

    void checkSyntax();
    void onLintFinished(int exitCode, QProcess::ExitStatus);

    static constexpr int ErrorIndicator = 8;
    static constexpr int SEARCH_INDICATOR = 9;
    static constexpr int CURRENT_SEARCH_INDICATOR = 10;

    LspClient *lspClient = nullptr;
    QTimer *lspChangeTimer = nullptr;
    QTimer *lspCompleteTimer = nullptr;
    QTimer *hoverTimer = nullptr;
    int lspVersion = 1;
    int autocWordLen = 0;
    QPoint hoverViewportPos;
    QPoint hoverGlobalPos;

    QString filePath;
    QsciLexerPython *lexer;
    Theme theme;
    int zoomLevel;
    QProcess *lintProcess;
    QTimer *lintTimer;
    QSet<int> breakpointSet;
    int debugLine = -1;

    TextSearch *textSearch;
    void updateSearchHighlights(const QString &text);
    void selectCurrentMatch(const QString &searchText);
    int searchCurrentIndex = 0;
    QList<int> searchMatches;
};

#endif // CODEEDITOR_H
