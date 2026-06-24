#include "completionpopup.h"

#include <QVBoxLayout>

static constexpr int kMaxVisible = 8;
static constexpr int kPopupWidth = 260;

CompletionPopup::CompletionPopup(QWidget *viewportParent)
    : QFrame(viewportParent)
{
    setAutoFillBackground(true);
    setLineWidth(1);
    setFrameShape(QFrame::Box);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);

    list = new QListWidget(this);
    list->setFocusPolicy(Qt::NoFocus);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(list);

    setStyleSheet(R"(
        QFrame {
            background: #252526;
            border: 1px solid #454545;
        }
        QListWidget {
            background: #252526;
            color: #d4d4d4;
            border: none;
            outline: none;
            font-family: Consolas;
            font-size: 11pt;
        }
        QListWidget::item {
            padding: 1px 6px;
        }
        QListWidget::item:selected {
            background: #094771;
            color: #ffffff;
        }
        QListWidget::item:hover:!selected {
            background: #2a2d2e;
        }
    )");

    connect(list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *) {
        acceptCurrent();
    });

    hide();
}

void CompletionPopup::showAt(QPoint viewportPos, const QStringList &items, int wordLen)
{
    if (items.isEmpty()) {
        hide();
        return;
    }

    list->clear();
    for (const QString &item : items)
        list->addItem(item);
    list->setCurrentRow(0);

    wLen = wordLen;

    const int itemH = list->sizeHintForRow(0) + 2;
    const int visible = qMin(items.count(), kMaxVisible);
    const int popupH = visible * itemH + 4;

    const int vpH = parentWidget()->height();
    int y = viewportPos.y();
    if (y + popupH > vpH && y - popupH - itemH >= 0)
        y = viewportPos.y() - popupH - itemH;

    int x = viewportPos.x();
    if (x + kPopupWidth > parentWidget()->width())
        x = qMax(0, parentWidget()->width() - kPopupWidth);

    move(x, y);
    resize(kPopupWidth, popupH);
    show();
    raise();
}

bool CompletionPopup::isActive() const
{
    return isVisible() && list->count() > 0;
}

void CompletionPopup::navigateDown()
{
    const int next = (list->currentRow() + 1) % list->count();
    list->setCurrentRow(next);
    list->scrollToItem(list->currentItem());
}

void CompletionPopup::navigateUp()
{
    const int prev = (list->currentRow() - 1 + list->count()) % list->count();
    list->setCurrentRow(prev);
    list->scrollToItem(list->currentItem());
}

void CompletionPopup::acceptCurrent()
{
    if (list->currentItem())
        emit accepted(list->currentItem()->text(), wLen);
    hide();
}

void CompletionPopup::dismiss()
{
    hide();
}
