#include "server.h"

#include <limits.h>
#include <signal.h>

const unsigned char IAC_IP[2] = {0xff, 0xf4};

const char *welcome_banner =
    "================================\n"
    " Welcome to CSIE Ledger System \n"
    "================================\n";

const char *ready_prompt = "Please enter your command: ";
const char *transaction_prompt = "Please enter an operation: ";

server svr;
request *requestP = NULL;

int maxfd = 0;
int record_fd = -1;

/*
 * account_owner[i]
 *
 * -1  : no client in this server process owns this account
 * >=0 : socket fd of the client that currently owns this account
 *
 * fcntl record locks are process-associated, so this extra table is
 * needed to distinguish clients within the same server process.
 */
static int account_owner[ACCOUNT_NUM];


/* ============================================================
 * Function declarations
 * ============================================================ */

/* Server / connection helpers */
static void init_server(unsigned short port);
static void init_request(request *reqP);
static void free_request(request *reqP);
static int accept_conn(void);
static int send_welcome(int fd);
static ssize_t write_all(int fd, const void *buf, size_t len);

/* Input helpers */
static int recv_into_buffer(request *reqP);
static int pop_command(request *reqP, char *command, size_t command_size);

/* Command handling */
static int handle_command(request *reqP, const char *command);

/* Parsing helpers */
static int parse_int_strict(const char *text, int *value);
static int parse_account_id(const char *text, int *account_id);

/* Record helpers */
static int account_index(int account_id);
static off_t account_offset(int account_id);
static int read_account(int account_id, account_record *record);
static int write_account(int account_id, const account_record *record);

/* Lock helpers */
static int try_record_lock(int account_id, short lock_type);
static int unlock_record(int account_id);

/* Cleanup helpers */
static void cleanup_transaction(request *reqP);
static void close_client(int fd, fd_set *master_set);

/* Response helpers */
static void send_locked(int fd);
static void send_invalid_command(int fd);


/* ============================================================
 * main
 * ============================================================ */

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s [port]\n", argv[0]);
        return EXIT_FAILURE;
    }

    signal(SIGPIPE, SIG_IGN);

    /*
     * Open accountRecord once and reuse the same fd throughout
     * the lifetime of this server process.
     */
    record_fd = open(RECORD_PATH, O_RDWR);
    if (record_fd < 0) {
        ERR_EXIT("open accountRecord");
    }

    for (int i = 0; i < ACCOUNT_NUM; ++i) {
        account_owner[i] = -1;
    }

    init_server((unsigned short)atoi(argv[1]));

    fprintf(stderr, "starting on %s, port %u, fd %d, maxconn %d...\n",
            svr.hostname, svr.port, svr.listen_fd, maxfd);

    /*
     * ========================================================
     * TODO 1: I/O Multiplexing
     * ========================================================
     *
     * Replace the temporary one-client-at-a-time loop below
     * with select() or poll().
     *
     * The event loop should monitor:
     *
     *   1. svr.listen_fd
     *      -> accept a new client
     *
     *   2. connected client sockets
     *      -> recv_into_buffer()
     *      -> repeatedly pop_command()
     *      -> handle_command()
     *
     * On disconnect/error:
     *
     *      close_client()
     *
     * Do NOT block on one idle client.
     */

    /*
     * Temporary starter implementation.
     * Replace this entire loop for TODO 1.
     */
    while (1) {
        int conn_fd = accept_conn();
        if (conn_fd < 0) {
            continue;
        }

        request *reqP = &requestP[conn_fd];

        if (send_welcome(conn_fd) < 0) {
            close(conn_fd);
            free_request(reqP);
            continue;
        }

        int ret = recv_into_buffer(reqP);

        if (ret > 0) {
            char command[MAX_MSG_LEN];

            while (pop_command(reqP, command, sizeof(command)) > 0) {
                fprintf(stderr, "received: [%s]\n", command);

                if (handle_command(reqP, command)) {
                    break;
                }
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


/* ============================================================
 * Input buffering
 * ============================================================ */

/*
 * Return:
 *   1 : some bytes were received
 *   0 : EOF / client disconnected
 *  -1 : read error / buffer overflow
 */
static int recv_into_buffer(request *reqP) {
    char incoming[MAX_MSG_LEN];

    ssize_t nread = read(reqP->conn_fd, incoming, sizeof(incoming));

    if (nread < 0) {
        if (errno == EINTR) {
            return 1;
        }

        return -1;
    }

    if (nread == 0) {
        return 0;
    }

    if (nread >= 2 &&
        (unsigned char)incoming[0] == IAC_IP[0] &&
        (unsigned char)incoming[1] == IAC_IP[1]) {
        return 0;
    }

    /*
     * ========================================================
     * TODO 2: Fragmented TCP input
     * ========================================================
     *
     * TCP is a byte stream. One command may arrive through
     * multiple read() calls.
     *
     * Example:
     *
     *     begin 902001\n
     *
     * may arrive as:
     *
     *     "beg"
     *     "in 902"
     *     "001\n"
     *
     * Append new bytes to reqP->buf starting at reqP->buf_len.
     *
     * Do NOT overwrite incomplete bytes from previous reads.
     *
     * Remember to:
     *
     *   - check remaining buffer space
     *   - update reqP->buf_len
     */

    (void)incoming;
    (void)nread;

    return 1;
}


/* ============================================================
 * Command extraction
 * ============================================================ */

/*
 * Return:
 *   1 : one complete command was extracted
 *   0 : no complete command yet
 *  -1 : malformed / command too long
 */
static int pop_command(request *reqP, char *command, size_t command_size) {
    char *newline = memchr(reqP->buf, '\n', reqP->buf_len);

    if (newline == NULL) {
        if (reqP->buf_len >= sizeof(reqP->buf)) {
            return -1;
        }

        return 0;
    }

    size_t line_len = (size_t)(newline - reqP->buf);
    size_t command_len = line_len;

    /*
     * Support CRLF input:
     *
     *     command\r\n
     *
     * Remove the optional '\r' before '\n'.
     */
    if (command_len > 0 && reqP->buf[command_len - 1] == '\r') {
        command_len--;
    }

    if (command_len >= command_size) {
        return -1;
    }

    memcpy(command, reqP->buf, command_len);
    command[command_len] = '\0';

    /*
     * Remove the consumed command from reqP->buf while preserving
     * any leftover bytes.
     *
     * Example:
     *
     *     read 902001\nexit\n
     *
     * becomes:
     *
     *     exit\n
     */
    size_t consumed = line_len + 1;
    size_t remaining = reqP->buf_len - consumed;

    memmove(reqP->buf, reqP->buf + consumed, remaining);

    reqP->buf_len = remaining;

    return 1;
}


/* ============================================================
 * Command state machine
 * ============================================================ */

/*
 * Return:
 *   0 : keep connection alive
 *   1 : close connection
 */
static int handle_command(request *reqP, const char *command) {
    /*
     * ========================================================
     * TODO 3: READY / TRANSACTION state machine
     * ========================================================
     *
     * READY:
     *
     *     read <account_id>
     *     begin <account_id>
     *     exit
     *
     *
     * TRANSACTION:
     *
     *     add <delta>
     *     commit
     *     abort
     *     exit
     *
     *
     * Suggested structure:
     *
     *     if (reqP->state == READY) {
     *         ...
     *     }
     *
     *     if (reqP->state == TRANSACTION) {
     *         ...
     *     }
     *
     *
     * read:
     *     acquire F_RDLCK
     *     read_account()
     *     release lock immediately
     *
     * begin:
     *     check account_owner[]
     *     acquire F_WRLCK
     *     read current balance
     *     enter TRANSACTION
     *     keep the write lock
     *
     * add:
     *     modify pending_balance only
     *
     * commit:
     *     write_account()
     *     release transaction lock
     *     return to READY
     *
     * abort:
     *     do NOT modify accountRecord
     *     release transaction lock
     *     return to READY
     *
     * exit / invalid command / disconnect:
     *     release any active transaction first
     */

    (void)reqP;
    (void)command;

    return 0;
}


/* ============================================================
 * Parsing helpers
 * ============================================================ */

static int parse_int_strict(const char *text, int *value) {
    if (text == NULL || *text == '\0') {
        return -1;
    }

    errno = 0;

    char *endptr = NULL;
    long result = strtol(text, &endptr, 10);

    if (errno == ERANGE ||
        endptr == text ||
        *endptr != '\0' ||
        result < INT_MIN ||
        result > INT_MAX) {
        return -1;
    }

    *value = (int)result;

    return 0;
}


static int parse_account_id(const char *text, int *account_id) {
    int id;

    if (parse_int_strict(text, &id) < 0) {
        return -1;
    }

    if (id < ACCOUNT_ID_START || id > ACCOUNT_ID_END) {
        return -1;
    }

    *account_id = id;

    return 0;
}


/* ============================================================
 * Account record helpers
 * ============================================================ */

static int account_index(int account_id) {
    return account_id - ACCOUNT_ID_START;
}


static off_t account_offset(int account_id) {
    return (off_t)account_index(account_id) * (off_t)sizeof(account_record);
}


static int read_account(int account_id, account_record *record) {
    ssize_t ret = pread(record_fd, record, sizeof(*record), account_offset(account_id));

    if (ret != (ssize_t)sizeof(*record)) {
        return -1;
    }

    return 0;
}


static int write_account(int account_id, const account_record *record) {
    ssize_t ret = pwrite(record_fd, record, sizeof(*record), account_offset(account_id));

    if (ret != (ssize_t)sizeof(*record)) {
        return -1;
    }

    return 0;
}


/* ============================================================
 * Record locking
 * ============================================================ */

/*
 * Return:
 *   1 : lock successfully acquired
 *   0 : lock conflict
 *  -1 : unexpected error
 */
static int try_record_lock(int account_id, short lock_type) {
    struct flock lock;

    memset(&lock, 0, sizeof(lock));

    lock.l_type = lock_type;
    lock.l_whence = SEEK_SET;
    lock.l_start = account_offset(account_id);
    lock.l_len = sizeof(account_record);

    /*
     * ========================================================
     * TODO 4: Acquire a nonblocking byte-range record lock
     * ========================================================
     *
     * Use:
     *
     *     fcntl(record_fd, F_SETLK, &lock)
     *
     * F_SETLK must be used instead of F_SETLKW.
     *
     * If errno is EACCES or EAGAIN:
     *
     *     return 0
     *
     * Unexpected failure:
     *
     *     return -1
     *
     * Success:
     *
     *     return 1
     */

    return -1;
}


static int unlock_record(int account_id) {
    struct flock lock;

    memset(&lock, 0, sizeof(lock));

    lock.l_type = F_UNLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = account_offset(account_id);
    lock.l_len = sizeof(account_record);

    /*
     * ========================================================
     * TODO 5: Release the byte-range record lock
     * ========================================================
     *
     * Use fcntl() with F_SETLK.
     */

    return -1;
}


/* ============================================================
 * Transaction cleanup
 * ============================================================ */

static void cleanup_transaction(request *reqP) {
    /*
     * ========================================================
     * TODO 6: Transaction cleanup
     * ========================================================
     *
     * If this client is currently in TRANSACTION:
     *
     *     1. find reqP->account_index
     *     2. release its record lock
     *     3. clear account_owner[index]
     *     4. set state back to READY
     *     5. set account_index to -1
     *     6. clear original_balance
     *     7. clear pending_balance
     *
     * This function must work correctly for:
     *
     *     abort
     *     exit
     *     invalid command
     *     disconnect
     *     read/write error
     */

    (void)reqP;
}


/*
 * Common disconnect path for the final select()/poll() implementation.
 */
static void close_client(int fd, fd_set *master_set) {
    request *reqP = &requestP[fd];

    cleanup_transaction(reqP);

    if (master_set != NULL) {
        FD_CLR(fd, master_set);
    }

    close(fd);
    init_request(reqP);
}


/* ============================================================
 * Response helpers
 * ============================================================ */

static void send_locked(int fd) {
    const char *msg = ">>> Locked.\n";
    (void)write_all(fd, msg, strlen(msg));
}


static void send_invalid_command(int fd) {
    const char *msg = ">>> [Error] Invalid command.\n";
    (void)write_all(fd, msg, strlen(msg));
}


/* ============================================================
 * Connection helpers
 * ============================================================ */

static int send_welcome(int fd) {
    if (write_all(fd, welcome_banner, strlen(welcome_banner)) < 0) {
        return -1;
    }

    if (write_all(fd, ready_prompt, strlen(ready_prompt)) < 0) {
        return -1;
    }

    return 0;
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

    /*
     * fd numbers may be reused after close(), so always reset
     * the request state before assigning the new connection.
     */
    init_request(&requestP[conn_fd]);

    requestP[conn_fd].conn_fd = conn_fd;

    snprintf(requestP[conn_fd].host,
             sizeof(requestP[conn_fd].host),
             "%s",
             inet_ntoa(cliaddr.sin_addr));

    fprintf(stderr, "new connection: fd %d from %s\n",
            conn_fd, requestP[conn_fd].host);

    return conn_fd;
}


/* ============================================================
 * Request helpers
 * ============================================================ */

static void init_request(request *reqP) {
    memset(reqP, 0, sizeof(*reqP));

    reqP->conn_fd = -1;
    reqP->state = READY;
    reqP->account_index = -1;
}


static void free_request(request *reqP) {
    cleanup_transaction(reqP);
    init_request(reqP);
}


/* ============================================================
 * Server setup
 * ============================================================ */

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


/* ============================================================
 * Robust write
 * ============================================================ */

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

        if (ret == 0) {
            return -1;
        }

        written += (size_t)ret;
    }

    return (ssize_t)written;
}
