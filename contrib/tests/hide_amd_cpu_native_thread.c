/* LD_PRELOAD companion to hide_amd_cpu.c. Run CPUID before a new Wine thread
 * has entered its original start routine or allocated a Wine signal stack. */
#define _GNU_SOURCE
#include <asm/prctl.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

struct thread_start
{
    void *(*function)(void *);
    void *argument;
};

static int reported;

static void *native_thread_start(void *argument)
{
    struct thread_start start = *(struct thread_start *)argument;
    unsigned int regs[4];
    char vendor[13];

    free(argument);
    if (!syscall(SYS_arch_prctl, ARCH_GET_CPUID, 0))
    {
        __asm__ volatile("cpuid" : "=a"(regs[0]), "=b"(regs[1]), "=c"(regs[2]), "=d"(regs[3])
                         : "a"(0), "c"(0));
        memcpy(vendor, regs + 1, 4);
        memcpy(vendor + 4, regs + 3, 4);
        memcpy(vendor + 8, regs + 2, 4);
        vendor[12] = 0;
        if (strcmp(vendor, "GenuineIntel")) _exit(3);
        if (__sync_bool_compare_and_swap(&reported, 0, 1))
            fprintf(stderr, "Native thread CPUID before Wine thread setup passed\n");
    }
    return start.function(start.argument);
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
                   void *(*function)(void *), void *argument)
{
    int (*create)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
    struct thread_start *start;
    int ret;

    create = dlsym(RTLD_NEXT, "pthread_create");
    if (!create || !(start = malloc(sizeof(*start)))) _exit(3);
    start->function = function;
    start->argument = argument;
    ret = create(thread, attributes, native_thread_start, start);
    if (ret) free(start);
    return ret;
}
