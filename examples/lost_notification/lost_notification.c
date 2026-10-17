/* Closed-world notification protocol: no timers, signals or external actors.
 * Main holds the mutex before creating the producer, then atomically releases it
 * in cond_wait. INJECT_BUG removes the producer's notification. */
#include <pthread.h>
#include <unistd.h>

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int ready;

static void *produce(void *unused) {
    (void)unused;
    if (pthread_mutex_lock(&mutex)) _exit(2);
    ready = 1;
#ifndef INJECT_BUG
    if (pthread_cond_signal(&changed)) _exit(2);
#endif
    if (pthread_mutex_unlock(&mutex)) _exit(2);
    return 0;
}

int main(void) {
    pthread_t worker;
    if (pthread_mutex_lock(&mutex)) return 2;
    if (pthread_create(&worker, 0, produce, 0)) return 2;
    while (!ready) {
        if (pthread_cond_wait(&changed, &mutex)) return 2;
    }
    if (pthread_mutex_unlock(&mutex) || pthread_join(worker, 0)) return 2;
    static const char success[] = "RUNTIME_OK\n";
    return write(STDOUT_FILENO, success, sizeof(success)-1) == sizeof(success)-1 ? 0 : 2;
}
