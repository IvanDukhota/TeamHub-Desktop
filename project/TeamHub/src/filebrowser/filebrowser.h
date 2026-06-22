#ifndef FILEBROWSER_H
#define FILEBROWSER_H
#include <QFileIconProvider>
#include <QFileSystemModel>
#include <QFileSystemWatcher>
#include <QIcon>
#include <QInputDialog>
#include <QLineEdit>
#include <QMap>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTreeView>
#include <QTreeWidget>
#include <QWidget>

class GitStatusDelegate;

class FileBrowser : public QWidget
{
    Q_OBJECT
public:
    explicit FileBrowser(QWidget *parent = nullptr);

    void setRootPath(const QString &path);
    QString rootPath() const;
    QString findFile(const QString &filename);

    void setRemoteFiles(const QStringList &relPaths);
    void revealFile(const QString &absolutePath);
    void refreshGitStatus();

    void clearRemoteMode();

signals:
    void fileDoubleClicked(const QString &filePath);
    void openFolderRequested();
    void cloneRepoRequested();

private slots:
    void onItemDoubleClicked(const QModelIndex &index);
    void onRemoteItemDoubleClicked(QTreeWidgetItem *item, int column);
    void showContextMenu(const QPoint &pos);
    void deleteSelected();
    void copySelected();
    void cutSelected();
    void pasteToSelected();
    void newFile();
    void newFolder();
    void renameSelected();

private:
    void setupFileBrowser();
    void setupFilter();

    QWidget *placeholder;
    QStackedWidget *stack;
    QTreeView *tree;
    QFileSystemModel *model;
    QTreeWidget *remoteTree;
    QLineEdit *searchBox;

    QString clipboardPath;
    bool isCut = false;
    bool projectLoaded = false;
    QIcon pythonIcon;
    QMap<QString, QIcon> extIcons;

    QMap<QString, QChar> gitStatus;
    QProcess *gitStatusProc = nullptr;
    QFileSystemWatcher *gitWatcher = nullptr;
    GitStatusDelegate *gitDelegate = nullptr;
};

#endif // FILEBROWSER_H
