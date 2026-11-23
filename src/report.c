/*
 * report.c  -  Part 6: the report process.
 *
 * Started by the scheduler with fork() + exec():
 *      report <shared-memory-name> [quiet]
 *
 * It attaches to the POSIX shared-memory segment, BLOCKS on the process-shared
 * semaphore stored in the segment, and prints the final report only after the
 * scheduler has finished writing the metrics and posted the semaphore.
 * With the optional argument "quiet" it performs the same synchronisation
 * but prints nothing (used by --table-only).
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "common.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: report <shm-name> [quiet]\n");
        return 2;
    }
    int quiet = (argc > 2 && strcmp(argv[2], "quiet") == 0);

    int fd = shm_open(argv[1], O_RDWR, 0);
    if (fd < 0) { perror("report: shm_open"); return 1; }

    SharedReport *sh = mmap(NULL, sizeof *sh, PROT_READ | PROT_WRITE,
                            MAP_SHARED, fd, 0);
    if (sh == MAP_FAILED) { perror("report: mmap"); close(fd); return 1; }
    close(fd);                         /* the mapping stays valid */

    /* Wait until the scheduler says "the data is complete". */
    while (sem_wait(&sh->ready) == -1 && errno == EINTR) { }

    if (!quiet) {
        const ReportData *d = &sh->data;
        printf("\n=================== FINAL REPORT ===================\n");
        printf("(read from shared memory by report process, pid %d)\n", (int)getpid());
        printf("Algorithm          : %s\n", d->algo);
        if (d->cs_cost > 0)
            printf("Context-switch cost: %d time unit(s)\n", d->cs_cost);
        printf("\n%-6s %8s %6s %11s %11s %8s %9s\n",
               "Job", "Arrival", "Burst", "Completion", "Turnaround", "Waiting", "Response");
        for (int i = 0; i < d->n; i++) {
            const JobMetrics *m = &d->jobs[i];
            printf("%-6s %8d %6d %11d %11d %8d %9d\n", m->id, m->arrival, m->burst,
                   m->completion, m->turnaround, m->waiting, m->response);
        }
        printf("%-6s %8s %6s %11s %11.2f %8.2f %9.2f\n", "Avg", "", "", "",
               d->avg_tat, d->avg_wait, d->avg_resp);

        int busy = d->total_time - d->idle_time - d->cs_overhead;
        printf("\nContext switches   : %d\n", d->ctx_switches);
        printf("Total time         : %d units (CPU idle %d, switch overhead %d)\n",
               d->total_time, d->idle_time, d->cs_overhead);
        printf("CPU utilisation    : %.1f%% (useful work %d / %d)\n",
               d->total_time ? 100.0 * busy / d->total_time : 0.0, busy, d->total_time);
        printf("====================================================\n");
        fflush(stdout);
    }

    munmap(sh, sizeof *sh);
    return 0;
}
