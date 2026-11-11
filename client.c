/*
 * client.c - FIFO based client-server: CLIENT
 *
 * Usage: ./client <client_id>      e.g.  ./client C101
 *
 * - Creates a private FIFO /tmp/os_<id>_fifo for receiving responses
 * - Sends Request structs to the server's well-known FIFO
 * - Waits for the response (with a 5 second timeout, so it never hangs)
 * - Removes its private FIFO on exit (EXIT, Ctrl+D, or Ctrl+C)
 *
 * Build: gcc -Wall -Wextra -o client client.c
 */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "common.h"

#define TIMEOUT_MS 5000

static char my_fifo[PATH_LEN];

/* Ctrl+C: remove our FIFO then quit (unlink/_exit are async-signal-safe) */
static void on_signal(int sig)
{
    (void)sig;
    if (my_fifo[0]) unlink(my_fifo);
    _exit(130);
}

static int valid_id(const char *id)
{
    size_t len = strlen(id);
    if (len == 0 || len >= ID_LEN - 1) return 0;
    for (size_t i = 0; i < len; i++)
        if (!isalnum((unsigned char)id[i]) && id[i] != '_') return 0;
    return 1;
}

static int send_request(int sfd, const char *id, const char *cmd, const char *args)
{
    Request req;
    memset(&req, 0, sizeof req);
    snprintf(req.client_id, sizeof req.client_id, "%s", id);
    snprintf(req.fifo_path, sizeof req.fifo_path, "%s", my_fifo);
    snprintf(req.command, sizeof req.command, "%s", cmd);
    snprintf(req.args, sizeof req.args, "%s", args);

    /* one atomic write() (size < PIPE_BUF) -> never mixed with other clients */
    if (write(sfd, &req, sizeof req) != (ssize_t)sizeof req) {
        if (errno == EPIPE) fprintf(stderr, "Server has gone away.\n");
        else perror("write to server");
        return -1;
    }
    return 0;
}

/* returns 0 on success, -1 on timeout/error */
static int wait_response(int rfd, Response *resp)
{
    struct pollfd pfd = { .fd = rfd, .events = POLLIN };
    int r = poll(&pfd, 1, TIMEOUT_MS);
    if (r == 0) {
        fprintf(stderr, "Timeout: no response from server within %d s.\n",
                TIMEOUT_MS / 1000);
        return -1;
    }
    if (r < 0) { perror("poll"); return -1; }
    if (read_full(rfd, resp, sizeof *resp) != (ssize_t)sizeof *resp) {
        fprintf(stderr, "Incomplete response from server.\n");
        return -1;
    }
    resp->text[RESP_LEN - 1] = '\0';
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <client_id>   (letters, digits, _ only)\n", argv[0]);
        return 1;
    }
    const char *id = argv[1];
    if (!valid_id(id)) {
        fprintf(stderr, "Invalid client id '%s'\n", id);
        return 1;
    }
    snprintf(my_fifo, sizeof my_fifo, "/tmp/os_%s_fifo", id);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* 1. Create private response FIFO */
    if (mkfifo(my_fifo, 0666) == -1) {
        if (errno == EEXIST)
            fprintf(stderr, "Client id '%s' is already in use (or a stale FIFO is left).\n"
                            "If no such client is running: rm %s\n", id, my_fifo);
        else
            perror("mkfifo");
        my_fifo[0] = '\0';      /* not ours - don't unlink it on exit */
        return 1;
    }
    chmod(my_fifo, 0666);

    /* O_RDWR: we are reader AND (dummy) writer on our own FIFO. Because a
     * reader exists as long as we are alive, the server's non-blocking open
     * succeeds; when we die it fails with ENXIO -> server detects disconnect. */
    int rfd = open(my_fifo, O_RDWR);
    if (rfd == -1) { perror("open private fifo"); unlink(my_fifo); return 1; }

    /* 2. Open the server FIFO. Non-blocking so we get an error (ENXIO)
     * instead of hanging forever if the server is not running. */
    int sfd = open(SERVER_FIFO, O_WRONLY | O_NONBLOCK);
    if (sfd == -1) {
        if (errno == ENXIO || errno == ENOENT)
            fprintf(stderr, "Server is not running (start ./server first).\n");
        else
            perror("open server fifo");
        close(rfd);
        unlink(my_fifo);
        return 1;
    }
    fcntl(sfd, F_SETFL, fcntl(sfd, F_GETFL) & ~O_NONBLOCK);

    /* 3. Register with the server */
    Response resp;
    if (send_request(sfd, id, "CONNECT", "") == 0 && wait_response(rfd, &resp) == 0)
        printf("%s\n", resp.text);
    else {
        close(sfd); close(rfd); unlink(my_fifo);
        return 1;
    }

    /* 4. Interactive loop */
    char line[ARGS_LEN + CMD_LEN + 8];
    for (;;) {
        printf("%s> ", id);
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) {     /* Ctrl+D / EOF */
            printf("\n");
            send_request(sfd, id, "EXIT", "");
            wait_response(rfd, &resp);
            break;
        }
        line[strcspn(line, "\r\n")] = '\0';

        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '\0') continue;                   /* empty input */

        char *cmd = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) *p++ = '\0';
        while (isspace((unsigned char)*p)) p++;
        char *args = p;

        if (strlen(cmd) >= CMD_LEN || strlen(args) >= ARGS_LEN) {
            fprintf(stderr, "Input too long.\n");
            continue;
        }

        if (send_request(sfd, id, cmd, args) == -1) break;
        if (wait_response(rfd, &resp) == -1) {
            if (strcasecmp(cmd, "EXIT") == 0) break;
            continue;                               /* timeout: let user retry */
        }
        printf("%s\n", resp.text);
        if (resp.status == STATUS_BYE) break;
    }

    /* 5. Cleanup */
    close(sfd);
    close(rfd);
    unlink(my_fifo);
    return 0;
}
