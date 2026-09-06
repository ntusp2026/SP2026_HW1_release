#include "server.h"

#include <signal.h>
#include <limits.h>

const unsigned char IAC_IP[2] = {0xff, 0xf4};

const char *welcome_banner =
    "================================\n"
    " Welcome to CSIE Ledger System \n"
    "================================\n";

const char *ready_prompt =
    "Please enter your command: ";

const char *transaction_prompt =
    "Please enter an operation: ";


/* ============================================================
 * Global server state
 * ============================================================ */

server svr;

request *requestP = NULL;

int maxfd = 0;

int record_fd = -1;


/*
 * account_owner[i]
 *
 * -1  : this server process does not currently own this account
 * >=0 : socket fd of the client that owns this transaction
 *
 * TODO:
 * Students must use this together with fcntl record locks.
 */
static int account_owner[ACCOUNT_NUM];


/* ============================================================
 * Function declarations
 * ============================================================ */

/* ----- server / connection helpers ----- */

static void init_server(unsigned short port);

static void init_request(request *reqP);

static void reset_request(request *reqP);

static int accept_conn(void);

static int send_welcome(int fd);

static ssize_t write_all(
    int fd,
    const void *buf,
    size_t len
);


/* ----- input helpers ----- */

/*
 * TODO:
 * Append newly received bytes into reqP->buf.
 *
 * Return:
 *   1  : bytes received
 *   0  : EOF
 *  -1  : error
 */
static int recv_into_buffer(request *reqP);


/*
 * TODO:
 * Extract one newline-terminated command from reqP->buf.
 *
 * Return:
 *   1 : one command extracted
 *   0 : incomplete command
 *  -1 : malformed / too long
 */
static int pop_command(
    request *reqP,
    char *command,
    size_t command_size
);


/* ----- command handling ----- */

/*
 * TODO:
 * Implement READY / TRANSACTION state machine.
 *
 * Return:
 *   0 : keep connection
 *   1 : close connection
 */
static int handle_command(
    request *reqP,
    const char *command
);


/* ----- parsing helpers (provided) ----- */

static int parse_int_strict(
    const char *text,
    int *value
);

static int parse_account_id(
    const char *text,
    int *account_id
);


/* ----- record helpers (provided) ----- */

static int account_index(int account_id);

static off_t account_offset(int account_id);

static int read_account(
    int account_id,
    account_record *record
);

static int write_account(
    int account_id,
    const account_record *record
);


/* ----- record lock helpers ----- */

/*
 * TODO:
 * Students complete the actual fcntl() operation.
 *
 * Return:
 *   1 : lock acquired
 *   0 : lock conflict
 *  -1 : unexpected error
 */
static int try_record_lock(
    int account_id,
    short lock_type
);


/*
 * TODO:
 * Release one byte-range record lock.
 */
static int unlock_record(int account_id);


/* ----- cleanup helpers ----- */

/*
 * TODO:
 * If reqP is in TRANSACTION:
 *
 *   1. release record lock
 *   2. clear account_owner[]
 *   3. restore READY state
 */
static void cleanup_transaction(request *reqP);


/*
 * Provided common disconnect path.
 */
static void close_client(
    int fd,
    fd_set *master_set
);


/* ----- response helpers ----- */

static void send_locked(int fd);

static void send_invalid_command(int fd);


/* ============================================================
 * main
 * ============================================================ */

int main(int argc, char **argv) {

    if (argc != 2) {
        fprintf(
            stderr,
            "usage: %s [port]\n",
            argv[0]
        );

        return EXIT_FAILURE;
    }


    signal(SIGPIPE, SIG_IGN);


    /* --------------------------------------------------------
     * Open accountRecord once for the entire server lifetime.
     * -------------------------------------------------------- */

    record_fd = open(
        RECORD_PATH,
        O_RDWR
    );

    if (record_fd < 0) {
        ERR_EXIT("open accountRecord");
    }


    /* --------------------------------------------------------
     * Initialize same-process ownership table.
     * -------------------------------------------------------- */

    for (int i = 0; i < ACCOUNT_NUM; ++i) {
        account_owner[i] = -1;
    }


    init_server(
        (unsigned short)atoi(argv[1])
    );


    fprintf(
        stderr,
        "starting on %s, port %u, fd %d, maxconn %d...\n",
        svr.hostname,
        svr.port,
        svr.listen_fd,
        maxfd
    );


    /* ========================================================
     * TODO 1:
     *
     * Implement select() / poll() based I/O multiplexing.
     *
     * Required behavior:
     *
     *   listening socket ready
     *       -> accept new client
     *
     *   client socket ready
     *       -> recv_into_buffer()
     *       -> pop_command()
     *       -> handle_command()
     *
     *   disconnect / error
     *       -> close_client()
     *
     * Do NOT process one client at a time.
     * ======================================================== */


    /*
     * TODO:
     * Replace the following temporary implementation.
     */

    while (1) {

        int conn_fd =
            accept_conn();

        if (conn_fd < 0) {
            continue;
        }


        if (send_welcome(conn_fd) < 0) {

            close(conn_fd);

            reset_request(
                &requestP[conn_fd]
            );

            continue;
        }


        /*
         * Temporary starter behavior.
         *
         * Replace with select()/poll().
         */

        request *reqP =
            &requestP[conn_fd];


        int ret =
            recv_into_buffer(reqP);

        if (ret > 0) {

            char command[MAX_MSG_LEN];

            if (
                pop_command(
                    reqP,
                    command,
                    sizeof(command)
                ) > 0
            ) {

                /*
                 * Temporary debug output.
                 */
                fprintf(
                    stderr,
                    "received: [%s]\n",
                    command
                );
            }
        }


        close(conn_fd);

        reset_request(reqP);
    }


    close(record_fd);

    close(svr.listen_fd);

    free(requestP);

    return EXIT_SUCCESS;
}


/* ============================================================
 * INPUT BUFFER
 * ============================================================ */

static int recv_into_buffer(request *reqP) {

    /*
     * TODO 2:
     *
     * TCP is a byte stream.
     *
     * A command such as:
     *
     *     begin 902001\n
     *
     * may arrive as:
     *
     *     "beg"
     *     "in 902"
     *     "001\n"
     *
     * Append new bytes to reqP->buf.
     *
     * Do NOT overwrite incomplete bytes.
     */

    char incoming[MAX_MSG_LEN];

    ssize_t nread = read(
        reqP->conn_fd,
        incoming,
        sizeof(incoming)
    );


    if (nread < 0) {

        if (errno == EINTR) {
            return 1;
        }

        return -1;
    }


    if (nread == 0) {
        return 0;
    }


    if (
        nread >= 2
        &&
        (unsigned char)incoming[0]
            == IAC_IP[0]
        &&
        (unsigned char)incoming[1]
            == IAC_IP[1]
    ) {
        return 0;
    }


    /*
     * TODO:
     * append incoming bytes into reqP->buf
     */


    return 1;
}


static int pop_command(
    request *reqP,
    char *command,
    size_t command_size
) {

    /*
     * TODO 3:
     *
     * Find the first '\n'.
     *
     * No '\n':
     *     return 0
     *
     * Complete line:
     *
     *     copy command
     *     remove optional '\r'
     *     preserve leftover bytes using memmove()
     *
     * Example:
     *
     *     read 902001\nexit\n
     *
     * first call:
     *
     *     command = "read 902001"
     *
     * reqP->buf becomes:
     *
     *     "exit\n"
     */

    (void)reqP;
    (void)command;
    (void)command_size;

    return 0;
}


/* ============================================================
 * COMMAND HANDLER
 * ============================================================ */

static int handle_command(
    request *reqP,
    const char *command
) {

    /*
     * TODO 4:
     *
     * Implement the state machine:
     *
     *
     * READY
     *
     *     read <account>
     *     begin <account>
     *     exit
     *
     *
     * TRANSACTION
     *
     *     add <delta>
     *     commit
     *     abort
     *     exit
     *
     *
     * Important:
     *
     * begin:
     *     acquire write lock
     *     enter TRANSACTION
     *
     * add:
     *     modify pending_balance only
     *
     * commit:
     *     write_account()
     *     release lock
     *
     * abort:
     *     release lock
     *     do NOT write accountRecord
     */

    (void)reqP;
    (void)command;

    return 0;
}


/* ============================================================
 * PROVIDED PARSING HELPERS
 * ============================================================ */

static int parse_int_strict(
    const char *text,
    int *value
) {

    if (
        text == NULL
        ||
        *text == '\0'
    ) {
        return -1;
    }


    errno = 0;

    char *endptr = NULL;


    long result = strtol(
        text,
        &endptr,
        10
    );


    if (
        errno == ERANGE
        ||
        endptr == text
        ||
        *endptr != '\0'
        ||
        result < INT_MIN
        ||
        result > INT_MAX
    ) {
        return -1;
    }


    *value =
        (int)result;

    return 0;
}


static int parse_account_id(
    const char *text,
    int *account_id
) {

    int id;


    if (
        parse_int_strict(
            text,
            &id
        ) < 0
    ) {
        return -1;
    }


    if (
        id < ACCOUNT_ID_START
        ||
        id > ACCOUNT_ID_END
    ) {
        return -1;
    }


    *account_id = id;

    return 0;
}


/* ============================================================
 * PROVIDED RECORD HELPERS
 * ============================================================ */

static int account_index(
    int account_id
) {

    return
        account_id
        - ACCOUNT_ID_START;
}


static off_t account_offset(
    int account_id
) {

    return
        (off_t)account_index(account_id)
        *
        (off_t)sizeof(account_record);
}


static int read_account(
    int account_id,
    account_record *record
) {

    ssize_t ret = pread(
        record_fd,
        record,
        sizeof(*record),
        account_offset(account_id)
    );


    if (
        ret
        != (ssize_t)sizeof(*record)
    ) {
        return -1;
    }


    return 0;
}


static int write_account(
    int account_id,
    const account_record *record
) {

    ssize_t ret = pwrite(
        record_fd,
        record,
        sizeof(*record),
        account_offset(account_id)
    );


    if (
        ret
        != (ssize_t)sizeof(*record)
    ) {
        return -1;
    }


    return 0;
}


/* ============================================================
 * RECORD LOCKS
 * ============================================================ */

static int try_record_lock(
    int account_id,
    short lock_type
) {

    struct flock lock;


    memset(
        &lock,
        0,
        sizeof(lock)
    );


    /*
     * We prepare the correct byte range for students.
     *
     * They still need to understand:
     *
     * F_RDLCK
     * F_WRLCK
     * F_SETLK
     * EACCES / EAGAIN
     */

    lock.l_type =
        lock_type;

    lock.l_whence =
        SEEK_SET;

    lock.l_start =
        account_offset(account_id);

    lock.l_len =
        sizeof(account_record);


    /*
     * TODO 5:
     *
     * Use fcntl(..., F_SETLK, ...)
     *
     * Return:
     *
     *   1 = success
     *   0 = conflict
     *  -1 = unexpected error
     */


    return -1;
}


static int unlock_record(
    int account_id
) {

    struct flock lock;


    memset(
        &lock,
        0,
        sizeof(lock)
    );


    lock.l_type =
        F_UNLCK;

    lock.l_whence =
        SEEK_SET;

    lock.l_start =
        account_offset(account_id);

    lock.l_len =
        sizeof(account_record);


    /*
     * TODO 6:
     * Release this record lock using fcntl().
     */


    return -1;
}


/* ============================================================
 * TRANSACTION CLEANUP
 * ============================================================ */

static void cleanup_transaction(
    request *reqP
) {

    /*
     * TODO 7:
     *
     * If reqP owns an active transaction:
     *
     *   unlock account
     *   account_owner[index] = -1
     *   state = READY
     *   account_index = -1
     *   clear balances
     */

    (void)reqP;
}


/* ============================================================
 * COMMON DISCONNECT PATH
 * ============================================================ */

static void close_client(
    int fd,
    fd_set *master_set
) {

    request *reqP =
        &requestP[fd];


    /*
     * Student cleanup logic is centralized here.
     */
    cleanup_transaction(reqP);


    if (master_set != NULL) {

        FD_CLR(
            fd,
            master_set
        );
    }


    close(fd);

    reset_request(reqP);
}


/* ============================================================
 * RESPONSE HELPERS
 * ============================================================ */

static void send_locked(
    int fd
) {

    const char *msg =
        ">>> Locked.\n";


    (void)write_all(
        fd,
        msg,
        strlen(msg)
    );
}


static void send_invalid_command(
    int fd
) {

    const char *msg =
        ">>> [Error] Invalid command.\n";


    (void)write_all(
        fd,
        msg,
        strlen(msg)
    );
}


/* ============================================================
 * CONNECTION HELPERS
 * ============================================================ */

static int send_welcome(
    int fd
) {

    if (
        write_all(
            fd,
            welcome_banner,
            strlen(welcome_banner)
        ) < 0
    ) {
        return -1;
    }


    if (
        write_all(
            fd,
            ready_prompt,
            strlen(ready_prompt)
        ) < 0
    ) {
        return -1;
    }


    return 0;
}


static int accept_conn(void) {

    struct sockaddr_in cliaddr;

    socklen_t clilen =
        sizeof(cliaddr);


    int conn_fd = accept(
        svr.listen_fd,
        (struct sockaddr *)&cliaddr,
        &clilen
    );


    if (conn_fd < 0) {

        if (
            errno == EINTR
            ||
            errno == EAGAIN
        ) {
            return -1;
        }

        ERR_EXIT("accept");
    }


    if (conn_fd >= maxfd) {

        close(conn_fd);

        return -1;
    }


    requestP[conn_fd].conn_fd =
        conn_fd;


    snprintf(
        requestP[conn_fd].host,
        sizeof(
            requestP[conn_fd].host
        ),
        "%s",
        inet_ntoa(
            cliaddr.sin_addr
        )
    );


    fprintf(
        stderr,
        "new connection: fd %d from %s\n",
        conn_fd,
        requestP[conn_fd].host
    );


    return conn_fd;
}


/* ============================================================
 * REQUEST HELPERS
 * ============================================================ */

static void init_request(
    request *reqP
) {

    memset(
        reqP,
        0,
        sizeof(*reqP)
    );


    reqP->conn_fd =
        -1;

    reqP->state =
        READY;

    reqP->account_index =
        -1;
}


static void reset_request(
    request *reqP
) {

    init_request(reqP);
}


/* ============================================================
 * SERVER SETUP
 * ============================================================ */

static void init_server(
    unsigned short port
) {

    struct sockaddr_in servaddr;

    int reuse = 1;


    if (
        gethostname(
            svr.hostname,
            sizeof(svr.hostname)
        ) < 0
    ) {
        ERR_EXIT("gethostname");
    }


    svr.port =
        port;


    svr.listen_fd = socket(
        AF_INET,
        SOCK_STREAM,
        0
    );


    if (svr.listen_fd < 0) {
        ERR_EXIT("socket");
    }


    if (
        setsockopt(
            svr.listen_fd,
            SOL_SOCKET,
            SO_REUSEADDR,
            &reuse,
            sizeof(reuse)
        ) < 0
    ) {
        ERR_EXIT("setsockopt");
    }


    memset(
        &servaddr,
        0,
        sizeof(servaddr)
    );


    servaddr.sin_family =
        AF_INET;

    servaddr.sin_addr.s_addr =
        htonl(INADDR_ANY);

    servaddr.sin_port =
        htons(port);


    if (
        bind(
            svr.listen_fd,
            (struct sockaddr *)&servaddr,
            sizeof(servaddr)
        ) < 0
    ) {
        ERR_EXIT("bind");
    }


    if (
        listen(
            svr.listen_fd,
            1024
        ) < 0
    ) {
        ERR_EXIT("listen");
    }


    maxfd =
        getdtablesize();


    requestP = calloc(
        (size_t)maxfd,
        sizeof(*requestP)
    );


    if (requestP == NULL) {
        ERR_EXIT(
            "calloc request table"
        );
    }


    for (
        int i = 0;
        i < maxfd;
        ++i
    ) {

        init_request(
            &requestP[i]
        );
    }


    requestP[
        svr.listen_fd
    ].conn_fd =
        svr.listen_fd;
}


/* ============================================================
 * ROBUST WRITE
 * ============================================================ */

static ssize_t write_all(
    int fd,
    const void *buf,
    size_t len
) {

    const char *ptr =
        buf;

    size_t written =
        0;


    while (written < len) {

        ssize_t ret = write(
            fd,
            ptr + written,
            len - written
        );


        if (ret < 0) {

            if (errno == EINTR) {
                continue;
            }

            return -1;
        }


        if (ret == 0) {
            return -1;
        }


        written +=
            (size_t)ret;
    }


    return (ssize_t)written;
}
