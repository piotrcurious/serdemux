/*
 * serdemux.c
 *
 * Serial (de)multiplexer:
 *  - Serial -> pipes: read lines from serial; first binary byte = stream id; rest (including newline) is written to streams/<id>.
 *  - Pipes -> serial: a reader thread per streams/<id> blocks for writers; when it reads data it prefixes stream id byte and writes to serial.
 *
 * Build:
 *   gcc -std=c11 -O2 -pthread -o serdemux serdemux.c
 *
 * Usage:
 *   ./serdemux -d /dev/ttyUSB0 -b 115200 -n 16
 *
 * Default: device = /dev/ttyUSB0, baud = 115200, streams = 16
 *
 * Notes:
 *  - Lines from serial must be newline-terminated to be forwarded.
 *  - Writes to streams/<id> are forwarded as-is (whatever bytes you write); they will be sent to the serial device prefixed by the single binary stream byte.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <poll.h>
#include <signal.h>
#include <pthread.h>

#define DEFAULT_DEVICE "/dev/ttyUSB0"
#define DEFAULT_BAUD 115200
#define DEFAULT_STREAMS 16
#define STREAM_DIR "streams"
#define MAX_LINE 16384
#define READ_CHUNK 4096

static volatile sig_atomic_t running = 1;
static int serial_fd = -1;
static pthread_mutex_t serial_write_mutex = PTHREAD_MUTEX_INITIALIZER;

struct stream_info {
    unsigned id;
    char path[256];
    pthread_t thread;
    bool thread_started;
};

static struct stream_info *streams = NULL;
static unsigned num_streams = 0;

static void sigint_handler(int signo) {
    (void)signo;
    running = 0;
    // We'll exit quickly; main loop will check running.
}

static speed_t baud_to_constant(int baud) {
    switch (baud) {
    case 50: return B50;
    case 75: return B75;
    case 110: return B110;
    case 134: return B134;
    case 150: return B150;
    case 200: return B200;
    case 300: return B300;
    case 600: return B600;
    case 1200: return B1200;
    case 1800: return B1800;
    case 2400: return B2400;
    case 4800: return B4800;
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
#ifdef B460800
    case 460800: return B460800;
#endif
#ifdef B921600
    case 921600: return B921600;
#endif
    default: return (speed_t) -1;
    }
}

static int configure_serial(int fd, int baud) {
    struct termios tio;
    if (tcgetattr(fd, &tio) != 0) {
        perror("tcgetattr");
        return -1;
    }

    cfmakeraw(&tio); // raw mode: disables canonical processing, echo, signals, etc.

    speed_t b = baud_to_constant(baud);
    if (b == (speed_t)-1) {
        fprintf(stderr, "Unsupported baud: %d\n", baud);
        return -1;
    }
    cfsetispeed(&tio, b);
    cfsetospeed(&tio, b);

    // 8N1 by default since cfmakeraw implies 8-bit and no parity
    tio.c_cflag &= ~CSTOPB;
    tio.c_cflag &= ~PARENB;
    tio.c_cflag &= ~CSIZE;
    tio.c_cflag |= CS8;

    tio.c_cc[VMIN] = 1;  // read returns as soon as 1 byte available
    tio.c_cc[VTIME] = 0; // no inter-byte timeout

    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        perror("tcsetattr");
        return -1;
    }

    return 0;
}

/* Thread that opens streams[id] FIFO for reading (blocking) and forwards any read data
 * to the serial device, prefixing it with the stream id byte.
 */
static void *stream_reader_thread(void *arg) {
    struct stream_info *s = (struct stream_info *)arg;

    while (running) {
        int fd = open(s->path, O_RDONLY); // blocking open: will wait until a writer opens pipe
        if (fd < 0) {
            if (!running) break;
            fprintf(stderr, "stream %u: open(%s) O_RDONLY failed: %s\n", s->id, s->path, strerror(errno));
            // small sleep to avoid busy loop on repeated errors
            sleep(1);
            continue;
        }
        // Read loop: read whatever writers send; when writer closes -> read returns 0 -> close fd and reopen
        uint8_t buf[READ_CHUNK];
        while (running) {
            ssize_t r = read(fd, buf, sizeof(buf));
            if (r > 0) {
                // Build message: first byte is stream id, then the payload
                uint8_t txbuf[READ_CHUNK + 1];
                txbuf[0] = (uint8_t)(s->id & 0xFF);
                memcpy(&txbuf[1], buf, (size_t)r);
                size_t to_write = (size_t)r + 1;

                // Serial writes protected by mutex
                pthread_mutex_lock(&serial_write_mutex);
                ssize_t written = 0;
                ssize_t w;
                while (written < (ssize_t)to_write) {
                    w = write(serial_fd, txbuf + written, to_write - written);
                    if (w < 0) {
                        if (errno == EINTR) continue;
                        fprintf(stderr, "stream %u: write to serial failed: %s\n", s->id, strerror(errno));
                        break;
                    }
                    written += w;
                }
                pthread_mutex_unlock(&serial_write_mutex);
            } else if (r == 0) {
                // Writer closed the pipe. Close and break to reopen.
                close(fd);
                break;
            } else {
                if (errno == EINTR) continue;
                fprintf(stderr, "stream %u: read error: %s\n", s->id, strerror(errno));
                close(fd);
                break;
            }
        }
        // small sleep before re-open to be gentle in case of rapid cycles
        usleep(10000);
    }

    return NULL;
}

static int ensure_streams_dir(void) {
    struct stat st;
    if (stat(STREAM_DIR, &st) != 0) {
        if (mkdir(STREAM_DIR, 0775) != 0) {
            perror("mkdir streams/");
            return -1;
        }
    } else {
        if (!S_ISDIR(st.st_mode)) {
            fprintf(stderr, "%s exists and is not a directory\n", STREAM_DIR);
            return -1;
        }
    }
    return 0;
}

static int mkfifo_for_stream(unsigned id) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%u", STREAM_DIR, id);
    // If file exists and is FIFO, ok. If exists and not FIFO, error.
    struct stat st;
    if (stat(path, &st) == 0) {
        if (!S_ISFIFO(st.st_mode)) {
            fprintf(stderr, "file %s exists and is not a FIFO\n", path);
            return -1;
        }
        return 0;
    }
    if (mkfifo(path, 0666) != 0) {
        if (errno == EEXIST) return 0;
        perror("mkfifo");
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *device = DEFAULT_DEVICE;
    int baud = DEFAULT_BAUD;
    num_streams = DEFAULT_STREAMS;

    int opt;
    while ((opt = getopt(argc, argv, "d:b:n:h")) != -1) {
        switch (opt) {
        case 'd':
            device = optarg;
            break;
        case 'b':
            baud = atoi(optarg);
            break;
        case 'n':
            num_streams = (unsigned)atoi(optarg);
            if (num_streams == 0) num_streams = 1;
            if (num_streams > 256) num_streams = 256;
            break;
        case 'h':
        default:
            fprintf(stderr, "Usage: %s [-d serial_device] [-b baud] [-n num_streams]\n", argv[0]);
            return 1;
        }
    }

    if (ensure_streams_dir() != 0) return 1;

    // allocate stream_info structures
    streams = calloc(num_streams, sizeof(*streams));
    if (!streams) {
        perror("calloc");
        return 1;
    }

    for (unsigned i = 0; i < num_streams; ++i) {
        streams[i].id = i;
        snprintf(streams[i].path, sizeof(streams[i].path), "%s/%u", STREAM_DIR, i);
        streams[i].thread_started = false;
        // create FIFO file
        if (mkfifo_for_stream(i) != 0) {
            fprintf(stderr, "Failed to create FIFO for stream %u\n", i);
            // continue so other streams may work
        }
    }

    // Open and configure serial port
    serial_fd = open(device, O_RDWR | O_NOCTTY);
    if (serial_fd < 0) {
        perror("open serial device");
        fprintf(stderr, "Tried device: %s\n", device);
        return 1;
    }

    if (configure_serial(serial_fd, baud) != 0) {
        close(serial_fd);
        return 1;
    }

    // Make serial_fd blocking for writes/reads (we use blocking reads)
    int flags = fcntl(serial_fd, F_GETFL, 0);
    if (flags >= 0) {
        flags &= ~O_NONBLOCK;
        fcntl(serial_fd, F_SETFL, flags);
    }

    // Start reader threads for each stream
    for (unsigned i = 0; i < num_streams; ++i) {
        if (pthread_create(&streams[i].thread, NULL, stream_reader_thread, &streams[i]) != 0) {
            fprintf(stderr, "Failed to create thread for stream %u: %s\n", i, strerror(errno));
            streams[i].thread_started = false;
        } else {
            streams[i].thread_started = true;
        }
    }

    // Setup signal handler
    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // Main loop: read from serial and forward to corresponding FIFO (strip first byte)
    uint8_t readbuf[READ_CHUNK];
    uint8_t linebuf[MAX_LINE];
    size_t linepos = 0;

    fprintf(stderr, "serdemux started. device=%s baud=%d streams=%u\n", device, baud, num_streams);
    fprintf(stderr, "Pipes located under ./%s/<stream_id>\n", STREAM_DIR);

    while (running) {
        ssize_t r = read(serial_fd, readbuf, sizeof(readbuf));
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("read(serial)");
            break;
        } else if (r == 0) {
            // EOF on serial? unlikely for real serial devices; treat as termination
            fprintf(stderr, "Serial device EOF. Exiting.\n");
            break;
        } else {
            // append to line buffer and process complete lines (newline-delimited)
            size_t idx = 0;
            while (idx < (size_t)r) {
                uint8_t c = readbuf[idx++];
                if (linepos < MAX_LINE) {
                    linebuf[linepos++] = c;
                } else {
                    // line too long, reset
                    fprintf(stderr, "Line exceeded MAX_LINE, discarding\n");
                    linepos = 0;
                    continue;
                }
                if (c == '\n') {
                    // complete line ready
                    if (linepos >= 1) {
                        unsigned stream_id = (unsigned) ((uint8_t)linebuf[0]);
                        size_t payload_len = (linepos >= 2) ? (linepos - 1) : 0;
                        if (stream_id < num_streams && payload_len > 0) {
                            char path[256];
                            snprintf(path, sizeof(path), "%s/%u", STREAM_DIR, stream_id);
                            // Open FIFO for writing non-blocking (so we don't block if no reader)
                            int wfd = open(path, O_WRONLY | O_NONBLOCK);
                            if (wfd >= 0) {
                                ssize_t wrote = 0;
                                ssize_t w;
                                const uint8_t *payload = &linebuf[1];
                                size_t remaining = payload_len;
                                while (remaining > 0) {
                                    w = write(wfd, payload + wrote, remaining);
                                    if (w < 0) {
                                        if (errno == EINTR) continue;
                                        if (errno == EPIPE) {
                                            // No readers; drop
                                            break;
                                        }
                                        fprintf(stderr, "write to pipe %s failed: %s\n", path, strerror(errno));
                                        break;
                                    }
                                    wrote += w;
                                    remaining -= (size_t)w;
                                }
                                close(wfd);
                            } else {
                                // If open fails with ENXIO, there is no reader; drop payload
                                if (errno != ENXIO && errno != ENOENT) {
                                    fprintf(stderr, "open %s for writing failed: %s\n", path, strerror(errno));
                                }
                                // silently drop if no reader
                            }
                        } else {
                            // No payload or out-of-range stream id: ignore, but log optionally
                            if (payload_len == 0) {
                                // nothing to forward
                            } else {
                                fprintf(stderr, "Received line for out-of-range stream %u (max %u)\n", stream_id, num_streams - 1);
                            }
                        }
                    }
                    // reset line buffer
                    linepos = 0;
                }
            }
        }
    }

    // Cleanup/exit
    fprintf(stderr, "Shutting down...\n");

    // Attempt to cancel reader threads (best-effort); they may be blocked in open/read.
    for (unsigned i = 0; i < num_streams; ++i) {
        if (streams[i].thread_started) {
            // try to cancel; if thread is blocked in open/read it may not respond.
            pthread_cancel(streams[i].thread);
        }
    }
    // join threads quickly
    for (unsigned i = 0; i < num_streams; ++i) {
        if (streams[i].thread_started) {
            pthread_join(streams[i].thread, NULL);
        }
    }

    if (serial_fd >= 0) close(serial_fd);
    free(streams);

    fprintf(stderr, "Exit.\n");
    return 0;
}
