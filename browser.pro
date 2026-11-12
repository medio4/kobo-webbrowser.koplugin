# Kobo browser built against the Qt 5.2.1 + QtWebKit shipped in Kobo firmware (Nickel).
TEMPLATE = app
TARGET = kobo-browser
QT += core gui widgets network gui-private svg
CONFIG += release
CONFIG -= debug
# QtWebKit 5.2.1 public headers (from the qtwebkit source tarball, see ../downloads/qtwebkit-headers)
INCLUDEPATH += $$PWD/../downloads/qtwebkit-headers/qtwebkit-opensource-src-5.2.1/include
# Link against the firmware's own WebKit libraries (symlinked in ./lib with their Qt5 names)
LIBS += -L$$PWD/lib -lQt5WebKitWidgets -lQt5WebKit -ldl
QMAKE_RPATHDIR += /usr/local/Kobo /usr/local/Qt-5.2.1-arm/lib
SOURCES += src/main.cpp src/browser.cpp src/touchwebview.cpp src/flatbutton.cpp src/keyboard.cpp src/touchinput.cpp src/eink.cpp src/fbwriter.cpp src/webpage.cpp src/pagescroll.cpp src/hwtcon.cpp src/epub.cpp src/toc.cpp src/navigator.cpp src/memtune.cpp src/power.cpp src/textentry.cpp src/phoneshare.cpp src/menupanel.cpp src/bookmarks.cpp src/scrollmark.cpp src/icons.cpp src/adblock.cpp src/csscompat.cpp src/rotation.cpp src/third_party/qrcodegen.c
HEADERS += src/browser.h src/touchwebview.h src/flatbutton.h src/keyboard.h src/crashhandler.h src/touchinput.h src/eink.h src/fbwriter.h src/webpage.h src/pagescroll.h src/hwtcon.h src/epub.h src/toc.h src/navigator.h src/memtune.h src/power.h src/textentry.h src/phoneshare.h src/menupanel.h src/bookmarks.h src/scrollmark.h src/icons.h src/adblock.h src/csscompat.h src/rotation.h
INCLUDEPATH += $$PWD/src
RESOURCES += src/epub.qrc
QMAKE_CFLAGS += -std=gnu99
