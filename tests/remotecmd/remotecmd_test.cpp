// Desktop test for src/remotecmd.cpp: words written to the FIFO arrive as commands, whole
// lines only, and the FIFO is removed again. Run with tests/remotecmd/run.sh.
#include "../../src/remotecmd.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QStringList>
#include <QtCore/QFile>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

struct Sink : QObject {
    Q_OBJECT
public:
    QStringList got;
public slots:
    void take(const QString &w) { got << w; }
};

static void put(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_NONBLOCK);
    if (fd < 0) { ++failures; printf("FAIL cannot open the FIFO for writing\n"); return; }
    if (write(fd, text, strlen(text)) < 0) ++failures;
    close(fd);
    QCoreApplication::processEvents(); QCoreApplication::processEvents();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const char *path = "/tmp/kb-remotecmd-test.fifo";
    {
        RemoteCommands off;                 // no KB_CMD_FIFO: nothing
        CHECK(!off.isOpen());
    }
    setenv("KB_CMD_FIFO", path, 1);
    {
        RemoteCommands rc;
        Sink sink;
        QObject::connect(&rc, SIGNAL(command(QString)), &sink, SLOT(take(QString)));
        CHECK(rc.isOpen() && QFile::exists(path));
        put(path, "next\n");
        CHECK(sink.got.join(",") == "next");
        put(path, "prev\nnext\n");          // two at once
        CHECK(sink.got.join(",") == "next,prev,next");
        put(path, "pr");                    // half a word waits for its end
        CHECK(sink.got.size() == 3);
        put(path, "ev\n\n  next  \n");
        CHECK(sink.got.join(",") == "next,prev,next,prev,next");
    }
    CHECK(!QFile::exists(path));            // removed with the object
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
#include "remotecmd_test.moc"
