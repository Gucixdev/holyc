#include "test_common.h"
#define SINGLE_INSTANCE_IMPLEMENTATION
#include "single_instance.h"

#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <time.h>

#define RACE_CHILD_COUNT 20

int main(void)
{
    print_cstr("test_single_instance\n");

    /* basic single-process acquire/release */
    {
        Single_Instance_Lock lock;
        Single_Instance_Result r = single_instance_acquire(&lock, "holyc_test_basic", 0);
        TEST_CHECK(r == SINGLE_INSTANCE_ACQUIRED, "the first acquire of a fresh lock name succeeds");
        single_instance_release(&lock);

        r = single_instance_acquire(&lock, "holyc_test_basic", 0);
        TEST_CHECK(r == SINGLE_INSTANCE_ACQUIRED, "after releasing, the SAME name can be acquired again (release really lets go)");
        single_instance_release(&lock);
    }

    /* a second acquire attempt, while the first is still held, is
     * correctly refused */
    {
        Single_Instance_Lock lock_a, lock_b;
        Single_Instance_Result ra, rb;
        ra = single_instance_acquire(&lock_a, "holyc_test_busy", 0);
        rb = single_instance_acquire(&lock_b, "holyc_test_busy", 0);
        TEST_CHECK(ra == SINGLE_INSTANCE_ACQUIRED, "the first holder acquires cleanly");
        TEST_CHECK(rb == SINGLE_INSTANCE_ALREADY_RUNNING, "a second attempt on the SAME name, while the first still holds it, is refused - not a silent double-acquire");
        single_instance_release(&lock_a);
    }

    /* the real atomicity claim: RACE_CHILD_COUNT real, concurrently-
     * forked processes all attempt to acquire the SAME lock name at
     * once - exactly ONE must win, not "usually one" or "one on this
     * particular run" - flock's own kernel-level atomicity is what's
     * actually under test here, not this file's own C logic (a naive
     * "check if a PID file exists, then create one" scheme would be a
     * real TOCTOU race and could let more than one child "win" under
     * real concurrent load - this proves that doesn't happen). */
    {
        pid_t pids[RACE_CHILD_COUNT];
        i32 i;
        i32 acquired_count = 0;
        i32 busy_count = 0;
        char outcome_path[RACE_CHILD_COUNT][64];

        for (i = 0; i < RACE_CHILD_COUNT; i++) {
            sprintf(outcome_path[i], "/tmp/holyc_race_outcome_%d", i);
        }

        for (i = 0; i < RACE_CHILD_COUNT; i++) {
            pid_t pid = fork();
            if (pid == 0) {
                /* child: race for the lock, record the outcome, exit */
                Single_Instance_Lock lock;
                Single_Instance_Result r = single_instance_acquire(&lock, "holyc_test_race", 0);
                i32 fd = open(outcome_path[i], O_WRONLY | O_CREAT | O_TRUNC, 0644);
                char c = (r == SINGLE_INSTANCE_ACQUIRED) ? 'A' : 'B';
                if (fd >= 0) { if (write(fd, &c, 1) < 0) { /* best-effort */ } close(fd); }
                if (r == SINGLE_INSTANCE_ACQUIRED) {
                    struct timespec ts;
                    ts.tv_sec = 0;
                    ts.tv_nsec = 100000000L; /* hold it briefly so every sibling has genuinely raced against a real held lock, not an instantly-released one */
                    nanosleep(&ts, NULL);
                    single_instance_release(&lock);
                }
                _exit(0);
            }
            pids[i] = pid;
        }

        for (i = 0; i < RACE_CHILD_COUNT; i++) {
            i32 status;
            waitpid(pids[i], &status, 0);
        }

        for (i = 0; i < RACE_CHILD_COUNT; i++) {
            i32 fd = open(outcome_path[i], O_RDONLY);
            char c = 'B';
            if (fd >= 0) { if (read(fd, &c, 1) < 0) { /* best-effort */ } close(fd); }
            unlink(outcome_path[i]);
            if (c == 'A') acquired_count++; else busy_count++;
        }

        TEST_CHECK(acquired_count == 1, "of 20 real, concurrently-forked processes racing for the same lock, EXACTLY one acquired it");
        TEST_CHECK(busy_count == RACE_CHILD_COUNT - 1, "every other real process correctly saw ALREADY_RUNNING, not a partial/inconsistent result");
    }

    /* -force: kills the current real holder and takes over */
    {
        pid_t holder_pid = fork();
        if (holder_pid == 0) {
            Single_Instance_Lock lock;
            single_instance_acquire(&lock, "holyc_test_force", 0);
            for (;;) pause(); /* holds the lock until SIGTERM'd by the force path below */
        }

        {
            /* give the child a moment to actually acquire before we
             * try to force past it - a real, bounded wait, not a race
             * against the child's own startup */
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 100000000L;
            nanosleep(&ts, NULL);
        }

        {
            Single_Instance_Lock lock;
            Single_Instance_Result r = single_instance_acquire(&lock, "holyc_test_force", 1);
            i32 status;
            i32 holder_exited;

            TEST_CHECK(r == SINGLE_INSTANCE_ACQUIRED, "force=1 against a real, currently-held lock successfully takes it over");

            waitpid(holder_pid, &status, 0);
            holder_exited = WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM;
            TEST_CHECK(holder_exited, "the previous real holder process was actually SIGTERM'd, not just outraced");

            single_instance_release(&lock);
        }
    }

    print_flush();
    return test_failures_ != 0;
}
