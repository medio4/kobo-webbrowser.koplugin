# Kobo browser built against the Qt 5.2.1 + QtWebKit shipped in Kobo firmware (Nickel).
TEMPLATE = app
TARGET = kobo-browser
QT += core gui widgets network gui-private
CONFIG += release
CONFIG -= debug
# QtWebKit 5.2.1 public headers (from the qtwebkit source tarball, see ../downloads/qtwebkit-headers)
INCLUDEPATH += $$PWD/../downloads/qtwebkit-headers/qtwebkit-opensource-src-5.2.1/include
# Link against the firmware's own WebKit libraries (symlinked in ./lib with their Qt5 names)
LIBS += -L$$PWD/lib -lQt5WebKitWidgets -lQt5WebKit -ldl
QMAKE_RPATHDIR += /usr/local/Kobo /usr/local/Qt-5.2.1-arm/lib
SOURCES += main.cpp touchinput.cpp eink.cpp fbwriter.cpp webpage.cpp pagescroll.cpp hwtcon.cpp epub.cpp memtune.cpp
HEADERS += browser.h crashhandler.h touchinput.h eink.h fbwriter.h webpage.h pagescroll.h hwtcon.h epub.h memtune.h
RESOURCES += epub.qrc
