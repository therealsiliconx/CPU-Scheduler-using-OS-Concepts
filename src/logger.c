/*
 * logger.c  -  Part 5: the logger process.
 *
 * The scheduler creates a pipe, fork()s, and in the child calls dup2() so
 * that the read end of the pipe becomes this program's standard input, then
 * exec()s this program.  We simply read events line by line from stdin and
 * append them to the log file (default: schedule.log).
 *
 * The program ends by itself when the scheduler closes the write end of the
 * pipe (fgets returns NULL at end-of-file).
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <time.h>

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "schedule.log";

    FILE *out = fopen(path, "a");              /* append mode */
    if (!out) {
        perror("logger: cannot open log file");
        return 1;
    }

    time_t now = time(NULL);
    struct tm tmv;
    char stamp[64];
    localtime_r(&now, &tmv);
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tmv);
    fprintf(out, "==== Mini CPU Scheduler log (started %s) ====\n", stamp);

    char line[512];
    while (fgets(line, sizeof line, stdin) != NULL) {   /* stdin == pipe */
        fputs(line, out);
        fflush(out);
    }

    fprintf(out, "==== end of log ====\n");
    fclose(out);
    return 0;
}
