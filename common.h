/*
 * common.h - definitions shared by server.c and client.c
 *
 * Request/response format (fixed-size C structs, as allowed by the project):
 *   Request  = { client_id, fifo_path, command, args }
 *   Response = { status, text }
 *
 * Why fixed-size structs?
 *   POSIX guarantees that a write() of <= PIPE_BUF bytes (4096 on Linux) to a
 *   pipe/FIFO is ATOMIC. Both structs are far smaller than PIPE_BUF, so when
 *   many clients write to the well-known FIFO at the same time, their requests
 *   are never interleaved/mixed. The server simply reads one whole struct at a
 *   time.
 */
#ifndef COMMON_H
#define COMMON_H

#include <errno.h>
#include <unistd.h>

#define SERVER_FIFO "/tmp/os_server_fifo"   /* well-known FIFO */

#define ID_LEN    32
#define PATH_LEN  128
#define CMD_LEN   16
#define ARGS_LEN  256
#define RESP_LEN  512

/* Response status codes */
#define STATUS_OK    0
#define STATUS_ERROR 1
#define STATUS_BYE   2   /* server acknowledges EXIT */

typedef struct {
    char client_id[ID_LEN];     /* e.g. "C101"                      */
    char fifo_path[PATH_LEN];   /* e.g. "/tmp/os_C101_fifo"         */
    char command[CMD_LEN];      /* e.g. "ADD"                       */
    char args[ARGS_LEN];        /* e.g. "10 20"                     */
} Request;

typedef struct {
    int  status;
    char text[RESP_LEN];
} Response;

/*
 * read_full - read exactly 'size' bytes (a pipe may deliver partial data).
 * Returns bytes read (== size), 0/less on EOF, or -1 on error.
 * If interrupted by a signal before any byte arrived, returns -1 with
 * errno == EINTR so the caller can check its shutdown flag.
 */
static inline ssize_t read_full(int fd, void *buf, size_t size)
{
    size_t got = 0;
    while (got < size) {
        ssize_t n = read(fd, (char *)buf + got, size - got);
        if (n > 0) {
            got += (size_t)n;
        } else if (n == 0) {
            return (ssize_t)got;            /* EOF */
        } else if (errno == EINTR) {
            if (got == 0) return -1;        /* let caller check flags */
            continue;
        } else {
            return -1;
        }
    }
    return (ssize_t)got;
}

#endif /* COMMON_H */
