// Print a backtrace to stderr on SIGSEGV/SIGABRT/SIGBUS, then re-raise.
#ifndef CRASHHANDLER_H
#define CRASHHANDLER_H
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
static void crashHandler(int sig)
{
    void *frames[64];
    int n = backtrace(frames, 64);
    char msg[64];
    int len = snprintf(msg, sizeof msg, "\n*** crash: signal %d, %d frames ***\n", sig, n);
    if (write(2, msg, len) < 0) {}
    backtrace_symbols_fd(frames, n, 2);
    signal(sig, SIG_DFL);
    raise(sig);
}
static void installCrashHandler()
{
    signal(SIGSEGV, crashHandler);
    signal(SIGBUS, crashHandler);
    signal(SIGABRT, crashHandler);
    signal(SIGILL, crashHandler);
    signal(SIGFPE, crashHandler);
}
#endif
