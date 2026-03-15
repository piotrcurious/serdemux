/*
 * serdemux v2
 *
 * A robust serial (de)multiplexer for Linux.
 *
 * Multiplexes multiple local streams into a single serial device,
 * and demultiplexes serial data back to the streams based on a
 * 1-byte stream ID prefix.
 *
 * Architecture:
 * - Each stream has two named pipes (FIFOs) in the 'streams' directory:
 *   - streams/<id>.tx : Data written here is sent to the serial port.
 *   - streams/<id>.rx : Data received from the serial port is written here.
 * - For each stream, a dedicated thread reads from the .tx FIFO and
 *   forwards data to the serial port, prefixed with the stream ID.
 * - The main thread reads from the serial port, parses newline-terminated
 *   lines, and forwards the payload to the corresponding .rx FIFO.
 *
 * Build:
 *   gcc -std=c11 -O2 -pthread -o serdemux serdemux.c
 *
 * Usage:
 *   ./serdemux [-d device] [-b baud] [-n streams] [-l maxline]
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
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
#include <stdarg.h>

#define DEFAULT_DEVICE "/dev/ttyUSB0"
#define DEFAULT_BAUD 115200
#define DEFAULT_STREAMS 16
#define STREAM_DIR "streams"
#define READ_CHUNK 4096
#define MAX_LINE_DEFAULT 16384

static volatile sig_atomic_t g_running = 1;
static int g_serial_fd = -1;
static pthread_mutex_t g_serial_write_mutex = PTHREAD_MUTEX_INITIALIZER;

struct stream_info {
    unsigned id;
    char rx_path[256];
    char tx_path[256];
    pthread_t thread;
    bool thread_started;
    int rx_fifo_fd; // Persistent RDWR to avoid ENXIO/blocking
};

static struct stream_info *g_streams = NULL;
static unsigned g_num_streams = 0;

static void log_info(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[INFO] "); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n");
    va_end(ap);
}

static void log_error(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[ERROR] "); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n");
    va_end(ap);
}

static void sig_handler(int signo) { (void)signo; g_running = 0; }

static speed_t baud_to_constant(int baud) {
    switch (baud) {
    case 50: return B50; case 75: return B75; case 110: return B110;
    case 134: return B134; case 150: return B150; case 200: return B200;
    case 300: return B300; case 600: return B600; case 1200: return B1200;
    case 1800: return B1800; case 2400: return B2400; case 4800: return B4800;
    case 9600: return B9600; case 19200: return B19200; case 38400: return B38400;
    case 57600: return B57600; case 115200: return B115200; case 230400: return B230400;
#ifdef B460800
    case 460800: return B460800;
#endif
#ifdef B921600
    case 921600: return B921600;
#endif
    default: return (speed_t)-1;
    }
}

static int configure_serial(int fd, int baud) {
    struct termios tio;
    if (tcgetattr(fd, &tio) != 0) { perror("tcgetattr"); return -1; }
    cfmakeraw(&tio);
    speed_t b = baud_to_constant(baud);
    if (b != (speed_t)-1) { cfsetispeed(&tio, b); cfsetospeed(&tio, b); }
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cc[VMIN] = 1; tio.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &tio) != 0) { perror("tcsetattr"); return -1; }
    return 0;
}

static void *stream_reader_thread(void *arg) {
    struct stream_info *s = (struct stream_info *)arg;
    uint8_t buf[READ_CHUNK + 1];
    buf[0] = (uint8_t)(s->id & 0xFF);

    while (g_running) {
        // Open tx FIFO (process -> serial)
        // Blocking open waits for a writer
        int fd = open(s->tx_path, O_RDONLY);
        if (fd < 0) {
            if (g_running) usleep(100000);
            continue;
        }

        while (g_running) {
            ssize_t r = read(fd, buf + 1, READ_CHUNK);
            if (r > 0) {
                pthread_mutex_lock(&g_serial_write_mutex);
                size_t to_write = (size_t)r + 1;
                size_t written = 0;
                while (written < to_write && g_running) {
                    ssize_t w = write(g_serial_fd, buf + written, to_write - written);
                    if (w < 0) {
                        if (errno == EINTR) continue;
                        log_error("Serial write failed for stream %u: %s", s->id, strerror(errno));
                        break;
                    }
                    written += (size_t)w;
                }
                pthread_mutex_unlock(&g_serial_write_mutex);
            } else if (r == 0) {
                break; // Writer closed
            } else {
                if (errno != EINTR) {
                    log_error("Read from %s failed: %s", s->tx_path, strerror(errno));
                    break;
                }
            }
        }
        close(fd);
        usleep(10000);
    }
    return NULL;
}

int main(int argc, char **argv) {
    const char *device = DEFAULT_DEVICE;
    int baud = DEFAULT_BAUD;
    unsigned num_streams = DEFAULT_STREAMS;
    size_t max_line = MAX_LINE_DEFAULT;

    int opt;
    while ((opt = getopt(argc, argv, "d:b:n:l:h")) != -1) {
        switch (opt) {
        case 'd': device = optarg; break;
        case 'b': baud = atoi(optarg); break;
        case 'n': num_streams = (unsigned)atoi(optarg); break;
        case 'l': max_line = (size_t)atol(optarg); break;
        default: fprintf(stderr, "Usage: %s [-d dev] [-b baud] [-n streams] [-l maxline]\n", argv[0]); return 1;
        }
    }

    if (num_streams > 256) num_streams = 256;
    g_num_streams = num_streams;

    if (mkdir(STREAM_DIR, 0775) != 0 && errno != EEXIST) {
        perror("mkdir streams/");
        return 1;
    }

    g_streams = calloc(num_streams, sizeof(*g_streams));
    if (!g_streams) { perror("calloc"); return 1; }

    for (unsigned i = 0; i < num_streams; ++i) {
        g_streams[i].id = i;
        snprintf(g_streams[i].rx_path, sizeof(g_streams[i].rx_path), "%s/%u.rx", STREAM_DIR, i);
        snprintf(g_streams[i].tx_path, sizeof(g_streams[i].tx_path), "%s/%u.tx", STREAM_DIR, i);

        if (mkfifo(g_streams[i].rx_path, 0666) != 0 && errno != EEXIST) log_error("Failed to create %s", g_streams[i].rx_path);
        if (mkfifo(g_streams[i].tx_path, 0666) != 0 && errno != EEXIST) log_error("Failed to create %s", g_streams[i].tx_path);

        // Open rx FIFO for RDWR to keep it alive and avoid ENXIO in main loop
        g_streams[i].rx_fifo_fd = open(g_streams[i].rx_path, O_RDWR | O_NONBLOCK);
        if (g_streams[i].rx_fifo_fd < 0) log_error("Failed to open %s", g_streams[i].rx_path);
    }

    g_serial_fd = open(device, O_RDWR | O_NOCTTY);
    if (g_serial_fd < 0) {
        log_error("Failed to open serial device %s: %s", device, strerror(errno));
        return 1;
    }

    if (configure_serial(g_serial_fd, baud) != 0) {
        close(g_serial_fd);
        return 1;
    }

    for (unsigned i = 0; i < num_streams; ++i) {
        if (pthread_create(&g_streams[i].thread, NULL, stream_reader_thread, &g_streams[i]) == 0)
            g_streams[i].thread_started = true;
    }

    struct sigaction sa = {.sa_handler = sig_handler};
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    uint8_t *linebuf = malloc(max_line);
    if (!linebuf) { perror("malloc"); return 1; }
    size_t linepos = 0;
    uint8_t readbuf[READ_CHUNK];

    log_info("serdemux v2 started. device=%s baud=%d streams=%u", device, baud, num_streams);

    while (g_running) {
        struct pollfd pfd = { .fd = g_serial_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 100);
        if (pr <= 0) {
            if (pr < 0 && errno != EINTR) { log_error("poll failed: %s", strerror(errno)); break; }
            continue;
        }

        ssize_t r = read(g_serial_fd, readbuf, sizeof(readbuf));
        if (r <= 0) {
            if (r == 0) { log_info("Serial EOF"); break; }
            if (errno == EINTR || errno == EAGAIN) continue;
            log_error("Serial read failed: %s", strerror(errno));
            break;
        }

        for (ssize_t i = 0; i < r; ++i) {
            if (linepos < max_line) {
                linebuf[linepos++] = readbuf[i];
            } else {
                log_error("Line too long, discarding");
                linepos = 0;
                continue;
            }

            if (readbuf[i] == '\n') {
                if (linepos >= 2) {
                    unsigned stream_id = (uint8_t)linebuf[0];
                    if (stream_id < num_streams) {
                        int wfd = g_streams[stream_id].rx_fifo_fd;
                        if (wfd >= 0) {
                            if (write(wfd, &linebuf[1], linepos - 1) < 0) {
                                if (errno != EAGAIN) log_error("Write to stream %u failed: %s", stream_id, strerror(errno));
                            }
                        }
                    } else {
                        log_error("Invalid stream ID %u received", stream_id);
                    }
                }
                linepos = 0;
            }
        }
    }

    log_info("Shutting down...");
    for (unsigned i = 0; i < num_streams; ++i) {
        if (g_streams[i].thread_started) {
            pthread_cancel(g_streams[i].thread);
            pthread_join(g_streams[i].thread, NULL);
        }
        if (g_streams[i].rx_fifo_fd >= 0) close(g_streams[i].rx_fifo_fd);
    }
    close(g_serial_fd);
    free(g_streams);
    free(linebuf);
    log_info("Exit.");
    return 0;
}
