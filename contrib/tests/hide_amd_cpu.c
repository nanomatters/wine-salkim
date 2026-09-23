/* Regression probe for WINE_HIDE_AMD_CPU. Build as a PE executable or link
 * the native Linux version with ntdll/unix/system.o and --gc-sections. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <asm/prctl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

extern void emulate_cpuid(void);
extern int handle_cpuid_fault(uintptr_t *, uintptr_t *, uintptr_t *, uintptr_t *, uintptr_t *);

/* Keep the probe independent of Wine's logging and thread initialization. */
unsigned char __wine_dbg_get_channel_flags(void *channel) { return 0; }
int __wine_dbg_header(int cls, void *channel, const char *function) { return 0; }
int __wine_dbg_output(const char *message) { return 0; }

static volatile sig_atomic_t cpuid_faults;
#endif

static volatile int other_faults;
static const char *expected_vendor;

#ifndef _WIN32
static void cpuid(unsigned int leaf, unsigned int subleaf, unsigned int regs[4])
{
    __asm__ volatile("cpuid" : "=a"(regs[0]), "=b"(regs[1]), "=c"(regs[2]), "=d"(regs[3])
                     : "a"(leaf), "c"(subleaf));
}
#endif

static int check_vendor(void)
{
    unsigned int regs[4];
    unsigned char carry;
    char vendor[13];
    unsigned int i;

    for (i = 0; i < 20; ++i)
    {
        __asm__ volatile("stc\n\tcpuid\n\tsetc %4"
                         : "=a"(regs[0]), "=b"(regs[1]), "=c"(regs[2]), "=d"(regs[3]), "=m"(carry)
                         : "a"(0), "c"(0) : "cc");
        if (!carry) return 1;
        memcpy(vendor, regs + 1, 4);
        memcpy(vendor + 4, regs + 3, 4);
        memcpy(vendor + 8, regs + 2, 4);
        vendor[12] = 0;
        if (strcmp(vendor, expected_vendor))
        {
            fprintf(stderr, "Expected %s, received %s\n", expected_vendor, vendor);
            return 1;
        }
    }
    return 0;
}

#ifdef _WIN32
static LONG CALLBACK exception_handler(EXCEPTION_POINTERS *exception)
{
    if (exception->ExceptionRecord->ExceptionCode != EXCEPTION_PRIV_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    if (*(unsigned char *)exception->ExceptionRecord->ExceptionAddress != 0xf4)
        return EXCEPTION_CONTINUE_SEARCH;
    ++other_faults;
#ifdef _WIN64
    ++exception->ContextRecord->Rip;
#else
    ++exception->ContextRecord->Eip;
#endif
    return EXCEPTION_CONTINUE_EXECUTION;
}

static DWORD WINAPI thread_probe(void *arg)
{
    return check_vendor();
}
#else
static void exception_handler(int sig, siginfo_t *info, void *context)
{
    greg_t *regs = ((ucontext_t *)context)->uc_mcontext.gregs;
#ifdef __x86_64__
    uintptr_t *pc = (uintptr_t *)&regs[REG_RIP];
    uintptr_t *ax = (uintptr_t *)&regs[REG_RAX];
    uintptr_t *bx = (uintptr_t *)&regs[REG_RBX];
    uintptr_t *cx = (uintptr_t *)&regs[REG_RCX];
    uintptr_t *dx = (uintptr_t *)&regs[REG_RDX];
#else
    uintptr_t *pc = (uintptr_t *)&regs[REG_EIP];
    uintptr_t *ax = (uintptr_t *)&regs[REG_EAX];
    uintptr_t *bx = (uintptr_t *)&regs[REG_EBX];
    uintptr_t *cx = (uintptr_t *)&regs[REG_ECX];
    uintptr_t *dx = (uintptr_t *)&regs[REG_EDX];
#endif

    if (sig == SIGSEGV && regs[REG_TRAPNO] == 13 && info->si_code == SI_KERNEL && !regs[REG_ERR]
        && handle_cpuid_fault(pc, ax, bx, cx, dx))
    {
        ++cpuid_faults;
        return;
    }
    if (*(unsigned char *)*pc != 0xf4) _exit(2);
    ++other_faults;
    ++*pc;
}

static void *thread_probe(void *arg)
{
    return (void *)(uintptr_t)check_vendor();
}
#endif

int main(int argc, char **argv)
{
    int result;
#ifdef _WIN32
    HANDLE thread;
    DWORD exit_code;
#else
    struct sigaction action = { .sa_sigaction = exception_handler, .sa_flags = SA_SIGINFO };
    pthread_t thread;
    void *thread_result;
    unsigned int baseline[4], observed[4];
    uintptr_t pc, ax = 0, bx = 1, cx = 2, dx = 3;
    static const unsigned char nop[] = { 0x90, 0x90 };
    const char *env = getenv("WINE_HIDE_AMD_CPU");
    int enabled = env && env[0] == '1';
#endif

    if (argc != 2) return 2;
    expected_vendor = argv[1];
#ifdef _WIN32
    if (!AddVectoredExceptionHandler(1, exception_handler)) return 2;
#else
    if (sigaction(SIGSEGV, &action, NULL)) return 2;
    cpuid(7, 0, baseline);
    emulate_cpuid();
    pc = enabled ? (uintptr_t)nop : 1;
    if (handle_cpuid_fault(&pc, &ax, &bx, &cx, &dx) || bx != 1 || cx != 2 || dx != 3) return 1;
    cpuid(7, 0, observed);
    if (memcmp(baseline, observed, sizeof(baseline))) return 1;
#ifdef __x86_64__
    ax = (uintptr_t)1 << 32 | 7;
    cx = (uintptr_t)1 << 32;
    __asm__ volatile("cpuid" : "+a"(ax), "=b"(bx), "+c"(cx), "=d"(dx));
    if (ax != baseline[0] || bx != baseline[1] || cx != baseline[2] || dx != baseline[3]) return 1;
#endif
    if (enabled && syscall(SYS_arch_prctl, ARCH_GET_CPUID, 0) != 0)
    {
        fprintf(stderr, "CPUID faulting unavailable\n");
        return 77;
    }
#endif
    result = check_vendor();
    __asm__ volatile("hlt");
    if (other_faults != 1) result = 1;
    result |= check_vendor();
#ifdef _WIN32
    thread = CreateThread(NULL, 0, thread_probe, NULL, 0, NULL);
    if (!thread || WaitForSingleObject(thread, 10000) != WAIT_OBJECT_0) return 2;
    if (!GetExitCodeThread(thread, &exit_code)) return 2;
    CloseHandle(thread);
    result |= exit_code;
#else
    if (pthread_create(&thread, NULL, thread_probe, NULL) || pthread_join(thread, &thread_result)) return 2;
    result |= (uintptr_t)thread_result;
    if (enabled ? cpuid_faults < 61 : cpuid_faults != 0) result = 1;
#endif
    if (!result) printf("CPUID vendor %s, thread and exception checks passed\n", expected_vendor);
    return result;
}
