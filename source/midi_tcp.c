/*
 * midi_tcp — TCP MIDI bridge for Korg Kronos
 *
 * Listens on TCP port 9875 (same as Korg's MIDID).
 * Inbound:  TCP → /proc/.midi_in (kernel module injection)
 * Outbound: /proc/.midi_ring (upgraded) or shared memory (fallback) → TCP
 *
 * Usage: midi_tcp [-p port] [-d] [-s]
 *   -p port  TCP port (default 9875)
 *   -d       debug output
 *   -s       don't daemonize
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <pthread.h>

static int debug = 0;
static volatile int running = 1;

/* SysEx capture thread state */
#define SYSEX_CAP_SIZE 65536
static uint8_t sysex_capbuf[SYSEX_CAP_SIZE];
static volatile int sysex_caplen = 0;
static volatile int sysex_capturing = 0;
static volatile int sysex_done = 0;
static pthread_t capture_tid;
static pthread_mutex_t capture_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t capture_start_cond = PTHREAD_COND_INITIALIZER;

static void sighandler(int sig) { running = 0; }

/* ------------------------------------------------------------------ */
/*  MIDI output reader — two backends: /proc/.midi_ring or shm         */
/* ------------------------------------------------------------------ */

struct midi_out_reader {
    /* /proc/.midi_ring backend */
    int ring_fd;

    /* shared-memory fallback */
    uint8_t *container;
    uint8_t *ring_data;
    uint32_t *p_write;
    uint32_t mask;
    uint32_t our_cursor;
    int valid;
};

static int shm_fd = -1;

static void *lock_block(int id, int *sz) {
    unsigned int kva = ioctl(shm_fd, 0x64, id);
    int size = ioctl(shm_fd, 0x65, id);
    if (size <= 0) return NULL;
    if (sz) *sz = size;
    unsigned int po = kva & 0xFFF, pb = kva & ~0xFFF;
    void *m = mmap(NULL, po + size, PROT_READ, MAP_SHARED | 0x8000, shm_fd, pb);
    if (m == MAP_FAILED)
        m = mmap(NULL, po + size, PROT_READ, MAP_SHARED, shm_fd, pb);
    if (m == MAP_FAILED) return NULL;
    return (uint8_t *)m + po;
}

static int setup_midi_out(struct midi_out_reader *r) {
    memset(r, 0, sizeof(*r));
    r->ring_fd = -1;

    /* Try upgraded /proc/.midi_ring first */
    r->ring_fd = open("/proc/.midi_ring", O_RDWR);
    if (r->ring_fd >= 0) {
        r->valid = 1;
        if (debug)
            fprintf(stderr, "MIDI Out: using /proc/.midi_ring (upgraded)\n");
        return 0;
    }

    /* Fallback: shared memory */
    int sz3;
    shm_fd = open("/proc/.shm", O_RDONLY);
    if (shm_fd < 0) { perror("open /proc/.shm"); return -1; }

    r->container = lock_block(3, &sz3);
    if (!r->container || sz3 < 0x210) {
        fprintf(stderr, "can't map container\n");
        return -1;
    }

    int entry = 0x14C;
    int blk_id = *(uint32_t *)(r->container + entry + 0x44);
    r->mask = *(uint32_t *)(r->container + entry + 0x4C);
    r->p_write = (uint32_t *)(r->container + entry + 0x50);

    if (r->mask == 0) {
        fprintf(stderr, "MIDI Out queue mask=0, will retry on connect\n");
        return 0;
    }

    int blksz;
    r->ring_data = lock_block(blk_id, &blksz);
    if (!r->ring_data) {
        fprintf(stderr, "can't map block %d\n", blk_id);
        return -1;
    }

    r->our_cursor = *r->p_write;
    r->valid = 1;

    if (debug)
        fprintf(stderr, "MIDI Out: shm fallback, block=%d mask=0x%x cursor=%u\n",
                blk_id, r->mask, r->our_cursor);
    return 0;
}

static int read_midi_out(struct midi_out_reader *r, uint8_t *buf, int maxlen) {
    /* /proc/.midi_ring backend — kernel handles cursors and masking */
    if (r->ring_fd >= 0)
        return read(r->ring_fd, buf, maxlen);

    /* Shared memory fallback */
    uint32_t w, avail, i;
    int len;

    if (!r->valid && r->container) {
        int entry = 0x14C;
        r->mask = *(uint32_t *)(r->container + entry + 0x4C);
        if (r->mask > 0 && !r->ring_data) {
            int blk_id = *(uint32_t *)(r->container + entry + 0x44);
            int blksz;
            r->ring_data = lock_block(blk_id, &blksz);
            if (r->ring_data) {
                r->p_write = (uint32_t *)(r->container + entry + 0x50);
                r->our_cursor = *r->p_write;
                r->valid = 1;
                if (debug)
                    fprintf(stderr, "MIDI Out: late init OK, mask=0x%x cursor=%u\n",
                            r->mask, r->our_cursor);
            }
        }
    }

    if (!r->valid) return 0;

    w = *r->p_write;
    if (w == r->our_cursor) return 0;

    avail = w - r->our_cursor;
    len = avail > (uint32_t)maxlen ? maxlen : (int)avail;

    for (i = 0; i < (uint32_t)len; i++)
        buf[i] = r->ring_data[(r->our_cursor + i) & r->mask];

    r->our_cursor += len;
    return len;
}

static void reset_midi_out(struct midi_out_reader *r) {
    if (r->ring_fd >= 0) {
        /* Write anything to /proc/.midi_ring to reset cursor */
        write(r->ring_fd, "R", 1);
    } else if (r->valid && r->p_write) {
        r->our_cursor = *r->p_write;
    }
}

/* Global reference so capture thread can access it */
static struct midi_out_reader *g_midi_out = NULL;

static void *sysex_capture_thread(void *arg)
{
    (void)arg;
    while (running) {
        pthread_mutex_lock(&capture_mutex);
        while (!sysex_capturing && running)
            pthread_cond_wait(&capture_start_cond, &capture_mutex);
        pthread_mutex_unlock(&capture_mutex);

        if (!running) break;

        int caplen = 0;
        int got_f7 = 0;
        int idle_loops = 0;
        struct timeval t0, tnow;
        gettimeofday(&t0, NULL);

        while (caplen < SYSEX_CAP_SIZE && running) {
            int chunk = read_midi_out(g_midi_out,
                                       sysex_capbuf + caplen,
                                       SYSEX_CAP_SIZE - caplen);
            if (chunk > 0) {
                int j;
                for (j = caplen; j < caplen + chunk; j++)
                    if (sysex_capbuf[j] == 0xF7) got_f7++;
                caplen += chunk;
                idle_loops = 0;

                if (got_f7) {
                    usleep(500);
                    chunk = read_midi_out(g_midi_out,
                                           sysex_capbuf + caplen,
                                           SYSEX_CAP_SIZE - caplen);
                    if (chunk > 0) {
                        caplen += chunk;
                        got_f7 = 0;
                    } else {
                        break;
                    }
                }
            } else {
                idle_loops++;
                if (idle_loops > 100000) break;
            }

            gettimeofday(&tnow, NULL);
            long elapsed_ms = (tnow.tv_sec - t0.tv_sec) * 1000 +
                              (tnow.tv_usec - t0.tv_usec) / 1000;
            if (elapsed_ms > 3000) break;
        }

        sysex_caplen = caplen;
        sysex_capturing = 0;
        sysex_done = 1;
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    int port = 9875;
    int foreground = 0;
    int opt;
    int server_fd, client_fd = -1;
    int midi_fd = -1;
    struct sockaddr_in addr;
    struct midi_out_reader midi_out;
    uint8_t buf[4096];

    while ((opt = getopt(argc, argv, "p:ds")) != -1) {
        switch (opt) {
        case 'p': port = atoi(optarg); break;
        case 'd': debug = 1; break;
        case 's': foreground = 1; break;
        }
    }

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);
    signal(SIGPIPE, SIG_IGN);

    if (setup_midi_out(&midi_out) < 0)
        fprintf(stderr, "Warning: MIDI output monitoring unavailable\n");

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int reuse = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    if (listen(server_fd, 2) < 0) { perror("listen"); return 1; }

    if (!foreground) {
        if (fork() > 0) _exit(0);
        setsid();
    }

    g_midi_out = &midi_out;
    pthread_create(&capture_tid, NULL, sysex_capture_thread, NULL);

    fprintf(stderr, "midi_tcp: listening on port %d%s\n", port,
            midi_out.ring_fd >= 0 ? " (upgraded ring)" : "");

    while (running) {
        fd_set rfds;
        struct timeval tv;
        int maxfd = server_fd;

        FD_ZERO(&rfds);
        FD_SET(server_fd, &rfds);
        if (client_fd >= 0) {
            FD_SET(client_fd, &rfds);
            if (client_fd > maxfd) maxfd = client_fd;
        }

        tv.tv_sec = 0;
        tv.tv_usec = 1000;

        int ret = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (FD_ISSET(server_fd, &rfds)) {
            struct sockaddr_in caddr;
            socklen_t clen = sizeof(caddr);
            int new_fd = accept(server_fd, (struct sockaddr *)&caddr, &clen);
            if (new_fd >= 0) {
                if (client_fd >= 0) close(client_fd);
                client_fd = new_fd;
                int nodelay = 1;
                setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

                if (midi_fd >= 0) close(midi_fd);
                midi_fd = open("/proc/.midi_in", O_WRONLY);
                if (midi_fd < 0)
                    fprintf(stderr, "warning: can't open /proc/.midi_in\n");

                reset_midi_out(&midi_out);

                if (debug)
                    fprintf(stderr, "client connected\n");
            }
        }

        if (client_fd >= 0 && FD_ISSET(client_fd, &rfds)) {
            int n = recv(client_fd, buf, sizeof(buf), 0);
            if (n <= 0) {
                if (debug) fprintf(stderr, "client disconnected\n");
                close(client_fd);
                client_fd = -1;
                if (midi_fd >= 0) { close(midi_fd); midi_fd = -1; }
            } else {
                if (debug) {
                    fprintf(stderr, "TCP->MIDI %d bytes:", n);
                    int i;
                    for (i = 0; i < n && i < 16; i++)
                        fprintf(stderr, " %02x", buf[i]);
                    if (n > 16) fprintf(stderr, " ...");
                    fprintf(stderr, "\n");
                }
                if (midi_fd >= 0) {
                    int is_sysex = (n > 0 && buf[0] == 0xF0);

                    if (is_sysex && midi_out.valid) {
                        sysex_caplen = 0;
                        sysex_done = 0;
                        sysex_capturing = 1;
                        pthread_cond_signal(&capture_start_cond);
                        usleep(50);
                    }

                    write(midi_fd, buf, n);

                    if (is_sysex && midi_out.valid) {
                        int wait_ms = 0;
                        while (!sysex_done && wait_ms < 4000) {
                            usleep(1000);
                            wait_ms++;
                        }

                        if (sysex_caplen > 0) {
                            if (debug) {
                                fprintf(stderr, "MIDI->TCP %d bytes (SysEx):",
                                        sysex_caplen);
                                int j;
                                for (j = 0; j < sysex_caplen && j < 16; j++)
                                    fprintf(stderr, " %02x", sysex_capbuf[j]);
                                if (sysex_caplen > 16) fprintf(stderr, " ...");
                                fprintf(stderr, "\n");
                            }
                            send(client_fd, sysex_capbuf, sysex_caplen, 0);
                        }
                    }
                }
            }
        }

        if (client_fd >= 0) {
            int n = read_midi_out(&midi_out, buf, sizeof(buf));
            if (n > 0) {
                if (debug) {
                    fprintf(stderr, "MIDI->TCP %d bytes:", n);
                    int i;
                    for (i = 0; i < n && i < 16; i++)
                        fprintf(stderr, " %02x", buf[i]);
                    if (n > 16) fprintf(stderr, " ...");
                    fprintf(stderr, "\n");
                }
                send(client_fd, buf, n, 0);
            }
        }
    }

    sysex_capturing = 1;
    pthread_cond_signal(&capture_start_cond);
    pthread_join(capture_tid, NULL);

    if (midi_out.ring_fd >= 0) close(midi_out.ring_fd);
    if (client_fd >= 0) close(client_fd);
    if (midi_fd >= 0) close(midi_fd);
    close(server_fd);
    fprintf(stderr, "midi_tcp: shutdown\n");
    return 0;
}
