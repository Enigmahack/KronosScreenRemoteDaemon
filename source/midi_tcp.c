/*
 * midi_tcp - TCP MIDI bridge for Korg Kronos (hub mode)
 *
 * Listens on TCP port 9875 (same as Korg's MIDID).
 * Inbound:  TCP -> /proc/.midi_in (kernel module injection)
 * Outbound: /proc/.midi_ring -> TCP
 *
 * MIDI output is broadcast to ALL connected TCP clients simultaneously.  Channel
 * and system-common messages are parsed and broadcast whole; real-time bytes pass
 * through immediately.  SysEx is the exception: it is STREAMED in <=1 KB chunks
 * (and flushed again at F7) rather than buffered whole, so an arbitrarily large
 * object - a full Set List dump is ~79 KB - crosses the bridge with no size cap
 * and a client's activity/keepalive stays fed through a multi-second transfer;
 * clients reassemble F0..F7 across chunk boundaries (see parser_feed).  Up to
 * MAX_CLIENTS simultaneous connections are supported.  MIDI input received from
 * any client is injected into the Kronos; clients are independent and see each
 * other's injected data only if the Kronos echoes it back.
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
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>

#define MAX_CLIENTS 8

static int debug = 0;
static volatile int running = 1;

static void sighandler(int sig) { running = 0; }

/* ------------------------------------------------------------------ */
/*  Hub state                                                           */
/* ------------------------------------------------------------------ */

static int client_fds[MAX_CLIENTS];
static int num_clients = 0;
static int midi_fd = -1;

static void broadcast(const uint8_t *buf, int len)
{
    int i;
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (client_fds[i] >= 0)
            send(client_fds[i], buf, len, MSG_DONTWAIT);
    }
}

/* ------------------------------------------------------------------ */
/*  MIDI output reader - /proc/.midi_ring                              */
/* ------------------------------------------------------------------ */

struct midi_out_reader {
    int ring_fd;
    int valid;
};

static int setup_midi_out(struct midi_out_reader *r) {
    memset(r, 0, sizeof(*r));
    r->ring_fd = open("/proc/.midi_ring", O_RDWR);
    if (r->ring_fd < 0) {
        if (debug) fprintf(stderr, "MIDI Out: /proc/.midi_ring unavailable\n");
        return -1;
    }
    r->valid = 1;
    if (debug) fprintf(stderr, "MIDI Out: using /proc/.midi_ring\n");
    return 0;
}

static int read_midi_out(struct midi_out_reader *r, uint8_t *buf, int maxlen) {
    if (r->ring_fd < 0) return 0;
    return read(r->ring_fd, buf, maxlen);
}

static void reset_midi_out(struct midi_out_reader *r) {
    if (r->ring_fd >= 0)
        write(r->ring_fd, "R", 1);
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

/* SysEx is streamed to clients in small chunks, NOT buffered as a whole message.
 * A large object dump - a Kronos Set List is ~79 KB - must traverse the bridge
 * without any size cap, and the client's activity-based keepalive needs to see
 * bytes arriving throughout the multi-second transfer.
 *
 * The old design buffered the entire F0...F7 and broadcast once, on F7, into a
 * 64 KB buffer.  That broke large dumps two ways: (1) at 64 KB the length guard
 * also blocked the terminating F7, so the client received a truncated chunk that
 * never completed into a message; (2) nothing was sent until the whole transfer
 * finished, so the client's no-response timer fired mid-dump.  Now we flush every
 * SYSEX_FLUSH_AT bytes and again on F7; the client's MIDI parser reassembles
 * across chunk boundaries (only the first chunk carries F0, only the last F7). */
#define SYSEX_BUF_SIZE 4096
#define SYSEX_FLUSH_AT 1024

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

/* Feed one byte through the parser.  Broadcasts a complete MIDI message to
 * all connected clients as soon as one is assembled.  Real-time bytes
 * (0xF8-0xFF) are emitted immediately without disturbing parser state, so
 * they are correctly interleaved even inside a SysEx message. */
static void parser_feed(struct midi_parser *p, uint8_t b)
{
    /* Real-time: single byte, broadcast immediately. */
    if (b >= 0xF8) {
        if (debug)
            fprintf(stderr, "MIDI->TCP 1 byte (rt): %02x\n", b);
        broadcast(&b, 1);
        return;
    }

    /* SysEx end: emit the final chunk (carrying F7) and reset.  len is always
     * < SYSEX_FLUSH_AT here (a full chunk was flushed the moment it filled), so
     * there is room for the F7 - the old 64 KB truncation that dropped it is gone. */
    if (b == 0xF7) {
        if (p->in_sysex) {
            p->buf[p->len++] = b;
            if (debug) {
                int j;
                fprintf(stderr, "MIDI->TCP %d bytes (SysEx end):", p->len);
                for (j = 0; j < p->len && j < 16; j++)
                    fprintf(stderr, " %02x", p->buf[j]);
                if (p->len > 16) fprintf(stderr, " ...");
                fprintf(stderr, "\n");
            }
            broadcast(p->buf, p->len);
            p->len = 0;
            p->in_sysex = 0;
            p->running_status = 0;
        }
        return;
    }

    /* SysEx body: accumulate and stream out in SYSEX_FLUSH_AT-sized chunks so a
     * large dump traverses the bridge live rather than being buffered whole.  The
     * flush keeps in_sysex set and re-emits no F0/F7 - the client parser stays in
     * its SysEx state and reassembles the chunks into one message. */
    if (p->in_sysex) {
        p->buf[p->len++] = b;
        if (p->len >= SYSEX_FLUSH_AT) {
            broadcast(p->buf, p->len);
            p->len = 0;
        }
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
            /* System common - cancels running status. */
            int slen = syscom_len[b & 0x0F];
            p->running_status = 0;
            p->buf[0] = b;
            p->len = 1;
            p->expected = slen;
            if (slen == 1) {
                if (debug) fprintf(stderr, "MIDI->TCP 1 byte (syscom): %02x\n", b);
                broadcast(p->buf, 1);
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
            broadcast(p->buf, p->len);
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
    int server_fd;
    int i;
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

    for (i = 0; i < MAX_CLIENTS; i++) client_fds[i] = -1;

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

    if (listen(server_fd, MAX_CLIENTS) < 0) { perror("listen"); return 1; }

    if (!foreground) {
        if (fork() > 0) _exit(0);
        setsid();
    }

    parser_init(&parser);

    fprintf(stderr, "midi_tcp: listening on port %d%s (hub, max %d clients) "
            "[sysex-streaming flush=%d]\n",
            port, midi_out.valid ? " (midi_ring)" : " (no MIDI out)", MAX_CLIENTS,
            SYSEX_FLUSH_AT);

    while (running) {
        fd_set rfds;
        struct timeval tv;
        int maxfd = server_fd;

        FD_ZERO(&rfds);
        FD_SET(server_fd, &rfds);
        for (i = 0; i < MAX_CLIENTS; i++) {
            if (client_fds[i] >= 0) {
                FD_SET(client_fds[i], &rfds);
                if (client_fds[i] > maxfd) maxfd = client_fds[i];
            }
        }

        tv.tv_sec = 0;
        tv.tv_usec = 1000;

        int ret = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* New connection: add to first available slot, or reject if hub is full. */
        if (FD_ISSET(server_fd, &rfds)) {
            struct sockaddr_in caddr;
            socklen_t clen = sizeof(caddr);
            int new_fd = accept(server_fd, (struct sockaddr *)&caddr, &clen);
            if (new_fd >= 0) {
                int slot = -1;
                for (i = 0; i < MAX_CLIENTS; i++) {
                    if (client_fds[i] < 0) { slot = i; break; }
                }
                if (slot < 0) {
                    /* Hub full - reject connection */
                    close(new_fd);
                    if (debug) fprintf(stderr, "hub full, rejected connection\n");
                } else {
                    int nodelay = 1;
                    setsockopt(new_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

                    /* Big send buffer so a momentary client stall can't fill it and
                     * make the MSG_DONTWAIT broadcast() silently drop a chunk mid-
                     * SysEx (which offsets the rest of a bulk dump).  A full Set List
                     * is ~79 KB; 256 KB gives comfortable margin.  Belt-and-braces
                     * alongside the kernel-ring SPSC fix - kept non-blocking so a slow
                     * client can never stall the /proc/.midi_ring drain. */
                    int sndbuf = 256 * 1024;
                    setsockopt(new_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

                    /* Open MIDI injection and reset ring cursor on first client */
                    if (num_clients == 0) {
                        if (midi_fd >= 0) { close(midi_fd); midi_fd = -1; }
                        midi_fd = open("/proc/.midi_in", O_WRONLY);
                        if (midi_fd < 0)
                            fprintf(stderr, "warning: can't open /proc/.midi_in\n");
                        reset_midi_out(&midi_out);
                        parser_init(&parser);
                    }

                    client_fds[slot] = new_fd;
                    num_clients++;

                    if (debug)
                        fprintf(stderr, "client[%d] connected (total=%d)\n",
                                slot, num_clients);
                }
            }
        }

        /* Inbound MIDI from any TCP client -> inject into Kronos. */
        for (i = 0; i < MAX_CLIENTS; i++) {
            if (client_fds[i] < 0 || !FD_ISSET(client_fds[i], &rfds)) continue;
            int n = recv(client_fds[i], buf, sizeof(buf), 0);
            if (n <= 0) {
                if (debug)
                    fprintf(stderr, "client[%d] disconnected (total=%d)\n",
                            i, num_clients - 1);
                close(client_fds[i]);
                client_fds[i] = -1;
                num_clients--;
                if (num_clients == 0 && midi_fd >= 0) {
                    close(midi_fd);
                    midi_fd = -1;
                }
            } else {
                if (debug) {
                    int j;
                    fprintf(stderr, "TCP[%d]->MIDI %d bytes:", i, n);
                    for (j = 0; j < n && j < 16; j++)
                        fprintf(stderr, " %02x", buf[j]);
                    if (n > 16) fprintf(stderr, " ...");
                    fprintf(stderr, "\n");
                }
                if (midi_fd >= 0)
                    write(midi_fd, buf, n);
            }
        }

        /* Outbound MIDI from Kronos -> parse -> broadcast to all clients. */
        if (num_clients > 0) {
            int n = read_midi_out(&midi_out, buf, sizeof(buf));
            int j;
            for (j = 0; j < n; j++)
                parser_feed(&parser, buf[j]);
        }
    }

    if (midi_out.ring_fd >= 0) close(midi_out.ring_fd);
    for (i = 0; i < MAX_CLIENTS; i++)
        if (client_fds[i] >= 0) close(client_fds[i]);
    if (midi_fd >= 0) close(midi_fd);
    close(server_fd);
    fprintf(stderr, "midi_tcp: shutdown\n");
    return 0;
}
