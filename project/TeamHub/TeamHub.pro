QT += widgets core websockets multimedia network sql
CONFIG += c++17

INCLUDEPATH += libs/include

DEFINES += TEAMHUB_STYLES_DIR=\\\"$$PWD/styles/\\\"
DEFINES += TEAMHUB_ICONS_DIR=\\\"$$PWD/icons/\\\"

LIBS += -L$$PWD/libs -lqscintilla2_qt6d -lopus -lgit2

win32 {
    RC_ICONS = icons/th.ico
    LIBS += -ldwmapi

    QMAKE_POST_LINK += $$QMAKE_COPY \
        $$shell_path($$PWD/libs/qscintilla2_qt6d.dll) \
        $$shell_path($$OUT_PWD/) $$escape_expand(\\n\\t)
    QMAKE_POST_LINK += $$QMAKE_COPY \
        $$shell_path($$PWD/libs/libgit2.dll) \
        $$shell_path($$OUT_PWD/) $$escape_expand(\\n\\t)
    QMAKE_POST_LINK += $$QMAKE_COPY \
        $$shell_path($$PWD/.env) \
        $$shell_path($$OUT_PWD/) $$escape_expand(\\n\\t)
    QMAKE_POST_LINK += xcopy /E /I /Y \
        $$shell_path($$PWD/styles) \
        $$shell_path($$OUT_PWD/styles) $$escape_expand(\\n\\t)
    QMAKE_POST_LINK += xcopy /E /I /Y \
        $$shell_path($$PWD/icons) \
        $$shell_path($$OUT_PWD/icons) $$escape_expand(\\n\\t)

    QMAKE_POST_LINK += $$QMAKE_COPY \
        $$shell_path($$[QT_INSTALL_BINS]/Qt6Sql.dll) \
        $$shell_path($$OUT_PWD/) $$escape_expand(\\n\\t)
    QMAKE_POST_LINK += xcopy /E /I /Y \
        $$shell_path($$[QT_INSTALL_PLUGINS]/sqldrivers) \
        $$shell_path($$OUT_PWD/sqldrivers) $$escape_expand(\\n\\t)
}

SOURCES += \
    src/editor/lspclient.cpp \
    src/auth/authmanager.cpp \
    src/auth/authdialog.cpp \
    src/avatar/avatar.cpp \
    src/collab/collabsession.cpp \
    src/collab/sessionreportdialog.cpp \
    src/config/appconfig.cpp \
    src/db/projectdb.cpp \
    src/debug/debugadapter.cpp \
    src/editor/codeeditor.cpp \
    src/editor/textsearch.cpp \
    src/filebrowser/filebrowser.cpp \
    src/git/gitmanager.cpp \
    src/git/gitpanel.cpp \
    src/main.cpp \
    src/mainwindow.cpp \
    src/rga/rgamanager.cpp \
    src/settings/settingsmanager.cpp \
    src/settings/settingsdialog.cpp \
    src/rga/rgaseq.cpp \
    src/team/teammanager.cpp \
    src/team/teamspanel.cpp \
    src/terminal/terminal.cpp \
    src/voicechat/voicechat.cpp

HEADERS += \
    src/editor/lspclient.h \
    src/auth/authmanager.h \
    src/auth/authdialog.h \
    src/avatar/avatar.h \
    src/collab/collabsession.h \
    src/collab/sessionreport.h \
    src/collab/sessionreportdialog.h \
    src/config/appconfig.h \
    src/db/projectdb.h \
    src/debug/debugadapter.h \
    src/editor/codeeditor.h \
    src/editor/textsearch.h \
    src/filebrowser/filebrowser.h \
    src/git/gitmanager.h \
    src/git/gitpanel.h \
    src/mainwindow.h \
    src/rga/rganode.h \
    src/rga/rgamanager.h \
    src/settings/settingsmanager.h \
    src/settings/settingsdialog.h \
    src/rga/rgaseq.h \
    src/team/teammanager.h \
    src/team/teamspanel.h \
    src/terminal/terminal.h \
    src/voicechat/voicechat.h

qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target