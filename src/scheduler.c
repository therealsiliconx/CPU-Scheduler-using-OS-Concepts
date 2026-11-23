/*
 * scheduler.c  -  Mini CPU Scheduler (OS Lab project)
 *
 *   Part 1  Job table            load_jobs(), parse_int(), id_cmp()
 *   Part 2  Algorithms           rq_pick(), better(), slice_for()
 *   Part 3  Live execution       job_thread(), arrival_thread(), dispatcher()
 *   Part 4  Output and metrics   print_gantt(), print_job_table(), print_comparison()
 *   Part 5  Logger process       start_logger(), stop_logger()   (+ logger.c)
 *   Part 6  Shared-memory report run_report()                    (+ report.c)
 *   Bonus   Context-switch cost  --cs-cost N, and the "sweep" mode
 *
 * Build : make          Run : ./scheduler <input_file> <algorithm> [quantum]
 *
 * ----------------------------------------------------------------------------
 *  HOW THE THREADS TALK TO EACH OTHER  (see README.txt for the full table)
 *
 *   main thread = DISPATCHER      picks the next job, posts run_sem[job],
 *                                 then blocks on `done` until the job's slice
 *                                 is over.
 *   JOB THREAD (one per job)      blocks on its own run_sem; when posted it
 *                                 runs `slice` time units, one unit at a time,
 *                                 printing one line per unit, then posts `done`.
 *   ARRIVAL THREAD                blocks on arr_go; every time the clock moves
 *                                 it wakes up, puts every job whose arrival
 *                                 time has been reached into the ready queue,
 *                                 and posts arr_done.
 *
 *   The simulated clock `now` is advanced by whichever thread currently owns
 *   the CPU (job thread, or dispatcher while idle / switching) in tick_clock().
 *   Because all hand-offs are semaphores, the program is deterministic and
 *   never busy-waits.  sleep is used ONLY to simulate one unit of work.
 * ----------------------------------------------------------------------------
 */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "common.h"

/* ------------------------------------------------------------------------- */
/*  Constants                                                                */
/* ------------------------------------------------------------------------- */
#define DEFAULT_UNIT_MS  100        /* ONE TIME UNIT = 100 ms of real time   */
#define DEFAULT_QUANTUM  2          /* used by rr / all when none is given   */
#define MAX_VALUE        100000     /* sanity limit for arrival / burst      */
#define LOG_FILE         "schedule.log"

typedef enum { ALGO_FCFS = 0, ALGO_SJF, ALGO_SRTF, ALGO_RR } Algo;
static const char *ALGO_SHORT[] = { "FCFS", "SJF", "SRTF", "RR" };

/* One line of the input file. */
typedef struct {
    char id[ID_LEN];
    int  arrival;
    int  burst;
} JobSpec;

/* ------------------------------------------------------------------------- */
/*  Small helpers                                                            */
/* ------------------------------------------------------------------------- */
static void die(const char *msg)
{
    perror(msg);
    exit(EXIT_FAILURE);
}

/* sem_wait that survives signals. */
static void sem_wait_r(sem_t *s)
{
    while (sem_wait(s) == -1 && errno == EINTR) { }
}

/* Simulate work: sleep for `us` microseconds (0 = do not sleep at all). */
static void nap(int us)
{
    if (us <= 0)
        return;
    struct timespec ts;
    ts.tv_sec  = us / 1000000;
    ts.tv_nsec = (long)(us % 1000000) * 1000L;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) { }
}

/* Full-string integer parser. Returns 1 on success. */
static int parse_int(const char *s, long *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || errno == ERANGE)
        return 0;
    *out = v;
    return 1;
}

/* "Natural" id comparison so that P2 < P10. */
static int id_cmp(const char *a, const char *b)
{
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            char *ea, *eb;
            long x = strtol(a, &ea, 10);
            long y = strtol(b, &eb, 10);
            if (x != y)
                return x < y ? -1 : 1;
            a = ea;
            b = eb;
        } else {
            if (*a != *b)
                return (unsigned char)*a < (unsigned char)*b ? -1 : 1;
            a++;
            b++;
        }
    }
    if (*a) return 1;
    if (*b) return -1;
    return 0;
}

/* Path of a program that lives next to the scheduler executable. */
static void sibling_path(const char *name, char *out, size_t sz)
{
    char self[PATH_MAX];
    ssize_t r = readlink("/proc/self/exe", self, sizeof self - 1);
    if (r > 0) {
        self[r] = '\0';
        char *slash = strrchr(self, '/');
        if (slash) {
            *slash = '\0';
            snprintf(out, sz, "%s/%s", self, name);
            return;
        }
    }
    snprintf(out, sz, "./%s", name);
}

/* ========================================================================= */
/*  PART 1 - JOB TABLE                                                       */
/* ========================================================================= */

/*
 * Reads and validates the input file.  Every problem is reported with its
 * line number; nothing crashes.  Returns 0 on success, -1 on any error.
 */
static int load_jobs(const char *path, JobSpec *jobs, int *count)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Error: cannot open input file '%s': %s\n", path, strerror(errno));
        return -1;
    }

    char line[512];
    int lineno = 0, n = 0, errors = 0;

    while (fgets(line, sizeof line, f) != NULL) {
        lineno++;

        /* A line longer than the buffer: complain and throw the rest away. */
        if (strchr(line, '\n') == NULL && !feof(f)) {
            fprintf(stderr, "Error: line %d is too long\n", lineno);
            errors++;
            int ch;
            while ((ch = fgetc(f)) != '\n' && ch != EOF) { }
            continue;
        }

        char *hash = strchr(line, '#');          /* strip comments */
        if (hash)
            *hash = '\0';

        char *p = line;
        while (isspace((unsigned char)*p))
            p++;
        if (*p == '\0')
            continue;                            /* blank or comment line */

        char id[64], a[64], b[64], extra[64];
        int fields = sscanf(p, "%63s %63s %63s %63s", id, a, b, extra);
        if (fields < 3) {
            fprintf(stderr, "Error: line %d: expected '<job_id> <arrival> <burst>'\n", lineno);
            errors++;
            continue;
        }
        if (fields > 3) {
            fprintf(stderr, "Error: line %d: too many fields\n", lineno);
            errors++;
            continue;
        }

        long arr, bur;
        if (!parse_int(a, &arr) || !parse_int(b, &bur)) {
            fprintf(stderr, "Error: line %d: arrival and burst must be whole numbers\n", lineno);
            errors++;
            continue;
        }
        if (arr < 0 || bur < 0) {
            fprintf(stderr, "Error: line %d: negative numbers are not allowed (job %s)\n", lineno, id);
            errors++;
            continue;
        }
        if (bur == 0) {
            fprintf(stderr, "Error: line %d: burst time of job %s must be at least 1\n", lineno, id);
            errors++;
            continue;
        }
        if (arr > MAX_VALUE || bur > MAX_VALUE) {
            fprintf(stderr, "Error: line %d: values larger than %d are not supported\n", lineno, MAX_VALUE);
            errors++;
            continue;
        }
        if (strlen(id) >= ID_LEN) {
            fprintf(stderr, "Error: line %d: job id '%s' is too long (max %d characters)\n",
                    lineno, id, ID_LEN - 1);
            errors++;
            continue;
        }
        int dup = 0;
        for (int i = 0; i < n; i++)
            if (strcmp(jobs[i].id, id) == 0)
                dup = 1;
        if (dup) {
            fprintf(stderr, "Error: line %d: duplicate job id '%s'\n", lineno, id);
            errors++;
            continue;
        }
        if (n >= MAX_JOBS) {
            fprintf(stderr, "Error: line %d: too many jobs (maximum is %d)\n", lineno, MAX_JOBS);
            errors++;
            continue;
        }

        memcpy(jobs[n].id, id, strlen(id) + 1);   /* length checked above */
        jobs[n].arrival = (int)arr;
        jobs[n].burst   = (int)bur;
        n++;
    }
    fclose(f);

    if (errors > 0) {
        fprintf(stderr, "Input rejected: %d problem(s) found in '%s'.\n", errors, path);
        return -1;
    }
    if (n == 0) {
        fprintf(stderr, "Error: '%s' contains no jobs (empty file or only comments).\n", path);
        return -1;
    }

    /* Keep the table sorted by job id: index order == "smaller id" order,
     * which makes the tie-break rule of the brief trivial to apply. */
    for (int i = 1; i < n; i++) {
        JobSpec key = jobs[i];
        int j = i - 1;
        while (j >= 0 && id_cmp(jobs[j].id, key.id) > 0) {
            jobs[j + 1] = jobs[j];
            j--;
        }
        jobs[j + 1] = key;
    }
    *count = n;
    return 0;
}

/* ========================================================================= */
/*  Simulation state                                                         */
/* ========================================================================= */
typedef struct Ctx Ctx;

typedef struct {
    char      id[ID_LEN];
    int       arrival;
    int       burst;
    int       remaining;     /* mutex                                         */
    int       first_start;   /* -1 until the first CPU allocation (mutex)     */
    int       completion;    /* mutex                                         */
    int       slice;         /* units to run; set by dispatcher, -1 = quit    */
    sem_t     run_sem;       /* initial value 0: job thread sleeps on it      */
    pthread_t tid;
    Ctx      *ctx;
} Job;

typedef enum { SEG_JOB, SEG_IDLE, SEG_CS } SegKind;
typedef struct {
    SegKind kind;
    int     job;             /* index into jobs[] for SEG_JOB                 */
    int     start;
    int     end;
} Seg;

struct Ctx {
    Job     *jobs;
    int      n;
    int     *arr_order;      /* job indices sorted by (arrival, id)           */
    Algo     algo;
    int      quantum;
    int      cs_cost;
    int      unit_us;
    int      verbose;        /* print the live console output?                */
    int      log_fd;         /* write end of the logger pipe, or -1           */

    pthread_mutex_t mutex;   /* protects EVERYTHING below                     */
    int      now;            /* simulated clock                               */
    int      rq[MAX_JOBS];   /* ready queue (FIFO order of insertion)         */
    int      rq_len;
    int      released;       /* how many jobs the arrival thread has released */
    Seg     *segs;           /* Gantt log                                     */
    int      nsegs, capsegs;
    int      ctx_switches;
    int      idle_time;
    int      cs_overhead;

    sem_t    done;           /* job thread -> dispatcher, initial value 0     */
    sem_t    arr_go;         /* clock owner -> arrival thread, initial 0      */
    sem_t    arr_done;       /* arrival thread -> clock owner, initial 0      */
};

/* ---- output helpers: caller must hold c->mutex ---------------------------- */
static void con(Ctx *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void con(Ctx *c, const char *fmt, ...)
{
    if (!c->verbose)
        return;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

/* Send one event line to the logger process through the pipe (Part 5). */
static void logev(Ctx *c, int t, const char *event, const char *job)
{
    if (c->log_fd < 0)
        return;
    char buf[160];
    int len = snprintf(buf, sizeof buf, "[t=%3d] [%-4s] %-9s %s\n",
                       t, ALGO_SHORT[c->algo], event, job);
    if (len > 0 && write(c->log_fd, buf, (size_t)len) < 0) {
        /* logger is gone - nothing sensible to do */
    }
}

/* ---- Gantt log: caller must hold c->mutex --------------------------------- */
static void gantt_add(Ctx *c, SegKind kind, int job, int start, int end)
{
    if (c->nsegs > 0) {
        Seg *l = &c->segs[c->nsegs - 1];
        if (l->kind == kind && l->job == job && l->end == start) {
            l->end = end;                       /* extend the previous bar */
            return;
        }
    }
    if (c->nsegs == c->capsegs) {
        c->capsegs = c->capsegs ? c->capsegs * 2 : 64;
        Seg *bigger = realloc(c->segs, (size_t)c->capsegs * sizeof *bigger);
        if (!bigger)
            die("realloc");
        c->segs = bigger;
    }
    Seg *s = &c->segs[c->nsegs++];
    s->kind  = kind;
    s->job   = job;
    s->start = start;
    s->end   = end;
}

/* ========================================================================= */
/*  PART 2 - SCHEDULING ALGORITHMS (ready queue selection)                   */
/* ========================================================================= */

/* The ready queue is a plain array in insertion order. Mutex must be held. */
static void rq_push(Ctx *c, int idx)
{
    c->rq[c->rq_len++] = idx;
}

/*
 * Is candidate `a` better than candidate `b` for SJF / SRTF?
 *   1. smaller key (burst for SJF, remaining time for SRTF)
 *   2. SRTF only: the job that was running keeps the CPU on a tie
 *   3. earlier arrival
 *   4. smaller job id  (== smaller index, the table is sorted by id)
 */
static int better(const Ctx *c, int a, int b, int last)
{
    const Job *ja = &c->jobs[a], *jb = &c->jobs[b];
    int ka = (c->algo == ALGO_SJF) ? ja->burst : ja->remaining;
    int kb = (c->algo == ALGO_SJF) ? jb->burst : jb->remaining;
    if (ka != kb)
        return ka < kb;
    if (c->algo == ALGO_SRTF) {
        if (a == last) return 1;
        if (b == last) return 0;
    }
    if (ja->arrival != jb->arrival)
        return ja->arrival < jb->arrival;
    return a < b;
}

/*
 * Choose (and remove) the next job according to the selected algorithm.
 * FCFS and Round Robin use the queue order; SJF and SRTF use better().
 * Returns -1 when the queue is empty (CPU idle). Mutex must be held.
 */
static int rq_pick(Ctx *c, int last)
{
    if (c->rq_len == 0)
        return -1;

    int best = 0;
    if (c->algo == ALGO_SJF || c->algo == ALGO_SRTF) {
        for (int i = 1; i < c->rq_len; i++)
            if (better(c, c->rq[i], c->rq[best], last))
                best = i;
    }
    int idx = c->rq[best];
    memmove(&c->rq[best], &c->rq[best + 1], (size_t)(c->rq_len - best - 1) * sizeof(int));
    c->rq_len--;
    return idx;
}

/* How many time units may the chosen job run before the dispatcher looks again? */
static int slice_for(const Ctx *c, const Job *j)
{
    switch (c->algo) {
    case ALGO_FCFS:
    case ALGO_SJF:  return j->remaining;                        /* non-preemptive */
    case ALGO_SRTF: return 1;                                   /* check every unit */
    case ALGO_RR:   return j->remaining < c->quantum ? j->remaining : c->quantum;
    }
    return j->remaining;
}

/* ========================================================================= */
/*  PART 3 - LIVE EXECUTION                                                  */
/* ========================================================================= */

/*
 * Move the simulated clock forward by ONE time unit.  Called by the thread
 * that currently "owns" the CPU.  After the clock moved, the arrival thread
 * is woken (semaphore) so that jobs arriving at the new time are in the ready
 * queue before anybody makes the next decision.
 */
static void tick_clock(Ctx *c)
{
    nap(c->unit_us);                            /* one unit of simulated work */

    pthread_mutex_lock(&c->mutex);
    c->now++;
    int more = (c->released < c->n);
    pthread_mutex_unlock(&c->mutex);

    if (more) {
        sem_post(&c->arr_go);
        sem_wait_r(&c->arr_done);
    }
}

/* Arrival thread: releases jobs into the ready queue at their arrival time. */
static void *arrival_thread(void *arg)
{
    Ctx *c = arg;

    for (;;) {
        sem_wait_r(&c->arr_go);                 /* sleep until the clock moved */

        pthread_mutex_lock(&c->mutex);
        int t = c->now;
        while (c->released < c->n &&
               c->jobs[c->arr_order[c->released]].arrival <= t) {
            int idx = c->arr_order[c->released];
            Job *j = &c->jobs[idx];
            rq_push(c, idx);
            c->released++;
            con(c, "[t=%2d] %s arrived (burst %d)\n", j->arrival, j->id, j->burst);
            logev(c, j->arrival, "arrived", j->id);
        }
        int finished_all = (c->released == c->n);
        pthread_mutex_unlock(&c->mutex);

        sem_post(&c->arr_done);
        if (finished_all)
            break;
    }
    return NULL;
}

/* Job thread: only runs when the dispatcher posts its own semaphore. */
static void *job_thread(void *arg)
{
    Job *j = arg;
    Ctx *c = j->ctx;

    for (;;) {
        sem_wait_r(&j->run_sem);                /* blocked until dispatched */

        pthread_mutex_lock(&c->mutex);
        int slice = j->slice;
        pthread_mutex_unlock(&c->mutex);
        if (slice <= 0)
            break;                              /* dispatcher says: terminate */

        for (int u = 0; u < slice; u++) {
            pthread_mutex_lock(&c->mutex);
            con(c, "[t=%2d] %s running (remaining %d)\n", c->now, j->id, j->remaining);
            pthread_mutex_unlock(&c->mutex);

            pthread_mutex_lock(&c->mutex);
            j->remaining--;                     /* this unit is now executed */
            pthread_mutex_unlock(&c->mutex);

            tick_clock(c);                      /* time passes, arrivals are checked */
        }
        sem_post(&c->done);                     /* tell the dispatcher: slice over */
    }
    return NULL;
}

/* Dispatcher: runs in the calling (main) thread of simulate(). */
static void dispatcher(Ctx *c)
{
    int finished = 0;
    int last = -1;               /* job that ran last; -1 after idle / at start */

    while (finished < c->n) {
        pthread_mutex_lock(&c->mutex);
        int pick = rq_pick(c, last);
        int t = c->now;

        /* ---- nothing is ready: the CPU idles for one time unit ---- */
        if (pick < 0) {
            con(c, "[t=%2d] CPU IDLE\n", t);
            gantt_add(c, SEG_IDLE, -1, t, t + 1);
            c->idle_time++;
            pthread_mutex_unlock(&c->mutex);
            tick_clock(c);
            last = -1;           /* switches to/from IDLE are not counted */
            continue;
        }

        Job *j = &c->jobs[pick];

        /* ---- context switch (job -> different job) ---- */
        if (last >= 0 && last != pick) {
            c->ctx_switches++;
            if (c->jobs[last].remaining > 0)
                logev(c, t, "preempted", c->jobs[last].id);
            if (c->cs_cost > 0)
                con(c, "[t=%2d] ---- context switch: %s -> %s (cost %d) ----\n",
                    t, c->jobs[last].id, j->id, c->cs_cost);
            else
                con(c, "[t=%2d] ---- context switch: %s -> %s ----\n",
                    t, c->jobs[last].id, j->id);
            pthread_mutex_unlock(&c->mutex);

            /* BONUS: the CPU is busy switching, no job makes progress. */
            for (int k = 0; k < c->cs_cost; k++) {
                pthread_mutex_lock(&c->mutex);
                gantt_add(c, SEG_CS, -1, c->now, c->now + 1);
                c->cs_overhead++;
                pthread_mutex_unlock(&c->mutex);
                tick_clock(c);
            }
            pthread_mutex_lock(&c->mutex);
            t = c->now;
        }

        /* ---- first CPU allocation, started / resumed events ---- */
        int first_run = (j->first_start < 0);
        if (first_run)
            j->first_start = t;
        if (last != pick)
            logev(c, t, first_run ? "started" : "resumed", j->id);

        j->slice = slice_for(c, j);
        int slice_start = t;
        pthread_mutex_unlock(&c->mutex);

        /* ---- hand the CPU to the job thread and wait for it ---- */
        sem_post(&j->run_sem);                  /* step 2 of Figure 2 */
        sem_wait_r(&c->done);                   /* step 5 of Figure 2 */

        pthread_mutex_lock(&c->mutex);
        t = c->now;
        gantt_add(c, SEG_JOB, pick, slice_start, t);
        if (j->remaining == 0) {
            j->completion = t;
            finished++;
            con(c, "[t=%2d] %s finished\n", t, j->id);
            logev(c, t, "finished", j->id);
        } else {
            rq_push(c, pick);                   /* back of the queue (arrivals at
                                                   time t are already in it)     */
        }
        pthread_mutex_unlock(&c->mutex);
        last = pick;
    }
}

/*
 * Run one complete simulation.  Fills `out` (metrics) and returns the Gantt
 * segments in *segs_out (caller frees).  Returns 0 on success.
 */
static int simulate(const JobSpec *specs, int n, Algo algo, int quantum, int cs_cost,
                    int unit_us, int verbose, int log_fd,
                    ReportData *out, Seg **segs_out, int *nsegs_out)
{
    Ctx c;
    memset(&c, 0, sizeof c);

    c.jobs      = calloc((size_t)n, sizeof *c.jobs);
    c.arr_order = malloc((size_t)n * sizeof(int));
    if (!c.jobs || !c.arr_order)
        die("malloc");

    c.n = n;
    c.algo = algo;
    c.quantum = quantum;
    c.cs_cost = cs_cost;
    c.unit_us = unit_us;
    c.verbose = verbose;
    c.log_fd = log_fd;

    for (int i = 0; i < n; i++) {
        Job *j = &c.jobs[i];
        snprintf(j->id, ID_LEN, "%s", specs[i].id);
        j->arrival     = specs[i].arrival;
        j->burst       = specs[i].burst;
        j->remaining   = specs[i].burst;
        j->first_start = -1;
        j->completion  = -1;
        j->ctx         = &c;
        if (sem_init(&j->run_sem, 0, 0) != 0)
            die("sem_init");
        c.arr_order[i] = i;
    }
    /* order of arrival: by arrival time, then by id (index) */
    for (int i = 1; i < n; i++) {
        int key = c.arr_order[i], k = i - 1;
        while (k >= 0 && (c.jobs[c.arr_order[k]].arrival > c.jobs[key].arrival ||
               (c.jobs[c.arr_order[k]].arrival == c.jobs[key].arrival && c.arr_order[k] > key))) {
            c.arr_order[k + 1] = c.arr_order[k];
            k--;
        }
        c.arr_order[k + 1] = key;
    }

    if (pthread_mutex_init(&c.mutex, NULL) != 0) die("pthread_mutex_init");
    if (sem_init(&c.done, 0, 0) != 0)     die("sem_init");
    if (sem_init(&c.arr_go, 0, 0) != 0)   die("sem_init");
    if (sem_init(&c.arr_done, 0, 0) != 0) die("sem_init");

    /* one thread per job + one arrival thread */
    pthread_t arr_tid;
    for (int i = 0; i < n; i++)
        if (pthread_create(&c.jobs[i].tid, NULL, job_thread, &c.jobs[i]) != 0)
            die("pthread_create");
    if (pthread_create(&arr_tid, NULL, arrival_thread, &c) != 0)
        die("pthread_create");

    /* release everything that arrives at time 0, then start dispatching */
    sem_post(&c.arr_go);
    sem_wait_r(&c.arr_done);

    dispatcher(&c);

    /* ---- orderly shutdown: tell every job thread to quit, join all ---- */
    for (int i = 0; i < n; i++) {
        pthread_mutex_lock(&c.mutex);
        c.jobs[i].slice = -1;
        pthread_mutex_unlock(&c.mutex);
        sem_post(&c.jobs[i].run_sem);
    }
    for (int i = 0; i < n; i++)
        pthread_join(c.jobs[i].tid, NULL);
    pthread_join(arr_tid, NULL);

    /* ---- collect the metrics ---- */
    memset(out, 0, sizeof *out);
    switch (algo) {
    case ALGO_RR:
        snprintf(out->algo, NAME_LEN, "Round Robin (q = %d)", quantum);
        out->quantum = quantum;
        break;
    case ALGO_FCFS: snprintf(out->algo, NAME_LEN, "FCFS"); break;
    case ALGO_SJF:  snprintf(out->algo, NAME_LEN, "SJF");  break;
    case ALGO_SRTF: snprintf(out->algo, NAME_LEN, "SRTF"); break;
    }
    out->cs_cost      = cs_cost;
    out->n            = n;
    out->ctx_switches = c.ctx_switches;
    out->total_time   = c.now;
    out->idle_time    = c.idle_time;
    out->cs_overhead  = c.cs_overhead;

    double sw = 0, st = 0, sr = 0;
    for (int i = 0; i < n; i++) {
        const Job *j = &c.jobs[i];
        JobMetrics *m = &out->jobs[i];
        snprintf(m->id, ID_LEN, "%s", j->id);
        m->arrival    = j->arrival;
        m->burst      = j->burst;
        m->completion = j->completion;
        m->turnaround = j->completion - j->arrival;
        m->waiting    = m->turnaround - j->burst;
        m->response   = j->first_start - j->arrival;
        sw += m->waiting;
        st += m->turnaround;
        sr += m->response;
    }
    out->avg_wait = sw / n;
    out->avg_tat  = st / n;
    out->avg_resp = sr / n;

    *segs_out  = c.segs;
    *nsegs_out = c.nsegs;

    /* ---- clean up ---- */
    for (int i = 0; i < n; i++)
        sem_destroy(&c.jobs[i].run_sem);
    sem_destroy(&c.done);
    sem_destroy(&c.arr_go);
    sem_destroy(&c.arr_done);
    pthread_mutex_destroy(&c.mutex);
    free(c.jobs);
    free(c.arr_order);
    return 0;
}

/* ========================================================================= */
/*  PART 4 - OUTPUT AND METRICS                                              */
/* ========================================================================= */

static void put(char *buf, int col, const char *s)
{
    for (int i = 0; s[i]; i++)
        buf[col + i] = s[i];
}

static int num_digits(int v)
{
    int d = 1;
    while (v >= 10) { v /= 10; d++; }
    return d;
}

/* Text Gantt chart. Long charts wrap onto several rows. */
static void print_gantt(const Seg *segs, int nsegs, const ReportData *rd)
{
    printf("\nGantt chart:\n");
    int i = 0;
    while (i < nsegs) {
        char l1[600], l2[600];
        memset(l1, ' ', sizeof l1);
        memset(l2, ' ', sizeof l2);
        int col = 0, first = i;

        while (i < nsegs) {
            char label[ID_LEN];
            if (segs[i].kind == SEG_IDLE)      snprintf(label, sizeof label, "IDLE");
            else if (segs[i].kind == SEG_CS)   snprintf(label, sizeof label, "CS");
            else                               snprintf(label, sizeof label, "%s", rd->jobs[segs[i].job].id);

            int dur = segs[i].end - segs[i].start;
            int w = dur * 2;
            if (w > 40) w = 40;
            int need = (int)strlen(label) + 2;
            if (w < need) w = need;
            if (w < num_digits(segs[i].end) + 2) w = num_digits(segs[i].end) + 2;
            if (i > first && col + w > 90)
                break;

            char tm[16];
            l1[col] = '|';
            put(l1, col + 1 + (w - 1 - (int)strlen(label)) / 2, label);
            snprintf(tm, sizeof tm, "%d", segs[i].start);
            put(l2, col, tm);
            col += w;
            i++;
        }
        char tm[16];
        l1[col] = '|';
        l1[col + 1] = '\0';
        snprintf(tm, sizeof tm, "%d", segs[i - 1].end);
        put(l2, col, tm);
        l2[col + (int)strlen(tm)] = '\0';
        printf("%s\n%s\n", l1, l2);
    }
    printf("(time in units;  CS = context-switch overhead;  IDLE = no job ready)\n");

    printf("Arrivals: ");
    for (int k = 0; k < rd->n; k++)
        printf("%s@%d%s", rd->jobs[k].id, rd->jobs[k].arrival, k + 1 < rd->n ? "  " : "\n");
}

static void print_job_table(const ReportData *rd)
{
    printf("\n%-6s %8s %6s %11s %11s %8s %9s\n",
           "Job", "Arrival", "Burst", "Completion", "Turnaround", "Waiting", "Response");
    for (int i = 0; i < rd->n; i++) {
        const JobMetrics *m = &rd->jobs[i];
        printf("%-6s %8d %6d %11d %11d %8d %9d\n", m->id, m->arrival, m->burst,
               m->completion, m->turnaround, m->waiting, m->response);
    }
    printf("%-6s %8s %6s %11s %11.2f %8.2f %9.2f\n", "Average", "", "", "",
           rd->avg_tat, rd->avg_wait, rd->avg_resp);

    int busy = rd->total_time - rd->idle_time - rd->cs_overhead;
    printf("\nContext switches: %d   Total time: %d   Idle: %d   Switch overhead: %d   CPU utilisation: %.1f%%\n",
           rd->ctx_switches, rd->total_time, rd->idle_time, rd->cs_overhead,
           rd->total_time ? 100.0 * busy / rd->total_time : 0.0);
}

static void print_comparison(const ReportData *r, int cnt, const char *input, int cs_cost)
{
    printf("\nComparison for %s (context-switch cost = %d)\n", input, cs_cost);
    if (cs_cost > 0)
        printf("%-22s %12s %15s %13s %17s %9s\n", "Algorithm", "Avg waiting", "Avg turnaround",
               "Avg response", "Context switches", "Finish t");
    else
        printf("%-22s %12s %15s %13s %17s\n", "Algorithm", "Avg waiting", "Avg turnaround",
               "Avg response", "Context switches");
    for (int i = 0; i < cnt; i++) {
        printf("%-22s %12.2f %15.2f %13.2f %17d", r[i].algo, r[i].avg_wait, r[i].avg_tat,
               r[i].avg_resp, r[i].ctx_switches);
        if (cs_cost > 0)
            printf(" %9d", r[i].total_time);
        printf("\n");
    }
}

/* ========================================================================= */
/*  PART 5 - LOGGER PROCESS (fork + pipe + dup2)                             */
/* ========================================================================= */

static pid_t start_logger(int *write_fd)
{
    int fds[2];
    if (pipe(fds) < 0) {
        perror("pipe");
        return -1;
    }
    char path[PATH_MAX + 16];
    sibling_path("logger", path, sizeof path);

    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {                              /* ---- logger process ---- */
        close(fds[1]);                           /* unused: write end        */
        if (dup2(fds[0], STDIN_FILENO) < 0)      /* read end becomes stdin   */
            _exit(126);
        close(fds[0]);                           /* original fd not needed   */
        execl(path, "logger", LOG_FILE, (char *)NULL);
        perror("exec logger");
        _exit(127);
    }
    close(fds[0]);                               /* parent: unused read end  */
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);          /* report child must not inherit it */
    *write_fd = fds[1];
    return pid;
}

static void stop_logger(pid_t pid, int write_fd)
{
    if (pid <= 0)
        return;
    close(write_fd);                             /* logger sees EOF and exits */
    while (waitpid(pid, NULL, 0) == -1 && errno == EINTR) { }   /* no zombie */
}

/* ========================================================================= */
/*  PART 6 - SHARED-MEMORY REPORT (fork + exec + shm + semaphore)            */
/* ========================================================================= */

static int run_report(const ReportData *data, int quiet)
{
    static int counter = 0;
    char name[64];
    snprintf(name, sizeof name, "/mini_sched_%d_%d", (int)getpid(), counter++);

    shm_unlink(name);                            /* in case of a stale segment */
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) { perror("shm_open"); return -1; }
    if (ftruncate(fd, (off_t)sizeof(SharedReport)) < 0) {
        perror("ftruncate");
        close(fd);
        shm_unlink(name);
        return -1;
    }
    SharedReport *sh = mmap(NULL, sizeof *sh, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (sh == MAP_FAILED) {
        perror("mmap");
        close(fd);
        shm_unlink(name);
        return -1;
    }
    close(fd);

    /* semaphore lives INSIDE the segment, pshared = 1, initial value 0 */
    if (sem_init(&sh->ready, 1, 0) != 0) {
        perror("sem_init");
        munmap(sh, sizeof *sh);
        shm_unlink(name);
        return -1;
    }

    char path[PATH_MAX + 16];
    sibling_path("report", path, sizeof path);

    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        sem_destroy(&sh->ready);
        munmap(sh, sizeof *sh);
        shm_unlink(name);
        return -1;
    }
    if (pid == 0) {                              /* ---- report process ---- */
        if (quiet)
            execl(path, "report", name, "quiet", (char *)NULL);
        else
            execl(path, "report", name, (char *)NULL);
        perror("exec report");
        _exit(127);
    }

    /* The report process is now blocked on sh->ready.  Write the results... */
    sh->data = *data;
    sem_post(&sh->ready);                        /* ...and only then release it */

    while (waitpid(pid, NULL, 0) == -1 && errno == EINTR) { }

    sem_destroy(&sh->ready);                     /* remove semaphore          */
    munmap(sh, sizeof *sh);
    shm_unlink(name);                            /* remove shared memory      */
    return 0;
}

/* ========================================================================= */
/*  main                                                                     */
/* ========================================================================= */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s <input_file> <algorithm> [quantum] [options]\n"
        "\n"
        "  algorithm : fcfs | sjf | srtf | rr | all | sweep\n"
        "  quantum   : time quantum for rr / all / (default %d)\n"
        "\n"
        "Options:\n"
        "  --cs-cost N     bonus: every context switch costs N time units (default 0)\n"
        "  --unit-ms N     real milliseconds per time unit (default %d)\n"
        "  --table-only    print only the comparison table (used for results.txt)\n"
        "  -h, --help      show this help\n"
        "\n"
        "'sweep' is the bonus experiment: Round Robin for quanta 1..10 with and\n"
        "without context-switch cost, to show how the averages change.\n"
        "\n"
        "Examples:\n"
        "  %s inputs/jobs1.txt fcfs\n"
        "  %s inputs/jobs1.txt rr 2\n"
        "  %s inputs/jobs1.txt all 2 --cs-cost 1\n"
        "  %s inputs/jobs1.txt sweep --cs-cost 1\n",
        prog, DEFAULT_QUANTUM, DEFAULT_UNIT_MS, prog, prog, prog, prog);
}

/* Bonus experiment: Round Robin for different quanta, with / without switch cost. */
static void run_sweep(const JobSpec *specs, int n, const char *input, int cs_cost)
{
    int maxburst = 1;
    for (int i = 0; i < n; i++)
        if (specs[i].burst > maxburst)
            maxburst = specs[i].burst;
    int qmax = maxburst < 10 ? maxburst : 10;
    int costs[2] = { 0, cs_cost > 0 ? cs_cost : 1 };

    printf("Bonus: Round Robin quantum sweep for %s\n", input);
    printf("(each run is a full thread-based simulation, executed without delays)\n");
    for (int ci = 0; ci < 2; ci++) {
        printf("\nContext-switch cost = %d time unit(s)\n", costs[ci]);
        printf("%-8s %12s %15s %13s %17s %12s %10s\n", "Quantum", "Avg waiting", "Avg turnaround",
               "Avg response", "Context switches", "Switch time", "Finish t");
        for (int q = 1; q <= qmax; q++) {
            ReportData rd;
            Seg *segs = NULL;
            int ns = 0;
            simulate(specs, n, ALGO_RR, q, costs[ci], 0, 0, -1, &rd, &segs, &ns);
            free(segs);
            printf("%-8d %12.2f %15.2f %13.2f %17d %12d %10d\n", q, rd.avg_wait, rd.avg_tat,
                   rd.avg_resp, rd.ctx_switches, rd.cs_overhead, rd.total_time);
        }
    }
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);            /* a dead logger must not kill us */

    const char *pos[4];
    int npos = 0, cs_cost = 0, unit_ms = DEFAULT_UNIT_MS, table_only = 0;

    for (int i = 1; i < argc; i++) {
        long v;
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--table-only") == 0) {
            table_only = 1;
        } else if (strcmp(argv[i], "--cs-cost") == 0 || strcmp(argv[i], "--unit-ms") == 0) {
            if (i + 1 >= argc || !parse_int(argv[i + 1], &v) || v < 0 || v > 100000) {
                fprintf(stderr, "Error: %s needs a non-negative whole number\n", argv[i]);
                return 1;
            }
            if (strcmp(argv[i], "--cs-cost") == 0) cs_cost = (int)v; else unit_ms = (int)v;
            i++;
        } else if (strncmp(argv[i], "--", 2) == 0) {
            fprintf(stderr, "Error: unknown option '%s'\n", argv[i]);
            usage(argv[0]);
            return 1;
        } else if (npos < 4) {
            pos[npos++] = argv[i];
        } else {
            usage(argv[0]);
            return 1;
        }
    }
    if (npos < 2 || npos > 3) {
        usage(argv[0]);
        return 1;
    }

    const char *input = pos[0];
    char alg[16];
    snprintf(alg, sizeof alg, "%s", pos[1]);
    for (char *p = alg; *p; p++)
        *p = (char)tolower((unsigned char)*p);

    int quantum = DEFAULT_QUANTUM;
    if (npos == 3) {
        long q;
        if (!parse_int(pos[2], &q) || q < 1 || q > MAX_VALUE) {
            fprintf(stderr, "Error: quantum must be a whole number >= 1 (got '%s')\n", pos[2]);
            return 1;
        }
        quantum = (int)q;
    }

    Algo list[4];
    int nalg = 0, sweep = 0;
    if      (strcmp(alg, "fcfs")  == 0) list[nalg++] = ALGO_FCFS;
    else if (strcmp(alg, "sjf")   == 0) list[nalg++] = ALGO_SJF;
    else if (strcmp(alg, "srtf")  == 0) list[nalg++] = ALGO_SRTF;
    else if (strcmp(alg, "rr")    == 0) list[nalg++] = ALGO_RR;
    else if (strcmp(alg, "sweep") == 0) sweep = 1;
    else if (strcmp(alg, "all")   == 0) {
        list[nalg++] = ALGO_FCFS;
        list[nalg++] = ALGO_SJF;
        list[nalg++] = ALGO_SRTF;
        list[nalg++] = ALGO_RR;
    } else {
        fprintf(stderr, "Error: unknown algorithm '%s' (use fcfs, sjf, srtf, rr, all)\n", pos[1]);
        return 1;
    }

    /* ---- Part 1 ---- */
    JobSpec specs[MAX_JOBS];
    int n = 0;
    if (load_jobs(input, specs, &n) != 0)
        return 1;

    if (sweep) {
        run_sweep(specs, n, input, cs_cost);
        return 0;
    }

    /* ---- Part 5: start the logger process ---- */
    int log_fd = -1;
    pid_t logger = start_logger(&log_fd);
    if (logger < 0)
        fprintf(stderr, "Warning: logger could not be started, continuing without a log.\n");

    ReportData results[4];
    int exit_code = 0;

    for (int a = 0; a < nalg; a++) {
        Algo algo = list[a];
        Seg *segs = NULL;
        int nsegs = 0;

        if (!table_only) {
            printf("\n==================== %s", ALGO_SHORT[algo]);
            if (algo == ALGO_RR)
                printf(" (quantum = %d)", quantum);
            if (cs_cost > 0)
                printf(" [context-switch cost = %d]", cs_cost);
            printf(" ====================\n");
            printf("Input: %s   (1 time unit = %d ms)\n\n", input, unit_ms);
        }

        simulate(specs, n, algo, quantum, cs_cost, unit_ms * 1000, !table_only,
                 logger > 0 ? log_fd : -1, &results[a], &segs, &nsegs);

        if (!table_only) {
            print_gantt(segs, nsegs, &results[a]);
            print_job_table(&results[a]);
        }
        free(segs);

        /* ---- Part 6: hand the metrics to the report process ---- */
        if (run_report(&results[a], table_only) != 0) {
            fprintf(stderr, "Warning: report process failed.\n");
            exit_code = 1;
        }
    }

    if (nalg > 1 || table_only)
        print_comparison(results, nalg, input, cs_cost);

    stop_logger(logger, log_fd);
    if (!table_only)
        printf("\nEvent log written to %s\n", LOG_FILE);
    return exit_code;
}
