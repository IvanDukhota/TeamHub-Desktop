#include "filebrowser.h"
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QHeaderView>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QStyle>
#include <QVBoxLayout>
#include <functional>

class GitStatusDelegate : public QStyledItemDelegate {
public:
    explicit GitStatusDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}

    void updateStatus(const QMap<QString, QChar> &status, const QString &root) {
        statusMap = status;
        rootPath = root;
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        QStyledItemDelegate::paint(painter, option, index);
        if (rootPath.isEmpty()) return;
        const auto *fsModel = qobject_cast<const QFileSystemModel *>(index.model());
        if (!fsModel) return;
        const QString rel = QDir(rootPath).relativeFilePath(fsModel->filePath(index));
        const QChar sc = statusMap.value(rel, '\0');
        if (sc == '\0') return;
        QColor col;
        switch (sc.toLatin1()) {
        case 'M': col = QColor("#e2c08d"); break;
        case 'A': col = QColor("#4ec9b0"); break;
        case 'D': col = QColor("#f14c4c"); break;
        case 'U': col = QColor("#7ab8f5"); break;
        case 'R': col = QColor("#b5cea8"); break;
        default: return;
        }
        painter->save();
        painter->setPen(col);
        QFont f = painter->font();
        f.setPixelSize(10);
        f.setBold(true);
        painter->setFont(f);
        painter->drawText(option.rect.adjusted(0, 0, -4, 0),
                          Qt::AlignRight | Qt::AlignVCenter, QString(sc));
        painter->restore();
    }

private:
    QMap<QString, QChar> statusMap;
    QString rootPath;
};

static QIcon loadIconTransparent(const QString &path)
{
    QImage img(path);
    if (img.isNull())
        return QIcon();
    img = img.convertToFormat(QImage::Format_ARGB32);

    const int w = img.width(), h = img.height();
    auto isWhitish = [&](int x, int y) {
        const QColor c(img.pixel(x, y));
        return c.alpha() > 0 && c.red() > 230 && c.green() > 230 && c.blue() > 230;
    };

    QVector<bool> visited(w * h, false);
    QList<QPoint> queue;

    auto enqueue = [&](int x, int y) {
        if (x >= 0 && x < w && y >= 0 && y < h && !visited[y * w + x] && isWhitish(x, y)) {
            visited[y * w + x] = true;
            queue.append({x, y});
        }
    };

    for (int x = 0; x < w; ++x) { enqueue(x, 0); enqueue(x, h - 1); }
    for (int y = 1; y < h - 1; ++y) { enqueue(0, y); enqueue(w - 1, y); }

    while (!queue.isEmpty()) {
        const QPoint p = queue.takeFirst();
        img.setPixel(p.x(), p.y(), qRgba(0, 0, 0, 0));
        enqueue(p.x() + 1, p.y()); enqueue(p.x() - 1, p.y());
        enqueue(p.x(), p.y() + 1); enqueue(p.x(), p.y() - 1);
    }

    return QIcon(QPixmap::fromImage(img));
}

class TeamHubIconProvider : public QFileIconProvider
{
    QMap<QString, QIcon> icons;

public:
    void addIcon(const QStringList &exts, const QIcon &icon) {
        for (const QString &ext : exts)
            icons[ext] = icon;
    }
    QIcon icon(const QFileInfo &info) const override
    {
        const QString ext = info.suffix().toLower();
        if (icons.contains(ext))
            return icons[ext];
        return QFileIconProvider::icon(info);
    }
};

FileBrowser::FileBrowser(QWidget *parent)
    : QWidget(parent)
{
    setupFileBrowser();
    setupFilter();
}

void FileBrowser::setupFileBrowser()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    searchBox = new QLineEdit(this);
    searchBox->setPlaceholderText("Search files...");
    searchBox->setObjectName("fileSearch");
    layout->addWidget(searchBox);

    placeholder = new QWidget(this);
    auto *phLayout = new QVBoxLayout(placeholder);
    phLayout->setAlignment(Qt::AlignCenter);
    phLayout->setSpacing(10);

    const QString btnStyle = "QPushButton {"
                             "  background: #0e639c;"
                             "  color: #ffffff;"
                             "  border: none;"
                             "  border-radius: 6px;"
                             "  padding: 8px 18px;"
                             "  font-size: 12px;"
                             "}"
                             "QPushButton:hover { background: #1177bb; }"
                             "QPushButton:pressed { background: #0a4f7e; }";

    auto *btnOpen = new QPushButton("Open Folder", placeholder);
    btnOpen->setFixedWidth(160);
    btnOpen->setCursor(Qt::PointingHandCursor);
    btnOpen->setStyleSheet(btnStyle);
    connect(btnOpen, &QPushButton::clicked, this, &FileBrowser::openFolderRequested);

    auto *btnClone = new QPushButton("Clone Repository", placeholder);
    btnClone->setFixedWidth(160);
    btnClone->setCursor(Qt::PointingHandCursor);
    btnClone->setStyleSheet(btnStyle);
    connect(btnClone, &QPushButton::clicked, this, &FileBrowser::cloneRepoRequested);

    phLayout->addWidget(btnOpen);
    phLayout->addWidget(btnClone);
    layout->addWidget(placeholder);

    stack = new QStackedWidget(this);
    stack->setVisible(false);
    layout->addWidget(stack);

    auto loadIcon = [](const QString &name) -> QIcon {
        QString path = QCoreApplication::applicationDirPath() + "/icons/" + name;
        QIcon ic = loadIconTransparent(path);
        if (ic.isNull())
            ic = loadIconTransparent(QString(TEAMHUB_ICONS_DIR) + name);
        return ic;
    };

    struct IconDef { QStringList exts; QString file; };
    const QList<IconDef> defs = {
        {{"py"},              "python.png"},
        {{"cpp","cxx","cc","c"}, "cpp.png"},
        {{"h","hpp"},        "h.png"},
        {{"json"},           "json.png"},
        {{"md"},             "md.png"},
        {{"pro"},            "pro.png"},
    };

    auto *iconProvider = new TeamHubIconProvider();
    for (const auto &def : defs) {
        QIcon ic = loadIcon(def.file);
        if (!ic.isNull()) {
            iconProvider->addIcon(def.exts, ic);
            for (const QString &ext : def.exts)
                extIcons[ext] = ic;
        }
    }
    pythonIcon = extIcons.value("py");

    model = new QFileSystemModel(this);
    model->setRootPath(QDir::homePath());
    model->setIconProvider(iconProvider);

    tree = new QTreeView(this);
    tree->setModel(model);
    tree->setRootIndex(QModelIndex());
    tree->setHeaderHidden(true);
    tree->hideColumn(1);
    tree->hideColumn(2);
    tree->hideColumn(3);
    tree->setIndentation(16);
    tree->setAnimated(true);
    tree->setUniformRowHeights(true);
    tree->setObjectName("fileBrowserTree");
    gitDelegate = new GitStatusDelegate(this);
    tree->setItemDelegate(gitDelegate);

    connect(tree, &QTreeView::doubleClicked, this, &FileBrowser::onItemDoubleClicked);
    connect(searchBox, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (text.isEmpty())
            model->setNameFilters({"*.py", "*.cpp", "*.cxx", "*.cc", "*.c", "*.h", "*.hpp",
                                   "*.json", "*.pro", "*.txt", "*.md", "*.qml", "*.xml",
                                   "*.js", "*.ts", "*.html", "*.css"});
        else
            model->setNameFilters({"*" + text + "*"});
    });
    tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree, &QTreeView::customContextMenuRequested, this, &FileBrowser::showContextMenu);

    stack->addWidget(tree);

    remoteTree = new QTreeWidget(this);
    remoteTree->setHeaderHidden(true);
    remoteTree->setIndentation(16);
    remoteTree->setObjectName("fileBrowserTree");
    remoteTree->setContextMenuPolicy(Qt::NoContextMenu);
    remoteTree->setAnimated(true);
    remoteTree->setUniformRowHeights(true);

    connect(remoteTree, &QTreeWidget::itemClicked, this, &FileBrowser::onRemoteItemDoubleClicked);

    stack->addWidget(remoteTree);
    stack->setCurrentIndex(0);
}

void FileBrowser::setupFilter()
{
    model->setNameFilters({"*.py", "*.cpp", "*.cxx", "*.cc", "*.c", "*.h", "*.hpp",
                           "*.json", "*.pro", "*.txt", "*.md", "*.qml", "*.xml",
                           "*.js", "*.ts", "*.html", "*.css"});
    model->setNameFilterDisables(false);
}

void FileBrowser::setRootPath(const QString &path)
{
    projectLoaded = true;
    model->setRootPath(path);
    tree->setRootIndex(model->index(path));
    placeholder->setVisible(false);
    stack->setVisible(true);
    stack->setCurrentIndex(0);

    if (!gitWatcher) {
        gitWatcher = new QFileSystemWatcher(this);
        connect(gitWatcher, &QFileSystemWatcher::fileChanged,
                this, &FileBrowser::refreshGitStatus);
    } else if (!gitWatcher->files().isEmpty()) {
        gitWatcher->removePaths(gitWatcher->files());
    }
    const QString gitIndex = path + "/.git/index";
    if (QFileInfo::exists(gitIndex))
        gitWatcher->addPath(gitIndex);

    refreshGitStatus();
}

void FileBrowser::refreshGitStatus()
{
    const QString root = rootPath();
    if (root.isEmpty()) return;
    if (gitStatusProc && gitStatusProc->state() != QProcess::NotRunning)
        return;

    gitStatusProc = new QProcess(this);
    gitStatusProc->setWorkingDirectory(root);

    connect(gitStatusProc,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, root](int exitCode) {
        QMap<QString, QChar> status;
        if (exitCode == 0) {
            auto priority = [](QChar c) -> int {
                switch (c.toLatin1()) {
                case 'D': return 5; case 'M': return 4; case 'A': return 3;
                case 'R': return 2; case 'U': return 1; default: return 0;
                }
            };
            const QString out = QString::fromUtf8(gitStatusProc->readAllStandardOutput());
            for (const QString &line : out.split('\n', Qt::SkipEmptyParts)) {
                if (line.size() < 4) continue;
                const QChar X = line[0], Y = line[1];
                QChar sc;
                if (X == '?' && Y == '?') sc = 'U';
                else if (Y != ' ') sc = Y;
                else sc = X;
                if (sc == ' ') continue;

                QString path = line.mid(3).trimmed();
                if (path.contains(" -> "))
                    path = path.section(" -> ", -1);
                status[path] = sc;

                QString parent = QFileInfo(path).dir().path();
                while (parent != "." && !parent.isEmpty()) {
                    const QChar ex = status.value(parent, '\0');
                    if (ex == '\0' || priority(sc) > priority(ex))
                        status[parent] = sc;
                    parent = QFileInfo(parent).dir().path();
                }
            }
        }
        gitStatus = status;
        if (gitDelegate)
            gitDelegate->updateStatus(gitStatus, root);
        tree->viewport()->update();
        gitStatusProc->deleteLater();
        gitStatusProc = nullptr;
    });
    gitStatusProc->start("git", {"status", "--porcelain"});
}

QString FileBrowser::rootPath() const
{
    return projectLoaded ? model->rootPath() : QString();
}

QString FileBrowser::findFile(const QString &filename)
{
    QDirIterator it(rootPath(),
                    QStringList() << filename,
                    QDir::Files,
                    QDirIterator::Subdirectories);
    return it.hasNext() ? it.next() : QString();
}

void FileBrowser::setRemoteFiles(const QStringList &relPaths)
{
    remoteTree->clear();
    placeholder->setVisible(false);
    stack->setVisible(true);

    const QIcon folderIcon = QApplication::style()->standardIcon(QStyle::SP_DirIcon);
    const QIcon defaultFileIcon = QApplication::style()->standardIcon(QStyle::SP_FileIcon);

    QMap<QString, QTreeWidgetItem *> dirItems;

    std::function<QTreeWidgetItem *(const QString &)> getOrCreateDir =
        [&](const QString &dirPath) -> QTreeWidgetItem * {
        if (dirPath.isEmpty())
            return nullptr;
        if (dirItems.contains(dirPath))
            return dirItems[dirPath];

        const int sep = dirPath.lastIndexOf('/');
        QString parentPath = sep >= 0 ? dirPath.left(sep) : QString();
        QString name = sep >= 0 ? dirPath.mid(sep + 1) : dirPath;

        QTreeWidgetItem *parentItem = getOrCreateDir(parentPath);
        QTreeWidgetItem *item = parentItem ? new QTreeWidgetItem(parentItem, QStringList(name))
                                           : new QTreeWidgetItem(remoteTree, QStringList(name));
        item->setIcon(0, folderIcon);
        item->setData(0, Qt::UserRole, QString());
        dirItems[dirPath] = item;
        return item;
    };

    for (const QString &relPath : relPaths) {
        const int sep = relPath.lastIndexOf('/');
        QString dirPart = sep >= 0 ? relPath.left(sep) : QString();
        QString fileName = sep >= 0 ? relPath.mid(sep + 1) : relPath;

        QTreeWidgetItem *parent = getOrCreateDir(dirPart);
        QTreeWidgetItem *fileItem = parent ? new QTreeWidgetItem(parent, QStringList(fileName))
                                           : new QTreeWidgetItem(remoteTree, QStringList(fileName));
        const QString ext = QFileInfo(fileName).suffix().toLower();
        fileItem->setIcon(0, extIcons.value(ext, defaultFileIcon));
        fileItem->setData(0, Qt::UserRole, relPath);
    }

    remoteTree->expandAll();
    stack->setCurrentIndex(1);
    searchBox->setEnabled(false);
}

void FileBrowser::revealFile(const QString &absolutePath)
{
    QModelIndex index = model->index(absolutePath);
    if (index.isValid()) {
        tree->setCurrentIndex(index);
        tree->scrollTo(index, QAbstractItemView::PositionAtCenter);
    }
}

void FileBrowser::clearRemoteMode()
{
    remoteTree->clear();
    stack->setCurrentIndex(0);
    searchBox->setEnabled(true);
    if (!projectLoaded) {
        stack->setVisible(false);
        placeholder->setVisible(true);
    }
}

void FileBrowser::onItemDoubleClicked(const QModelIndex &index)
{
    if (model->isDir(index))
        return;
    emit fileDoubleClicked(model->filePath(index));
}

void FileBrowser::onRemoteItemDoubleClicked(QTreeWidgetItem *item, int /*column*/)
{
    const QString relPath = item->data(0, Qt::UserRole).toString();
    if (!relPath.isEmpty())
        emit fileDoubleClicked(relPath);
}

void FileBrowser::showContextMenu(const QPoint &pos)
{
    QModelIndex index = tree->indexAt(pos);
    QMenu menu(this);

    if (index.isValid()) {
        menu.addAction("New File", this, &FileBrowser::newFile);
        menu.addAction("New Folder", this, &FileBrowser::newFolder);
        menu.addSeparator();
        menu.addAction("Cut", this, &FileBrowser::cutSelected);
        menu.addAction("Copy", this, &FileBrowser::copySelected);
        menu.addAction("Paste", this, &FileBrowser::pasteToSelected);
        menu.addSeparator();
        menu.addAction("Rename", this, &FileBrowser::renameSelected);
        menu.addAction("Delete", this, &FileBrowser::deleteSelected);
    } else {
        menu.addAction("New File", this, &FileBrowser::newFile);
        menu.addAction("New Folder", this, &FileBrowser::newFolder);
        menu.addAction("Paste", this, &FileBrowser::pasteToSelected);
    }

    menu.exec(tree->viewport()->mapToGlobal(pos));
}

void FileBrowser::deleteSelected()
{
    QModelIndex index = tree->currentIndex();
    if (!index.isValid())
        return;
    QString path = model->filePath(index);
    QFileInfo info(path);
    auto btn = QMessageBox::question(this,
                                     "Delete",
                                     "Delete " + info.fileName() + "?",
                                     QMessageBox::Yes | QMessageBox::No);
    if (btn != QMessageBox::Yes)
        return;
    if (info.isDir())
        QDir(path).removeRecursively();
    else
        QFile::remove(path);
}

void FileBrowser::copySelected()
{
    QModelIndex index = tree->currentIndex();
    if (!index.isValid())
        return;
    clipboardPath = model->filePath(index);
    isCut = false;
}

void FileBrowser::cutSelected()
{
    QModelIndex index = tree->currentIndex();
    if (!index.isValid())
        return;
    clipboardPath = model->filePath(index);
    isCut = true;
}

void FileBrowser::pasteToSelected()
{
    if (clipboardPath.isEmpty())
        return;
    QModelIndex index = tree->currentIndex();
    QString destDir;
    if (index.isValid()) {
        QString selectedPath = model->filePath(index);
        destDir = model->isDir(index) ? selectedPath : QFileInfo(selectedPath).absolutePath();
    } else {
        destDir = model->rootPath();
    }
    QString fileName = QFileInfo(clipboardPath).fileName();
    QString destPath = destDir + "/" + fileName;
    if (isCut) {
        QFile::rename(clipboardPath, destPath);
        clipboardPath.clear();
        isCut = false;
    } else
        QFile::copy(clipboardPath, destPath);
}

void FileBrowser::newFile()
{
    QModelIndex index = tree->currentIndex();
    QString dir = index.isValid() && model->isDir(index)
                      ? model->filePath(index)
                      : (index.isValid() ? QFileInfo(model->filePath(index)).absolutePath()
                                         : model->rootPath());
    bool ok;
    QString name = QInputDialog::getText(this,
                                         "New File",
                                         "File name:",
                                         QLineEdit::Normal,
                                         "new_file.py",
                                         &ok);
    if (!ok || name.isEmpty())
        return;
    QFile file(dir + "/" + name);
    file.open(QIODevice::WriteOnly);
    file.close();
}

void FileBrowser::newFolder()
{
    QModelIndex index = tree->currentIndex();
    QString dir = index.isValid() && model->isDir(index)
                      ? model->filePath(index)
                      : (index.isValid() ? QFileInfo(model->filePath(index)).absolutePath()
                                         : model->rootPath());
    bool ok;
    QString name = QInputDialog::getText(this,
                                         "New Folder",
                                         "Folder name:",
                                         QLineEdit::Normal,
                                         "new_folder",
                                         &ok);
    if (!ok || name.isEmpty())
        return;
    QDir(dir).mkdir(name);
}

void FileBrowser::renameSelected()
{
    QModelIndex index = tree->currentIndex();
    if (!index.isValid())
        return;
    QString oldPath = model->filePath(index);
    QString oldName = QFileInfo(oldPath).fileName();
    QString dir = QFileInfo(oldPath).absolutePath();
    bool ok;
    QString newName
        = QInputDialog::getText(this, "Rename", "New name:", QLineEdit::Normal, oldName, &ok);
    if (!ok || newName.isEmpty() || newName == oldName)
        return;
    QFile::rename(oldPath, dir + "/" + newName);
}