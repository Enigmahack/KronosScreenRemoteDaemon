/*
 * midi_tcp — TCP MIDI bridge for Korg Kronos
 *
 * Listens on TCP port 9875 (same as Korg's MIDID).
 * Inbound:  TCP → /proc/.midi_in (kernel module injection)
 * Outbound: /proc/.midi_ring (upgraded) or shared memory (fallback) → TCP
 *
 * All MIDI output is parsed into complete messages and forwarded to the TCP
 * client immediately as each message completes — SysEx, regular channel
 * messages, and real-time bytes alike.  The TCP client must tolerate
 * asynchronous, interleaved delivery and correlate SysEx responses to
 * requests by message content rather than timing.
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
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>

static int debug = 0;
static volatile int running = 1;

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

/* ------------------------------------------------------------------ */
/*  MIDI parser                                                         */
/* ------------------------------------------------------------------ */

/* Total message length (including status) for channel messages 0x8n-0xEn.
 * Index = (status >> 4) & 0x07. */
static const int chan_msg_len[7] = {
    3,  /* 0x8n Note Off */
    3,  /* 0x9n Note On */
    3,  /* 0xAn Poly Aftertouch */
    3,  /* 0xBn Control Change */
    2,  /* 0xCn Program Change */
    2,  /* 0xDn Channel Aftertouch */
    3,  /* 0xEn Pitch Bend */
};

/* Length for system-common messages 0xF1-0xF6.
 * Index = b & 0x0F (1=F1 .. 6=F6). */
static const int syscom_len[7] = {
    0,  /* unused */
    2,  /* 0xF1 MTC Quarter Frame */
    3,  /* 0xF2 Song Position */
    2,  /* 0xF3 Song Select */
    1,  /* 0xF4 undefined */
    1,  /* 0xF5 undefined */
    1,  /* 0xF6 Tune Request */
};

#define SYSEX_BUF_SIZE 65536

struct midi_parser {
    uint8_t  buf[SYSEX_BUF_SIZE];
    int      len;
    int      expected;
    int      in_sysex;
    uint8_t  running_status;
};

static void parser_init(struct midi_parser *p) {
    memset(p, 0, sizeof(*p));
}

/* Feed one byte through the parser.  Sends a complete MIDI message to
 * client_fd as soon as one is assembled.  Real-time bytes (0xF8-0xFF)
 * are emitted immediately without disturbing parser state, so they
 * are correctly interleaved even inside a SysEx message. */
static void parser_feed(struct midi_parser *p, uint8_t b, int client_fd)
{
    /* Real-time: single byte, pass through immediately. */
    if (b >= 0xF8) {
        if (debug)
            fprintf(stderr, "MIDI->TCP 1 byte (rt): %02x\n", b);
        if (client_fd >= 0)
            send(client_fd, &b, 1, 0);
        return;
    }

    /* SysEx end: flush accumulated SysEx message. */
    if (b == 0xF7) {
        if (p->in_sysex) {
            if (p->len < SYSEX_BUF_SIZE)
                p->buf[p->len++] = b;
            if (debug) {
                int j;
                fprintf(stderr, "MIDI->TCP %d bytes (SysEx):", p->len);
                for (j = 0; j < p->len && j < 16; j++)
                    fprintf(stderr, " %02x", p->buf[j]);
                if (p->len > 16) fprintf(stderr, " ...");
                fprintf(stderr, "\n");
            }
            if (client_fd >= 0)
                send(client_fd, p->buf, p->len, 0);
            p->len = 0;
            p->in_sysex = 0;
            p->running_status = 0;
        }
        return;
    }

    /* SysEx body: accumulate until F7. */
    if (p->in_sysex) {
        if (p->len < SYSEX_BUF_SIZE)
            p->buf[p->len++] = b;
        return;
    }

    /* New status byte. */
    if (b & 0x80) {
        if (b == 0xF0) {
            p->in_sysex = 1;
            p->len = 0;
            p->buf[p->len++] = b;
            p->running_status = 0;
            p->expected = 0;
            return;
        }

        if (b >= 0xF1 && b <= 0xF6) {
            /* System common — cancels running status. */
            int slen = syscom_len[b & 0x0F];
            p->running_status = 0;
            p->buf[0] = b;
            p->len = 1;
            p->expected = slen;
            if (slen == 1) {
                if (debug) fprintf(stderr, "MIDI->TCP 1 byte (syscom): %02x\n", b);
                if (client_fd >= 0) send(client_fd, p->buf, 1, 0);
                p->len = 0;
            }
            return;
        }

        /* Channel message (0x80-0xEF). */
        p->running_status = b;
        p->expected = chan_msg_len[(b >> 4) & 0x07];
        p->buf[0] = b;
        p->len = 1;
        return;
    }

    /* Data byte. */

    /* Apply running status when starting a new message without an explicit status. */
    if (p->len == 0) {
        if (!p->running_status)
            return;
        p->expected = chan_msg_len[(p->running_status >> 4) & 0x07];
        p->buf[0] = p->running_status;
        p->len = 1;
    }

    if (p->expected > 0 && p->len < p->expected) {
        p->buf[p->len++] = b;
        if (p->len == p->expected) {
            if (debug) {
                int j;
                fprintf(stderr, "MIDI->TCP %d bytes:", p->len);
                for (j = 0; j < p->len && j < 16; j++)
                    fprintf(stderr, " %02x", p->buf[j]);
                fprintf(stderr, "\n");
            }
            if (client_fd >= 0)
                send(client_fd, p->buf, p->len, 0);
            /* Keep running_status; reset len so next data byte re-uses it. */
            p->len = 0;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[]) {
    int port = 9875;
    int foreground = 0;
    int opt;
    int server_fd, client_fd = -1;
    int midi_fd = -1;
    struct sockaddr_in addr;
    struct midi_out_reader midi_out;
    struct midi_parser parser;
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

    parser_init(&parser);

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

        /* New connection: replace any existing client. */
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
                parser_init(&parser);

                if (debug)
                    fprintf(stderr, "client connected\n");
            }
        }

        /* Inbound MIDI from TCP client → write directly to kernel. */
        if (client_fd >= 0 && FD_ISSET(client_fd, &rfds)) {
            int n = recv(client_fd, buf, sizeof(buf), 0);
            if (n <= 0) {
                if (debug) fprintf(stderr, "client disconnected\n");
                close(client_fd);
                client_fd = -1;
                if (midi_fd >= 0) { close(midi_fd); midi_fd = -1; }
            } else {
                if (debug) {
                    int i;
                    fprintf(stderr, "TCP->MIDI %d bytes:", n);
                    for (i = 0; i < n && i < 16; i++)
                        fprintf(stderr, " %02x", buf[i]);
                    if (n > 16) fprintf(stderr, " ...");
                    fprintf(stderr, "\n");
                }
                if (midi_fd >= 0)
                    write(midi_fd, buf, n);
            }
        }

        /* Outbound MIDI from Kronos → parse → TCP. */
        if (client_fd >= 0) {
            int n = read_midi_out(&midi_out, buf, sizeof(buf));
            int i;
            for (i = 0; i < n; i++)
                parser_feed(&parser, buf[i], client_fd);
        }
    }

    if (midi_out.ring_fd >= 0) close(midi_out.ring_fd);
    if (client_fd >= 0) close(client_fd);
    if (midi_fd >= 0) close(midi_fd);
    close(server_fd);
    fprintf(stderr, "midi_tcp: shutdown\n");
    return 0;
}
