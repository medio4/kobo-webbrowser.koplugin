// Print a backtrace to stderr on SIGSEGV/SIGABRT/SIGBUS, then re-raise. On ARM the
// glibc backtrace() stops at the faulting frame (no unwind tables in Kobo's Qt), so we
// also print the faulting PC, the caller (LR) and the fault address from the signal context.
#ifndef CRASHHANDLER_H
#define CRASHHANDLER_H
#include <execinfo.h>
#include <dlfcn.h>
#include <ucontext.h>
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
static void crashWhere(const char *what, unsigned long a)
{
    char msg[256];
    Dl_info info;
    int len;
    if (dladdr((void *)a, &info) && info.dli_fname)
        len = snprintf(msg, sizeof msg, "%s 0x%lx = %s+0x%lx (%s)\n", what, a, info.dli_fname,
                       a - (unsigned long)info.dli_fbase, info.dli_sname ? info.dli_sname : "?");
    else
        len = snprintf(msg, sizeof msg, "%s 0x%lx\n", what, a);
    if (write(2, msg, len) < 0) {}
}
static void crashHandler(int sig, siginfo_t *si, void *ctx)
{
    void *frames[64];
    int n = backtrace(frames, 64);
    char msg[64];
    int len = snprintf(msg, sizeof msg, "\n*** crash: signal %d, %d frames ***\n", sig, n);
    if (write(2, msg, len) < 0) {}
    backtrace_symbols_fd(frames, n, 2);
#ifdef __arm__
    const mcontext_t &mc = static_cast<ucontext_t *>(ctx)->uc_mcontext;
    crashWhere("pc", mc.arm_pc);
    crashWhere("lr", mc.arm_lr);
    crashWhere("fault addr", (unsigned long)si->si_addr);
    char regs[160];
    int rl = snprintf(regs, sizeof regs, "r0 %lx r1 %lx r2 %lx r3 %lx sp %lx\n", mc.arm_r0, mc.arm_r1,
                      mc.arm_r2, mc.arm_r3, mc.arm_sp);
    if (write(2, regs, rl) < 0) {}
    // Return addresses on the stack: lets us guess deeper callers without unwind tables.
    unsigned long *sp = (unsigned long *)mc.arm_sp;
    for (int i = 0; i < 96; i++) {
        Dl_info di;
        unsigned long v = sp[i];
        if (dladdr((void *)v, &di) && di.dli_fname && v > (unsigned long)di.dli_fbase + 0x1000) crashWhere("stack", v);
    }
#else
    (void)si; (void)ctx;
#endif
    signal(sig, SIG_DFL);
    raise(sig);
}
static void installCrashHandler()
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crashHandler;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    int sigs[] = { SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE };
    for (unsigned i = 0; i < sizeof sigs / sizeof sigs[0]; i++) sigaction(sigs[i], &sa, 0);
}
#endif
