#include "dccthread.h"
#include "dlist.h"
#include <ucontext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <errno.h>


typedef struct dccthread {
    char name[DCCTHREAD_MAX_NAME_SIZE];
    ucontext_t *context;
    dccthread_t *waiting; 
    unsigned int yield; // Thread cedeu a CPU
    unsigned int sleeping; // Thread está dormindo
} dccthread_t;

ucontext_t manager; // Contexto do escalonador
sigset_t mask; // Conjunto de sinais usado para bloquear/desbloquear sinais
struct dlist *endQueue; // Lista de threads concluídas

// Inicializa a biblioteca de threads
void dccthread_init(void (*func)(int), int param) {
    endQueue = dlist_create();
    dccthread_create("main", func, param);

    // Timer
    struct sigevent time_event;
    struct sigaction time_action;
    struct itimerspec time_interval;
    timer_t timerid;

    time_event.sigev_signo = SIGRTMIN;
    time_event.sigev_notify = SIGEV_SIGNAL;
    time_action.sa_handler = (void*)dccthread_yield;
    time_action.sa_flags = 0;

    time_interval.it_value.tv_sec = time_interval.it_interval.tv_sec = 0;
    time_interval.it_value.tv_nsec = time_interval.it_interval.tv_nsec = 10000000; // 10 ms

    sigaction(SIGRTMIN, &time_action, NULL);
    timer_create(CLOCK_PROCESS_CPUTIME_ID, &time_event, &timerid);
    timer_settime(timerid, 0, &time_interval, NULL);

    sigemptyset(&mask);
    sigaddset(&mask, SIGRTMIN);

    sigprocmask(SIG_BLOCK, &mask, NULL);

    while (!dlist_empty(endQueue)) {
        dccthread_t *next_thread = dlist_get_index(endQueue, 0);

        if (next_thread->sleeping || next_thread->waiting != NULL) {
            dlist_pop_left(endQueue);
            dlist_push_right(endQueue, next_thread);
            continue;
        }

        swapcontext(&manager, next_thread->context);

        dlist_pop_left(endQueue);

        if (next_thread->yield || next_thread->sleeping || next_thread->waiting != NULL) {
            next_thread->yield = 0;
            dlist_push_right(endQueue, next_thread);
        }
    }
    exit(0);
}

// Criando uma nova thread
dccthread_t * dccthread_create(const char *name, void (*func)(int ), int param) {
    ucontext_t *newContext = malloc(sizeof(ucontext_t));
    getcontext(newContext);
    
    sigprocmask(SIG_BLOCK, &mask, NULL);

    newContext->uc_link = &manager;
    newContext->uc_stack.ss_sp = malloc(THREAD_STACK_SIZE);
    newContext->uc_stack.ss_size = THREAD_STACK_SIZE;
    newContext->uc_stack.ss_flags = 0;

    dccthread_t *newThread = malloc(sizeof(dccthread_t));
    strcpy(newThread->name, name);
    newThread->context = newContext;
    newThread->yield = 0;
    newThread->sleeping = 0;
    newThread->waiting = NULL;
    dlist_push_right(endQueue, newThread);

    makecontext(newContext, (void (*)(void))func, 1, param);
    sigprocmask(SIG_UNBLOCK, &mask, NULL);
    
    return newThread;
}

// Cedendo a CPU para a próxima thread na fila
void dccthread_yield() {
    sigprocmask(SIG_BLOCK, &mask, NULL);

    dccthread_t *currentThread = dccthread_self();
    currentThread->yield = 1;
    
    swapcontext(currentThread->context, &manager);
    sigprocmask(SIG_UNBLOCK, &mask, NULL);
}

// Terminando a execução da thread atual e liberando recursos
void dccthread_exit(void) {
    sigprocmask(SIG_BLOCK, &mask, NULL);

    dccthread_t *currentThread = dccthread_self();

    for (int index = 0; index < endQueue->count; index++) {
        dccthread_t *thread = dlist_get_index(endQueue, index);
        if (thread->waiting == currentThread) {
            thread->waiting = NULL;
        }
    }
    free(currentThread->context->uc_stack.ss_sp);
    
    sigprocmask(SIG_UNBLOCK, &mask, NULL);
}

// Bloqueando a thread atual
void dccthread_wait(dccthread_t *tid) {
    sigprocmask(SIG_BLOCK, &mask, NULL);
    int index = -1; // Inicialize o índice com um valor que indica que o thread não foi encontrado

    // Procurar o thread na fila
    for (int i = 0; i < endQueue->count; i++) {
        dccthread_t *thread = dlist_get_index(endQueue, i);
        if (thread == tid) {
            index = i;
            break;
        }
    }
    // Se o thread foi encontrado na fila
    if (index != -1) {
        dccthread_t *currentThread = dccthread_self();
        currentThread->waiting = tid;
        swapcontext(currentThread->context, &manager);
    }
    
    sigprocmask(SIG_UNBLOCK, &mask, NULL);
}

void dccthread_sleep(struct timespec ts) {
    sigprocmask(SIG_BLOCK, &mask, NULL);

    dccthread_t *currentThread = dccthread_self();
    currentThread->sleeping = 1; // Indica que a thread está dormindo

    // Calcular o tempo de término
    struct timespec endTime;
    clock_gettime(CLOCK_REALTIME, &endTime);
    endTime.tv_sec += ts.tv_sec;
    endTime.tv_nsec += ts.tv_nsec;

    while (1) {
        int result = clock_gettime(CLOCK_REALTIME, &endTime);
        if (result < 0) {
            perror("clock_gettime");
            exit(1);
        }
        if (result == 0 && endTime.tv_sec >= ts.tv_sec && endTime.tv_nsec >= ts.tv_nsec) {
            break;
        }
        dccthread_yield(); // Cedem a CPU 
    }

    currentThread->sleeping = 0; // Indica que a thread acordou
    sigprocmask(SIG_UNBLOCK, &mask, NULL);
}



dccthread_t * dccthread_self(void) {
    dccthread_t *current_thread = dlist_get_index(endQueue, 0);
    return current_thread;
}

const char * dccthread_name(dccthread_t *tid) {
    return tid->name;
}
