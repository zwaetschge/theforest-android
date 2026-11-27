/*
 * main.c - Linux debug host: maps TheForest.exe and its lifted DLLs from the game directory and
 * runs the recompiled code.
 *
 * usage: tfhost <game dir with TheForest.exe> [user dir]
 * env:   W64_TRACE=1|2|3, W64_LOGFILE, W64_ARGS, W64_ENV, W64_TIMEOUT (s), W64_HARD_IMPORTS=1
 */
#define _GNU_SOURCE
#include "w64.h"
#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <ucontext.h>
#include <sys/stat.h>
#include <unistd.h>

extern __thread uint8_t *w64_teb;
CPU *w64_cpu(void);

void w64_shutdown(int code)
{
    w64_log("[host] exit %d", code);
    fflush(stderr);
    _exit(code);
}

static void on_fault(int sig, siginfo_t *si, void *uc)
{
    void *pc = NULL;
#if defined(__x86_64__)
    if (uc) pc = (void *)((ucontext_t *)uc)->uc_mcontext.gregs[REG_RIP];
#endif
    { const char *w64_current_hostfn(void); w64_log("[host] signal %d at address %p pc=%p (last host function: %s) teb=%p", sig, si->si_addr, pc, w64_current_hostfn(), (void *)w64_teb); }
    void *bt[32];
    int n = backtrace(bt, 32);
    backtrace_symbols_fd(bt, n, 2);
    if (w64_teb) w64_backtrace(w64_cpu());
    fflush(stderr);
    _exit(128 + sig);
}

static void *timeout_thread(void *arg)
{
    int secs = (int)(intptr_t)arg;
    sleep((unsigned)secs);
    w64_log("[host] timeout after %d s", secs);
    w64_shutdown(0);
    return NULL;
}

static void *game_thread(void *arg)
{
    (void)arg;
    if (w64_load(w64_gamedir)) w64_shutdown(2);
    int r = w64_run_entry();
    w64_shutdown(r);
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <game dir> [user dir]\n", argv[0]);
        return 2;
    }
    realpath(argv[1], w64_gamedir);
    if (argc > 2) { mkdir(argv[2], 0777); realpath(argv[2], w64_userdir); }
    else { snprintf(w64_userdir, sizeof w64_userdir, "%s/.hpuser", getenv("HOME") ? getenv("HOME") : "/tmp"); mkdir(w64_userdir, 0777); }
    if (getenv("W64_TRACE")) w64_trace = atoi(getenv("W64_TRACE"));
    { extern int g_eh_trace; if (getenv("W64_EH_TRACE")) g_eh_trace = 1; }
    if (chdir(w64_gamedir)) {}
    w64_log("[host] game %s, user %s", w64_gamedir, w64_userdir);

    static uint8_t altstack[1 << 16];
    stack_t ss = { .ss_sp = altstack, .ss_size = sizeof altstack };
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);

    pthread_t th;
    if (getenv("W64_TIMEOUT")) pthread_create(&th, NULL, timeout_thread, (void *)(intptr_t)atoi(getenv("W64_TIMEOUT")));
    /* The lifted code keeps every guest call as a host C frame: run it on a large stack. */
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 512u << 20);
    pthread_create(&th, &at, game_thread, NULL);
    pthread_join(th, NULL);
    return 0;
}
