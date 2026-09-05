#ifndef SERVER_H
#define SERVER_H

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* Feel free to edit any part of this file. */

#define ERR_EXIT(msg)          \
    do {                       \
        perror(msg);           \
        exit(EXIT_FAILURE);    \
    } while (0)

#define ACCOUNT_NUM 20
#define ACCOUNT_ID_START 902001
#define ACCOUNT_ID_END 902020
#define MAX_BALANCE 1000000
#define MAX_MSG_LEN 512
#define RECORD_PATH "./accountRecord"

typedef struct {
    int id;
    int balance;
} account_record;

enum client_state {
    READY,
    TRANSACTION
};

typedef struct {
    char hostname[512];
    unsigned short port;
    int listen_fd;
} server;

typedef struct {
    char host[512];
    int conn_fd;
    char buf[MAX_MSG_LEN];
    size_t buf_len;

    enum client_state state;
    int account_index;
    int original_balance;
    int pending_balance;
} request;

extern server svr;
extern request *requestP;
extern int maxfd;
extern int record_fd;

#endif
