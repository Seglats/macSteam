/* cpu_spin: burn a given number of cores at normal priority, for crash-hunt stress runs
 * (tests/finals_bisect/freeze_watch.py's sessions). One process, N threads, so stopping the
 * process stops all of them; SIGINT/SIGTERM set a volatile flag the threads check.
 *
 * clang -O2 cpu_spin.c -o cpu_spin && ./cpu_spin [cores]   (default 7; this Mac has 18)
 */
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
    (void)sig;
    stop = 1;
}

static void *spin(void *arg)
{
    (void)arg;
    while (!stop) {}
    return NULL;
}

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 7, i;
    pthread_t t[64];

    if (n < 1 || n > 64) { fprintf(stderr, "cores must be 1-64\n"); return 2; }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    for (i = 0; i < n; i++) pthread_create(&t[i], NULL, spin, NULL);
    printf("burning %d cores; stop with SIGINT/SIGTERM\n", n);
    fflush(stdout);
    for (i = 0; i < n; i++) pthread_join(t[i], NULL);
    return 0;
}
