==============================================================================
 MINI CPU SCHEDULER  -  Operating Systems Lab Project
 A live dispatcher built with threads, semaphores, pipes and shared memory
==============================================================================

1. GROUP MEMBERS AND WORK DIVISION
------------------------------------------------------------------------------
 

   Member 1 : Muhammad Bilal Azhar, Roll No 24I-2022   
   Member 2 : Ahmed Shehzad, Roll No 24I-2086
   Member 3 : Ayesha Irfan, Roll No 24I-2077

 The work is divided equally. Every part below was also reviewed by the other
 two members, and every member can explain the WHOLE system.

 MEMBER 1  - "Input and algorithms"
   * Part 1  Job table: load_jobs(), parse_int(), id_cmp(); validation of
             missing file, empty file, negative numbers, duplicate IDs,
             bad formats (src/scheduler.c, section "PART 1")
   * Part 2  Scheduling algorithms: ready queue, rq_pick(), better(),
             slice_for(); FCFS / SJF / SRTF / Round Robin and all tie-break
             conventions (src/scheduler.c, section "PART 2")
   * Test inputs inputs/jobs1..4.txt and inputs/bad_*.txt
   * Makefile (all targets) and results.txt

 MEMBER 2  - "Live execution (core part)"
   * Part 3  Job threads, arrival thread, dispatcher, mutex, semaphores:
             job_thread(), arrival_thread(), dispatcher(), tick_clock(),
             simulate() (src/scheduler.c, section "PART 3")
   * Idle handling and context-switch counting
   * BONUS   Context-switch cost inside the dispatcher (--cs-cost)
   * The "synchronisation tools" section (section 5) of this README

 MEMBER 3  - "Output, processes and IPC"
   * Part 4  Output and metrics: print_gantt(), print_job_table(),
             print_comparison(), 'all' mode
   * Part 5  Logger process: fork + pipe + dup2 + exec (start_logger(),
             stop_logger(), src/logger.c)
   * Part 6  Shared-memory report: shm_open + mmap + process-shared semaphore
             + fork/exec (run_report(), src/report.c, src/common.h)
   * BONUS   Quantum sweep experiment ('sweep' mode)
   * Sections 2, 3, 4 and 6 of this README

 All members: code review, testing on the lab machine, preparing for the
 individual viva (each member explains every part of the project).


2. WHAT IS IN THE ZIP  
------------------------------------------------------------------------------
 Project_i242022_i242086_i242077/
 |
 |-- Makefile            Builds everything with ONE command: make
 |                       Extra targets: run, bonus, results, test, clean
 |-- README.txt          This file
 |-- results.txt         Comparison table (all mode) for every input file,
 |                       plus the bonus tables (context-switch cost, sweep)
 |
 |-- src/
 |    |-- scheduler.c    MAIN PROGRAM. Contains parts 1-4, the code that
 |    |                  starts the logger (part 5) and the report (part 6),
 |    |                  and the bonus. It is the scheduler PROCESS, with the
 |    |                  dispatcher, the arrival thread and one thread per job.
 |    |-- logger.c       LOGGER PROCESS (part 5). Reads events from its
 |    |                  standard input (= the pipe) and appends them to
 |    |                  schedule.log.
 |    |-- report.c       REPORT PROCESS (part 6). Started by fork() + exec().
 |    |                  Attaches to the shared memory, waits on a semaphore,
 |    |                  then prints the final report.
 |    `-- common.h       Structures shared by scheduler.c and report.c: the
 |                       layout of the shared-memory segment (SharedReport).
 |
 `-- inputs/
      |-- jobs1.txt      Example from the brief (P1 0 5 / P2 1 3 / P3 2 8)
      |-- jobs2.txt      ALL JOBS ARRIVE AT TIME 0           (required test 1)
      |-- jobs3.txt      IDLE GAPS (t=5..12 and t=18..25)     (required test 2)
      |-- jobs4.txt      12 unsorted jobs, algorithms give VERY DIFFERENT
      |                  results; SRTF starves the long job   (required test 3)
      `-- bad_*.txt      Invalid inputs used to test Part 1 (negative numbers,
                         duplicate id, empty file, wrong formats)

 Files created when you build / run (not part of the zip):
      scheduler, logger, report   the three executables (in the project folder)
      schedule.log                event log written by the logger process


3. HOW TO COMPILE AND RUN  (terminal commands)
------------------------------------------------------------------------------
 Requirements: Linux, gcc, make (a standard lab machine is enough).

   

   a) Build everything with ONE command
        make
      This runs:  gcc -std=c11 -Wall -Wextra -pthread -O2 ...
      for scheduler.c, logger.c and report.c. There are no warnings.

   c) Run
        ./scheduler <input_file> <algorithm> [quantum]

        algorithm : fcfs | sjf | srtf | rr | all

      Examples:
        ./scheduler inputs/jobs1.txt fcfs
        ./scheduler inputs/jobs1.txt sjf
        ./scheduler inputs/jobs1.txt srtf
        ./scheduler inputs/jobs1.txt rr 2          # Round Robin, quantum 2
        ./scheduler inputs/jobs1.txt all 2         # every algorithm + comparison
        ./scheduler inputs/jobs3.txt all 2         # idle-gap example
        ./scheduler inputs/jobs4.txt all 3         # very different results

      If the quantum is omitted for rr / all, the default is 2.

   d) Options (all optional, can be placed anywhere)
        --cs-cost N     BONUS: every context switch costs N time units
        --unit-ms N     real milliseconds for one time unit (default 100)
                        Use a small value (e.g. 10) for long inputs.
        --table-only    print only the comparison table (used by 'make results')
        -h, --help      usage text

        ./scheduler inputs/jobs1.txt all 2 --cs-cost 1
        ./scheduler inputs/jobs4.txt all 3 --unit-ms 10
        ./scheduler inputs/jobs1.txt sweep --cs-cost 1     # bonus experiment

   e) Make shortcuts
        make            build everything
        make run        demo: Round Robin, quantum 2, jobs1.txt
        make results    re-create results.txt (runs every input in 'all' mode)
        make bonus      run the context-switch-cost bonus demos
        make test       feed the bad_*.txt files to check the input validation
        make clean      delete executables and schedule.log

   f) Where is the output?
        console        live execution lines, Gantt chart, per-job table,
                       FINAL REPORT (printed by the report process)
        schedule.log   events: arrived / started / resumed / preempted /
                       finished (written by the logger process; appended on
                       every run)


4. HOW THE PROGRAM WORKS
------------------------------------------------------------------------------
 Processes
   scheduler (main process)   threads: dispatcher (main thread), arrival
                              thread, one job thread per job
   logger                     created with fork() + pipe() + dup2() + exec()
   report                     created with fork() + exec(), data via shared mem

 Time. One time unit is a fixed real delay, the constant DEFAULT_UNIT_MS
 (100 ms) in scheduler.c. nanosleep() is used ONLY to simulate that unit of
 work (tick_clock()). The simulated clock `now` is advanced by the thread that
 currently owns the CPU, so the output is deterministic.

 Dispatcher <-> job hand-off (Figure 2 of the brief)
   1. dispatcher picks a job from the ready queue (algorithm dependent)
   2. sem_post(run_sem of that job)         -> job thread wakes up
   3. job thread runs `slice` units, printing one line per unit
   4. sem_post(done)                        -> slice over
   5. dispatcher: sem_wait(done) returns, updates Gantt chart, remaining time,
      completion, requeues the job if unfinished, counts context switches.
 A job thread sleeps on its OWN semaphore, so only the job the dispatcher
 chose can run. Slice length: FCFS/SJF = whole remaining burst (no
 preemption); SRTF = 1 unit (re-decide every unit); RR = min(quantum, remaining).

 Arrival thread. After EVERY time unit the clock owner posts arr_go; the
 arrival thread wakes, puts all jobs with arrival <= now into the ready queue,
 and posts arr_done. Therefore a job that arrives at the same moment a quantum
 expires is already in the queue BEFORE the preempted job is re-queued (rule
 of the brief). Once all jobs are released the thread ends.

 Part 5 - logger. scheduler: pipe(); fork(). Child: close(write end);
 dup2(read end, STDIN_FILENO); close(read end); execl("logger"). Parent: close
 the read end, write events into the write end (FD_CLOEXEC so the report child
 does not inherit it). At the end the parent closes the write end (logger sees
 EOF and exits) and waitpid()s for it - no zombie.

 Part 6 - shared-memory report. POSIX shared memory: shm_open(O_CREAT|O_EXCL),
 ftruncate, mmap. A process-shared semaphore (sem_init pshared=1, value 0)
 lives inside the segment. The scheduler fork()+exec()s `report`, which attaches
 and blocks in sem_wait(). The scheduler then writes the metrics (per job and
 averages) and calls sem_post(); only then does the report print. Finally the
 scheduler waitpid()s, sem_destroy()s, munmap()s and shm_unlink()s.
 Choice: POSIX shared memory (shm_open + mmap), NOT System V.

 Conventions followed (from the brief)
   * Ties: earlier arrival, then smaller job id (ids compared "naturally",
     so P2 < P10).
   * Round Robin: a job that arrives when another job's quantum expires enters
     the queue before the preempted job.
   * SRTF: re-evaluated at every time-unit boundary; on equal remaining time
     the running job continues.
   * Idle CPU shown as "CPU IDLE" and as IDLE in the Gantt chart.
   * Context switch counted each time the CPU goes from one job to a DIFFERENT
     job. First dispatch and switches to/from IDLE are not counted.
   * turnaround = completion - arrival; waiting = turnaround - burst;
     response = first CPU allocation - arrival.


5. SYNCHRONISATION TOOLS - WHAT PROTECTS WHAT
------------------------------------------------------------------------------
 MUTEX  ctx.mutex  (pthread_mutex_t, one per simulation run)
   Protects every variable that more than one thread can touch:
     * now                      the simulated clock
     * rq[] and rq_len          the READY QUEUE
     * released                 number of jobs released by the arrival thread
     * jobs[i].remaining / first_start / completion / slice
     * segs[], nsegs            the Gantt-chart log
     * ctx_switches, idle_time, cs_overhead   counters
     * console output and the pipe writes to the logger (so lines of
       different threads never get mixed)

 SEMAPHORES (all unnamed POSIX semaphores)

   Name              Initial   Posted by            Waited on by       Purpose
   ----------------  -------   -------------------  -----------------  ---------------------
   jobs[i].run_sem      0      dispatcher           job thread i       "you may run now"
                                                                       (one per job)
   done                 0      job thread           dispatcher         "my slice is over"
   arr_go               0      clock owner (job     arrival thread     "the clock moved,
                               thread / dispatcher)                    release arrivals"
   arr_done             0      arrival thread       clock owner        "arrivals are queued"
   sh->ready            0      scheduler            report process     "shared memory is
   (in shared memory,                                                  complete, you may read"
    pshared = 1)

 Protocol notes
   * At any moment exactly ONE thread of the scheduler owns the CPU (the job
     thread that was dispatched, or the dispatcher while idle / switching);
     the other threads are blocked on semaphores. Nothing busy-waits.
   * sleep/nanosleep is used only to simulate work (one time unit).
   * Shutdown: the dispatcher sets slice = -1 and posts every run_sem; the job
     threads exit, then all threads are joined (pthread_join).
   * Clean-up: sem_destroy, pthread_mutex_destroy, free, close of the pipe,
     waitpid of both child processes, munmap + shm_unlink of the segment.


6. BONUS - CONTEXT-SWITCH COST
------------------------------------------------------------------------------
 With --cs-cost N every context switch makes the CPU busy for N time units in
 which NO job makes progress (shown as "CS" in the Gantt chart, counted in the
 "Switch overhead" and in the finish time). Jobs that arrive meanwhile are queued
 normally. The job to run next is chosen BEFORE the overhead starts.
   ./scheduler inputs/jobs1.txt all 2 --cs-cost 1
 'sweep' mode shows how the averages change for different quanta with cost 0
 and with the given cost (default 1):
   ./scheduler inputs/jobs4.txt sweep --cs-cost 1
 Result (see results.txt): with cost 0 a SMALL quantum gives the best average
 waiting time on jobs4.txt (q=1: 14.42), but with cost 1 it becomes the worst
 (q=1: 37.17) because 39 switches waste 39 time units; the best quantum moves
 to q=5 (23.33). A bigger quantum means fewer switches but worse response time.


7. ASSUMPTIONS AND KNOWN LIMITATIONS
------------------------------------------------------------------------------
 * Burst times must be at least 1; arrival and burst times are limited to
   100000; at most 64 jobs; job ids at most 15 characters.
 * Quantum must be a whole number >= 1 (default 2 when omitted).
 * Everything is simulated in whole time units; a job that arrives while
   another one is being dispatched is queued at the next unit boundary (its
   arrival time and the printed "arrived" line still use the exact arrival
   time because arrivals can only happen at unit boundaries).
 * With a context-switch cost the next job is chosen before the overhead
   starts; a shorter job that arrives during the overhead does not change this
   decision (SRTF re-checks again after the next unit).
 * Idle time is simulated unit by unit (it takes real time like any other
   unit); use --unit-ms to speed up long idle gaps.
 * The executables logger and report are looked up in the same folder as the
   scheduler executable; keep the three files together (make does that).
 * schedule.log is appended on every run; delete it (or make clean) for a
   fresh log.
 * Needs Linux (POSIX shared memory, /proc/self/exe). Tested with gcc 13 and
   with ThreadSanitizer / AddressSanitizer (no data races, no leaks).
