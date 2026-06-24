#pragma once

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QObject>
#include <QProcess>
#include <QString>

struct LspCompletionItem {
    QString label;
    QString insertText;
    QString sortText;
    int kind = 0; // 2=Method, 3=Function, 6=Variable, 7=Class, 9=Module
};

class LspClient : public QObject
{
    Q_OBJECT
public:
    explicit LspClient(QObject *parent = nullptr);
    ~LspClient();

    void start(const QString &rootPath);
    void stop();
    bool isRunning() const;
    bool isInitialized() const;

    void didOpen(const QString &path, const QString &text);
    void didChange(const QString &path, const QString &text, int version);
    void didClose(const QString &path);
    void requestCompletion(const QString &path, int line, int col);
    void requestHover(const QString &path, int line, int col);

signals:
    void initialized();
    void completionReady(const QList<LspCompletionItem> &items);
    void hoverReady(const QString &content);

private slots:
    void onReadyRead();

private:
    void send(const QJsonObject &obj);
    void dispatch(const QJsonObject &obj);
    static QString toUri(const QString &path);

    QProcess *proc = nullptr;
    QByteArray buf;
    int nextId = 1;
    bool ready = false;
    QMap<int, QString> pending;
};
