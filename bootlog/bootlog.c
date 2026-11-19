// SPDX-License-Identifier: Apache-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#define MAX_LOG_BYTES (8 * 1024 * 1024)
#define MAX_SECONDS 180

static volatile sig_atomic_t stopping;

static void stop_capture(int signo) {
    (void)signo;
    stopping = 1;
}

static int write_all(int fd, const char *data, size_t size) {
    while (size) {
        ssize_t written = write(fd, data, size);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return -1;
        data += written;
        size -= written;
    }
    return 0;
}

static time_t uptime_seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec;
}

int main(int argc, char **argv) {
    // Optional paths allow testing capture and rotation on a host.
    if (argc > 3) return 2;
    const char *directory = argc > 1 ? argv[1] : "/metadata/nashc-bootlog";
    const char *source = argc > 2 ? argv[2] : "/dev/kmsg";
    int input = open(source, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (input < 0) {
        perror("nashc_bootlog: open input");
        return 1;
    }
    int dir = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dir < 0) {
        perror("nashc_bootlog: open directory");
        close(input);
        return 1;
    }
    // Only these two files belong to the logger. Never touch other metadata.
    if (renameat(dir, "current.log", dir, "previous.log") && errno != ENOENT) {
        perror("nashc_bootlog: rotate");
        close(dir);
        close(input);
        return 1;
    }
    int output = openat(dir, "current.log",
                        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (output < 0) {
        perror("nashc_bootlog: open output");
        close(dir);
        close(input);
        return 1;
    }
    struct utsname kernel;
    char header[1024];
    uname(&kernel);
    int length = snprintf(header, sizeof(header),
                          "nashc early boot capture: %s %s\n"
                          "Raw /dev/kmsg records: priority,sequence,microseconds,flags;message\n",
                          kernel.release, kernel.version);
    int result = write_all(output, header, (size_t)length);
    fsync(output);
    fsync(dir);
    close(dir);

    struct sigaction action = {.sa_handler = stop_capture};
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);

    const time_t deadline = uptime_seconds() + MAX_SECONDS;
    time_t last_sync = uptime_seconds();
    size_t total = (size_t)length;
    bool dirty = false;
    char buffer[65536];
    while (!result && !stopping && uptime_seconds() < deadline) {
        ssize_t count = read(input, buffer, sizeof(buffer));
        if (count > 0) {
            if ((size_t)count > MAX_LOG_BYTES - total) {
                const char limit[] = "\n[capture stopped: 8 MiB limit]\n";
                result = write_all(output, limit, sizeof(limit) - 1);
                break;
            }
            result = write_all(output, buffer, (size_t)count);
            total += (size_t)count;
            dirty = true;
        } else if (!count) {
            break;
        } else if (errno == EPIPE) {
            // /dev/kmsg overruns move the cursor forward; continue reading.
            const char lost[] = "\n[kernel ring buffer overrun: records lost]\n";
            result = write_all(output, lost, sizeof(lost) - 1);
            total += sizeof(lost) - 1;
            if (total >= MAX_LOG_BYTES) break;
            dirty = true;
        } else if (errno != EAGAIN && errno != EINTR) {
            perror("nashc_bootlog: read");
            result = -1;
        } else if (errno == EAGAIN) {
            struct pollfd pending = {.fd = input, .events = POLLIN};
            poll(&pending, 1, 250);
        }
        // Bound the write rate while retaining logs through a sudden reset.
        if (dirty && uptime_seconds() != last_sync) {
            if (fsync(output)) result = -1;
            last_sync = uptime_seconds();
            dirty = false;
        }
    }
    if (fsync(output)) result = -1;
    close(output);
    close(input);
    return result ? 1 : 0;
}
