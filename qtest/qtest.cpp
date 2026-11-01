// Minimal Qt widgets test for the Kobo QPA: shows a label, quits after N seconds.
#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>
#include <QtGui/QGenericPluginFactory>
#include <QtCore/QTimer>
#include <QtCore/QDebug>
#include <cstdlib>
#include "../crashhandler.h"
int main(int argc, char **argv) {
    installCrashHandler();
    QApplication app(argc, argv);
    const char *touchDev = getenv("KB_TOUCH_DEV");
    if (touchDev && *touchDev) {
        QObject *t = QGenericPluginFactory::create("touch_device", QString::fromLatin1(touchDev));
        qDebug() << "touch:" << (t ? "ok" : "FAILED");
    }
    QWidget w;
    QVBoxLayout *l = new QVBoxLayout(&w);
    QLabel *lab = new QLabel("Hello from Qt 5.2.1 on Kobo\n(qtest)");
    QFont f = lab->font(); f.setPointSize(24); lab->setFont(f);
    l->addWidget(lab);
    QPushButton *b = new QPushButton("Quit");
    b->setMinimumHeight(100);
    QObject::connect(b, SIGNAL(clicked()), &app, SLOT(quit()));
    l->addWidget(b);
    int secs = getenv("KB_SECS") ? atoi(getenv("KB_SECS")) : 20;
    QTimer::singleShot(secs * 1000, &app, SLOT(quit()));
    w.showFullScreen();
    qDebug() << "shown";
    int rc = app.exec();
    qDebug() << "exec returned" << rc;
    return rc;
}
