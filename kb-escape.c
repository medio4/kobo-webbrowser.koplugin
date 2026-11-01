/* kb-escape: watchdog that runs next to kobo-browser (started by run.sh), outside
 * the Qt event loop, so it still works when the browser hangs.
 *
 *   kb-escape <browser-pid> <marker-file>
 *
 * - Holding the power button for KB_ESCAPE_HOLD_SEC seconds (default 5) kills the
 *   browser: SIGTERM, then SIGKILL 2 s later if it is still there. A dead process
 *   drops its EVIOCGRAB on the touchscreen, and run.sh returns to KOReader.
 *   The power device is read without grabbing it, so power.cpp still sees every
 *   press (short presses = sleep, unchanged; it ignores releases of long holds).
 * - Optional heartbeat: with KB_HEARTBEAT_SEC=N (> 0) and KB_HEARTBEAT_FILE set, the
 *   browser touches that file every second; once it has appeared, if it stops
 *   changing for N seconds of awake time, the browser is killed the same way.
 *   Off by default (a slow page or the 20 s Wi-Fi-off step can block the UI thread).
 * - Before killing, writes <marker-file> so run.sh knows to do the full refresh the
 *   browser could not do itself.
 * - Exits on its own when the browser process is gone (no orphans).
 *
 * Timing uses CLOCK_MONOTONIC, which stops while the device is suspended, so a
 * night asleep doesn't count as a hold or a stale heartbeat.
 * Power device: KB_POWER_DEV if set, else the first /dev/input/eventN (other than
 * KB_TOUCH_DEV) that can send KEY_POWER, the same search power.cpp does. */
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef KEY_POWER
#define KEY_POWER 116
#endif

static long long nowMs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int envInt(const char *name, int def)
{
    const char *v = getenv(name);
    return (v && *v) ? atoi(v) : def;
}

static void logMsg(const char *msg)
{
    fprintf(stderr, "kb-escape: %s\n", msg);
}

static int hasPowerKey(int fd)
{
    unsigned char keys[KEY_MAX / 8 + 1];
    memset(keys, 0, sizeof keys);
    return ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) >= 0
        && (keys[KEY_POWER / 8] & (1 << (KEY_POWER % 8)));
}

static int powerKeyDown(int fd)
{
    unsigned char keys[KEY_MAX / 8 + 1];
    memset(keys, 0, sizeof keys);
    if (ioctl(fd, EVIOCGKEY(sizeof keys), keys) < 0) return -1;
    return (keys[KEY_POWER / 8] & (1 << (KEY_POWER % 8))) != 0;
}

static int openPowerDevice(void)
{
    const char *env = getenv("KB_POWER_DEV");
    const char *touch = getenv("KB_TOUCH_DEV");
    char path[64];
    int i, fd;
    if (env && *env) {
        if (!strcmp(env, "none")) return -1;
        fd = open(env, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0 && hasPowerKey(fd)) return fd;
        if (fd >= 0) close(fd);
        return -1;
    }
    for (i = 0; i < 10; ++i) {
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        if (touch && !strcmp(path, touch)) continue;
        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        if (hasPowerKey(fd)) {
            char name[64] = "", msg[160];
            ioctl(fd, EVIOCGNAME(sizeof name), name);
            snprintf(msg, sizeof msg, "power button on %s (%s)", path, name);
            logMsg(msg);
            return fd;
        }
        close(fd);
    }
    return -1;
}

static int alive(pid_t pid)
{
    return kill(pid, 0) == 0 || errno == EPERM;
}

static void killBrowser(pid_t pid, const char *marker, const char *why)
{
    int i, fd;
    logMsg(why);
    fd = open(marker, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) {
        if (write(fd, why, strlen(why)) < 0) { /* best effort */ }
        close(fd);
    }
    kill(pid, SIGTERM);
    for (i = 0; i < 20 && alive(pid); ++i) usleep(100000);
    if (alive(pid)) {
        logMsg("still running after SIGTERM, sending SIGKILL");
        kill(pid, SIGKILL);
    }
}

int main(int argc, char **argv)
{
    pid_t pid;
    const char *marker, *hbFile;
    int fd, holdMs, hbMs;
    long long pressedAt = -1, lastAlive, hbSeenAt;
    time_t hbMtime = 0;
    struct stat st;

    if (argc != 3) {
        fprintf(stderr, "usage: %s <browser-pid> <marker-file>\n", argv[0]);
        return 2;
    }
    pid = (pid_t)atoi(argv[1]);
    marker = argv[2];
    if (pid <= 1) return 2;
    holdMs = envInt("KB_ESCAPE_HOLD_SEC", 5) * 1000;
    hbMs = envInt("KB_HEARTBEAT_SEC", 0) * 1000;
    hbFile = getenv("KB_HEARTBEAT_FILE");
    if (!hbFile || !*hbFile) hbMs = 0;

    fd = openPowerDevice();
    if (fd < 0) logMsg("no power button device; hold-to-quit is off");
    if (fd < 0 && hbMs <= 0) {
        /* Nothing to watch. Exit quietly; run.sh doesn't depend on us. */
        return 0;
    }

    lastAlive = hbSeenAt = nowMs();
    for (;;) {
        struct pollfd pfd;
        long long now = nowMs();
        int timeout = 1000, n;

        if (pressedAt >= 0) {
            long long left = pressedAt + holdMs - now;
            if (left < timeout) timeout = left > 0 ? (int)left : 0;
        }
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        n = poll(&pfd, fd >= 0 ? 1 : 0, timeout);
        if (n < 0 && errno != EINTR) { logMsg("poll failed"); return 1; }
        now = nowMs();

        if (n > 0 && (pfd.revents & POLLIN)) {
            struct input_event ev[16];
            ssize_t r;
            while ((r = read(fd, ev, sizeof ev)) > 0) {
                int i;
                for (i = 0; i < (int)(r / sizeof(struct input_event)); ++i) {
                    if (ev[i].type == EV_SYN && ev[i].code == SYN_DROPPED) {
                        /* Lost events: trust the kernel's key state instead. */
                        int down = powerKeyDown(fd);
                        if (down == 0) pressedAt = -1;
                        else if (down == 1 && pressedAt < 0) pressedAt = now;
                    }
                    if (ev[i].type != EV_KEY || ev[i].code != KEY_POWER) continue;
                    if (ev[i].value == 1) pressedAt = now;
                    else if (ev[i].value == 0) pressedAt = -1;
                    /* value 2 = autorepeat: still held */
                }
            }
        } else if (n > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            logMsg("power device went away");
            close(fd);
            fd = -1;
            pressedAt = -1;
            if (hbMs <= 0) return 0;
        }

        if (pressedAt >= 0 && now - pressedAt >= holdMs) {
            /* Double-check: a lost release must not kill the browser. */
            if (powerKeyDown(fd) != 0) {
                killBrowser(pid, marker, "power button held, quitting the browser");
                return 0;
            }
            pressedAt = -1;
        }

        if (now - lastAlive >= 1000 || n == 0) {
            if (!alive(pid)) return 0;
            lastAlive = now;
            if (hbMs > 0 && stat(hbFile, &st) == 0) {
                if (st.st_mtime != hbMtime) {
                    hbMtime = st.st_mtime;
                    hbSeenAt = now;
                } else if (hbMtime && now - hbSeenAt >= hbMs) {
                    killBrowser(pid, marker, "browser heartbeat stale, quitting the browser");
                    return 0;
                }
            }
        }
    }
}
