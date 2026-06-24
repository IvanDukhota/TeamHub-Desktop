#pragma once

#include <QFrame>
#include <QListWidget>
#include <QStringList>

class CompletionPopup : public QFrame
{
    Q_OBJECT
public:
    explicit CompletionPopup(QWidget *viewportParent);

    void showAt(QPoint viewportPos, const QStringList &items, int wordLen);

    bool isActive() const;
    void navigateDown();
    void navigateUp();
    void acceptCurrent();
    void dismiss();

    int currentWordLen() const { return wLen; }

signals:
    void accepted(const QString &text, int wordLen);

private:
    QListWidget *list;
    int wLen = 0;
};
