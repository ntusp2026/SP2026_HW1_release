#include "server.h"

#include <signal.h>

const unsigned char IAC_IP[2] = {0xff, 0xf4};

const char *welcome_banner =
    "================================\n"
    " Welcome to CSIE Ledger System \n"
    "================================\n";
const char *ready_prompt = "Please enter your command: ";

server svr;
request *requestP = NULL;
int maxfd = 0;
int record_fd = -1;

static void init_server(unsigned short port);
static void init_request(request *reqP);
static void free_request(request *reqP);
static int accept_conn(void);
static ssize_t write_all(int fd, const void *buf, size_t len);

/*
 * Return values:
 *   1: some input was read
 *   0: EOF / client disconnected
 *  -1: read error
 *
 * This starter function intentionally does not handle fragmented commands.
 * Replace or extend it so each client keeps incomplete bytes until '\n'.
 */
static int handle_read(request *reqP) {
    char buf[MAX_MSG_LEN];
    ssize_t nread = read(reqP->conn_fd, buf, sizeof(buf) - 1);

    if (nread < 0) {
        return -1;
    }
    if (nread == 0) {
        return 0;
    }

    buf[nread] = '\0';

    if (nread >= 2 &&
        (unsigned char)buf[0] == IAC_IP[0] &&
        (unsigned char)buf[1] == IAC_IP[1]) {
        return 0;
    }

    /* TODO: Accumulate input per client and extract one complete command. */
    size_t copy_len = (size_t)nread;
    if (copy_len >= sizeof(reqP->buf)) {
        copy_len = sizeof(reqP->buf) - 1;
    }
    memcpy(reqP->buf, buf, copy_len);
    reqP->buf[copy_len] = '\0';
    reqP->buf_len = copy_len;

    char *newline = strchr(reqP->buf, '\n');
    if (newline != NULL) {
        *newline = '\0';
        if (newline > reqP->buf && newline[-1] == '\r') {
            newline[-1] = '\0';
        }
        reqP->buf_len = strlen(reqP->buf);
    }

    return 1;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s [port]\n", argv[0]);
        return EXIT_FAILURE;
    }

    signal(SIGPIPE, SIG_IGN);

    record_fd = open(RECORD_PATH, O_RDWR);
    if (record_fd < 0) {
        ERR_EXIT("open accountRecord");
    }

    init_server((unsigned short)atoi(argv[1]));

    fprintf(stderr, "starting on %s, port %u, fd %d, maxconn %d...\n",
            svr.hostname, svr.port, svr.listen_fd, maxfd);

    /*
     * TODO:
     *   1. Replace this one-client-at-a-time loop with select() or poll().
     *   2. Implement the READY / TRANSACTION state machine.
     *   3. Add nonblocking byte-range record locks.
     *   4. Track same-process account ownership.
     *   5. Clean up locks and state on every disconnect/error path.
     */
    while (1) {
        int conn_fd = accept_conn();
        if (conn_fd < 0) {
            continue;
        }

        request *reqP = &requestP[conn_fd];
        if (write_all(conn_fd, welcome_banner, strlen(welcome_banner)) < 0 ||
            write_all(conn_fd, ready_prompt, strlen(ready_prompt)) < 0) {
            close(conn_fd);
            free_request(reqP);
            continue;
        }

        int ret = handle_read(reqP);
        if (ret > 0) {
            char reply[MAX_MSG_LEN + 64];
            int len = snprintf(reply, sizeof(reply),
                               "ACCEPT_FROM_CLIENT : %s\n", reqP->buf);
            if (len > 0) {
                size_t reply_len = (size_t)len;
                if (reply_len >= sizeof(reply)) {
                    reply_len = sizeof(reply) - 1;
                }
                (void)write_all(conn_fd, reply, reply_len);
            }
        }

        close(conn_fd);
        free_request(reqP);
    }

    close(record_fd);
    close(svr.listen_fd);
    free(requestP);
    return EXIT_SUCCESS;
}

static int accept_conn(void) {
    struct sockaddr_in cliaddr;
    socklen_t clilen = sizeof(cliaddr);

    int conn_fd = accept(svr.listen_fd, (struct sockaddr *)&cliaddr, &clilen);
    if (conn_fd < 0) {
        if (errno == EINTR || errno == EAGAIN) {
            return -1;
        }
        ERR_EXIT("accept");
    }

    if (conn_fd >= maxfd) {
        close(conn_fd);
        return -1;
    }

    requestP[conn_fd].conn_fd = conn_fd;
    snprintf(requestP[conn_fd].host, sizeof(requestP[conn_fd].host), "%s",
             inet_ntoa(cliaddr.sin_addr));

    fprintf(stderr, "new connection: fd %d from %s\n",
            conn_fd, requestP[conn_fd].host);
    return conn_fd;
}

static void init_request(request *reqP) {
    memset(reqP, 0, sizeof(*reqP));
    reqP->conn_fd = -1;
    reqP->state = READY;
    reqP->account_index = -1;
}

static void free_request(request *reqP) {
    /* TODO: Release any transaction lock owned by this client first. */
    init_request(reqP);
}

static void init_server(unsigned short port) {
    struct sockaddr_in servaddr;
    int reuse = 1;

    if (gethostname(svr.hostname, sizeof(svr.hostname)) < 0) {
        ERR_EXIT("gethostname");
    }
    svr.port = port;

    svr.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (svr.listen_fd < 0) {
        ERR_EXIT("socket");
    }

    if (setsockopt(svr.listen_fd, SOL_SOCKET, SO_REUSEADDR,
                   &reuse, sizeof(reuse)) < 0) {
        ERR_EXIT("setsockopt");
    }

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(port);

    if (bind(svr.listen_fd, (struct sockaddr *)&servaddr,
             sizeof(servaddr)) < 0) {
        ERR_EXIT("bind");
    }
    if (listen(svr.listen_fd, 1024) < 0) {
        ERR_EXIT("listen");
    }

    maxfd = getdtablesize();
    requestP = calloc((size_t)maxfd, sizeof(*requestP));
    if (requestP == NULL) {
        ERR_EXIT("calloc request table");
    }

    for (int i = 0; i < maxfd; ++i) {
        init_request(&requestP[i]);
    }
    requestP[svr.listen_fd].conn_fd = svr.listen_fd;
}

static ssize_t write_all(int fd, const void *buf, size_t len) {
    const char *ptr = buf;
    size_t written = 0;

    while (written < len) {
        ssize_t ret = write(fd, ptr + written, len - written);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        written += (size_t)ret;
    }
    return (ssize_t)written;
}
