#ifndef HOLYC_SINGLE_INSTANCE_H_
#define HOLYC_SINGLE_INSTANCE_H_

/* Atomic single-instance application lock: launch the same program
 * 50000 times concurrently and exactly ONE of them wins, every other
 * call returns "already running" immediately - the OS kernel itself
 * enforces the atomicity via flock(LOCK_EX | LOCK_NB) on a well-known
 * lock file, not a check-then-act race (checking "does a PID file
 * exist" first and creating one second is exactly the TOCTOU bug this
 * design avoids - two processes could both pass the check before
 * either creates the file; flock is atomic at the kernel/filesystem
 * level, there is no window for two callers to both "win").
 *
 * -force semantics: single_instance_acquire's own `force` argument
 * signals the ALREADY-RUNNING instance to exit (SIGTERM to the PID
 * recorded in the lock file) and then retries the lock a bounded
 * number of times, rather than looping forever or silently giving up -
 * a real, finite, documented wait, not an infinite spin. */

#include "core/base.h"

typedef struct {
    i32 fd;    /* the open, locked file descriptor - held for the process's entire lifetime, closing it (including on process exit/crash) releases the lock automatically, the kernel's own guarantee */
    char path[256];
} Single_Instance_Lock;

typedef enum {
    SINGLE_INSTANCE_ACQUIRED = 0,     /* this call is now the one true instance */
    SINGLE_INSTANCE_ALREADY_RUNNING,   /* another instance holds the lock, and force was 0 or the forced instance didn't exit in time */
    SINGLE_INSTANCE_ERROR                /* a real I/O error (path unwritable, etc) - not the same as "already running" */
} Single_Instance_Result;

/* `name` becomes the lock file's own basename (under /tmp) - callers
 * sharing a `name` compete for the SAME lock; different names are
 * entirely independent (two different programs, or the same program
 * run under two deliberately different identities, e.g. "myapp-dev"
 * vs "myapp"). If `force` is nonzero and another instance currently
 * holds the lock, its PID (recorded in the lock file at acquire time)
 * is sent SIGTERM, and this function waits up to ~1 second (in small
 * polling steps, not a long single sleep) for it to actually release
 * the lock before giving up and reporting SINGLE_INSTANCE_ALREADY_RUNNING -
 * a real, bounded wait, not an assumption that SIGTERM is instant. */
Single_Instance_Result single_instance_acquire(Single_Instance_Lock *out, const char *name, i32 force);

/* releases the lock (closes the fd) - also happens automatically if
 * the process exits or crashes without calling this at all (the
 * kernel releases flock locks when the holding fd closes, for any
 * reason), this is just the tidy explicit path. */
void single_instance_release(Single_Instance_Lock *lock);

#endif /* HOLYC_SINGLE_INSTANCE_H_ */

#ifdef SINGLE_INSTANCE_IMPLEMENTATION
#ifndef SINGLE_INSTANCE_IMPLEMENTATION_
#define SINGLE_INSTANCE_IMPLEMENTATION_

#include <fcntl.h>
#include <sys/file.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>

static size_t single_instance_strlen_(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static void single_instance_build_path_(char *out, size_t out_cap, const char *name)
{
    static const char prefix[] = "/tmp/holyc-single-instance-";
    size_t pi = 0, ni = 0, oi = 0;
    while (prefix[pi] && oi + 1 < out_cap) out[oi++] = prefix[pi++];
    while (name[ni] && oi + 1 < out_cap) out[oi++] = name[ni++];
    out[oi] = '\0';
}

static i32 single_instance_read_pid_(i32 fd)
{
    char buf[32];
    ssize_t n;
    i32 pid = 0;
    i32 i;
    lseek(fd, 0, SEEK_SET);
    n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) return 0;
    buf[n] = '\0';
    for (i = 0; buf[i] >= '0' && buf[i] <= '9'; i++) pid = pid * 10 + (buf[i] - '0');
    return pid;
}

static void single_instance_write_pid_(i32 fd)
{
    char buf[32];
    i32 pid = (i32)getpid();
    i32 i = 0;
    if (pid == 0) { buf[i++] = '0'; }
    else {
        char tmp[16];
        i32 n = 0;
        while (pid > 0) { tmp[n++] = (char)('0' + pid % 10); pid /= 10; }
        while (n > 0) buf[i++] = tmp[--n];
    }
    ftruncate(fd, 0);
    lseek(fd, 0, SEEK_SET);
    if (write(fd, buf, (size_t)i) < 0) { /* best-effort - a failed PID write just means a future -force can't find who to signal, not a lock-safety issue (flock itself is still exclusive regardless) */ }
}

Single_Instance_Result single_instance_acquire(Single_Instance_Lock *out, const char *name, i32 force)
{
    i32 fd;
    i32 attempt;

    single_instance_build_path_(out->path, sizeof(out->path), name);
    (void)single_instance_strlen_; /* kept for symmetry with this project's own other own-strlen_ helpers even though this file's own path-building loop doesn't need it directly */

    fd = open(out->path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) return SINGLE_INSTANCE_ERROR;

    if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
        single_instance_write_pid_(fd);
        out->fd = fd;
        return SINGLE_INSTANCE_ACQUIRED;
    }

    /* someone else holds it - flock's own LOCK_NB failure (EWOULDBLOCK)
     * is the atomic, race-free signal for this, not a separate
     * check-then-act probe */
    if (!force) {
        close(fd);
        return SINGLE_INSTANCE_ALREADY_RUNNING;
    }

    {
        i32 holder_pid = single_instance_read_pid_(fd);
        if (holder_pid > 0) kill((pid_t)holder_pid, SIGTERM);

        /* bounded polling wait (~1s total, 20 * 50ms) for the signaled
         * instance to actually exit and release its own flock - real
         * and finite, not assumed-instant or infinite */
        for (attempt = 0; attempt < 20; attempt++) {
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 50000000L; /* 50ms */
            nanosleep(&ts, NULL);
            if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
                single_instance_write_pid_(fd);
                out->fd = fd;
                return SINGLE_INSTANCE_ACQUIRED;
            }
        }
    }

    close(fd);
    return SINGLE_INSTANCE_ALREADY_RUNNING;
}

void single_instance_release(Single_Instance_Lock *lock)
{
    flock(lock->fd, LOCK_UN);
    close(lock->fd);
}

#endif /* SINGLE_INSTANCE_IMPLEMENTATION_ */
#endif /* SINGLE_INSTANCE_IMPLEMENTATION */
