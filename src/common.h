/*
 * common.h  -  data shared by the scheduler, logger and report programs.
 *
 * SharedReport is the layout of the POSIX shared-memory segment that the
 * scheduler fills in and the report process reads (Part 6).
 */
#ifndef COMMON_H
#define COMMON_H

#include <semaphore.h>

#define MAX_JOBS 64     /* the brief asks for at least 10 jobs            */
#define ID_LEN   16     /* longest job id is ID_LEN-1 characters          */
#define NAME_LEN 40     /* algorithm display name                         */

/* Final numbers of one job. */
typedef struct {
    char id[ID_LEN];
    int  arrival;
    int  burst;
    int  completion;
    int  turnaround;    /* completion - arrival                           */
    int  waiting;       /* turnaround - burst                             */
    int  response;      /* first CPU allocation - arrival                 */
} JobMetrics;

/* Final numbers of one complete scheduling run. */
typedef struct {
    char   algo[NAME_LEN];   /* e.g. "Round Robin (q = 2)"                */
    int    quantum;          /* 0 when the algorithm has no quantum       */
    int    cs_cost;          /* context-switch cost in time units (bonus) */
    int    n;                /* number of jobs                            */
    int    ctx_switches;
    int    total_time;       /* time at which the last job finished       */
    int    idle_time;        /* time units the CPU was IDLE               */
    int    cs_overhead;      /* time units spent on switching (bonus)     */
    double avg_wait;
    double avg_tat;
    double avg_resp;
    JobMetrics jobs[MAX_JOBS];
} ReportData;

/* Layout of the shared-memory segment. */
typedef struct {
    sem_t      ready;        /* process-shared semaphore, initial value 0 */
    ReportData data;
} SharedReport;

#endif
