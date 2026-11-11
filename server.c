/*
 * server.c - FIFO based client-server: SERVER
 *
 * - Creates the well-known FIFO (/tmp/os_server_fifo)
 * - Reads fixed-size Request structs from it, one at a time
 * - Processes TIME, ECHO, ADD, HELP, EXIT (+ CONNECT, STATS)
 * - Writes the Response into the client's private FIFO
 * - Runs until Ctrl+C / SIGTERM, then cleans up and removes the FIFO
 *
 * Build: gcc -Wall -Wextra -o server server.c
 */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#include "common.h"

#define MAX_CLIENTS 64
#define LOG_FILE    "os_server.log"

static volatile sig_atomic_t running = 1;   /* cleared by signal handler */
static FILE *logfile = NULL;

static char clients[MAX_CLIENTS][ID_LEN];   /* currently connected clients */
static int  n_clients = 0;
static unsigned long total_requests = 0;
static time_t start_time;

/* ---------- signals ---------- */
static void on_signal(int sig)
{
    (void)sig;
    running = 0;    /* interrupts the blocking read() -> loop ends */
}

/* ---------- logging (screen + file) ---------- */
static void logmsg(const char *fmt, ...)
{
    char ts[32];
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);

    va_list ap;
    va_start(ap, fmt);
    printf("[%s] ", ts);
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
    va_end(ap);

    if (logfile) {
        va_start(ap, fmt);
        fprintf(logfile, "[%s] ", ts);
        vfprintf(logfile, fmt, ap);
        fprintf(logfile, "\n");
        fflush(logfile);
        va_end(ap);
    }
}

/* ---------- active client table ---------- */
static int find_client(const char *id)
{
    for (int i = 0; i < n_clients; i++)
        if (strcmp(clients[i], id) == 0) return i;
    return -1;
}

static void add_client(const char *id)
{
    if (find_client(id) >= 0 || n_clients >= MAX_CLIENTS) return;
    snprintf(clients[n_clients++], ID_LEN, "%s", id);
}

static void remove_client(const char *id)
{
    int i = find_client(id);
    if (i < 0) return;
    memmove(clients[i], clients[i + 1], (size_t)(n_clients - i - 1) * ID_LEN);
    n_clients--;
}

/* ---------- send a response to ONE client's private FIFO ---------- */
static void send_response(const Request *req, int status, const char *text)
{
    /* Safety: only write to something that really is a FIFO in /tmp */
    struct stat st;
    if (strncmp(req->fifo_path, "/tmp/", 5) != 0 ||
        stat(req->fifo_path, &st) == -1 || !S_ISFIFO(st.st_mode)) {
        logmsg("Client %s: invalid/missing response FIFO '%s' - dropped",
               req->client_id, req->fifo_path);
        remove_client(req->client_id);
        return;
    }

    /* O_NONBLOCK: if the client died (no reader), open() fails with ENXIO
     * immediately instead of blocking the whole server forever. */
    int fd = open(req->fifo_path, O_WRONLY | O_NONBLOCK);
    if (fd == -1) {
        logmsg("Client %s disconnected unexpectedly (%s) - removed",
               req->client_id, strerror(errno));
        remove_client(req->client_id);
        return;
    }

    Response resp;
    memset(&resp, 0, sizeof resp);
    resp.status = status;
    snprintf(resp.text, sizeof resp.text, "%s", text);

    if (write(fd, &resp, sizeof resp) == -1) {
        logmsg("Client %s: write failed (%s) - removed",
               req->client_id, strerror(errno));
        remove_client(req->client_id);
    }
    close(fd);
}

/* ---------- command handling ---------- */
static int parse_two_numbers(const char *s, double *a, double *b)
{
    char *end;
    *a = strtod(s, &end);
    if (end == s) return 0;
    const char *p = end;
    *b = strtod(p, &end);
    if (end == p) return 0;
    while (isspace((unsigned char)*end)) end++;
    return *end == '\0';            /* reject extra junk like "1 2 3" */
}

static void handle_request(Request *req)
{
    char out[RESP_LEN];

    /* make sure all strings are terminated, whatever the client sent */
    req->client_id[ID_LEN - 1]  = '\0';
    req->fifo_path[PATH_LEN - 1] = '\0';
    req->command[CMD_LEN - 1]   = '\0';
    req->args[ARGS_LEN - 1]     = '\0';

    for (char *p = req->command; *p; p++)
        *p = (char)toupper((unsigned char)*p);

    total_requests++;
    logmsg("Request from %s: %s %s", req->client_id, req->command, req->args);

    if (strcmp(req->command, "CONNECT") == 0) {
        add_client(req->client_id);
        snprintf(out, sizeof out,
                 "Welcome %s! Type HELP to see available commands.",
                 req->client_id);
        send_response(req, STATUS_OK, out);

    } else if (strcmp(req->command, "TIME") == 0) {
        time_t now = time(NULL);
        struct tm tmv;
        char ts[64];
        localtime_r(&now, &tmv);
        strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);
        snprintf(out, sizeof out, "Server time: %s", ts);
        send_response(req, STATUS_OK, out);

    } else if (strcmp(req->command, "ECHO") == 0) {
        if (req->args[0] == '\0')
            send_response(req, STATUS_ERROR, "Error: ECHO needs a message. Usage: ECHO <message>");
        else
            send_response(req, STATUS_OK, req->args);

    } else if (strcmp(req->command, "ADD") == 0) {
        double a, b;
        if (!parse_two_numbers(req->args, &a, &b)) {
            send_response(req, STATUS_ERROR, "Error: invalid input. Usage: ADD <a> <b>");
        } else {
            snprintf(out, sizeof out, "Server response: %.10g", a + b);
            send_response(req, STATUS_OK, out);
        }

    } else if (strcmp(req->command, "HELP") == 0) {
        send_response(req, STATUS_OK,
            "Available commands:\n"
            "  TIME          - current server time\n"
            "  ECHO <msg>    - server returns your message\n"
            "  ADD <a> <b>   - sum of two numbers\n"
            "  STATS         - active clients and total requests\n"
            "  HELP          - show this help\n"
            "  EXIT          - close your session");

    } else if (strcmp(req->command, "STATS") == 0) {
        snprintf(out, sizeof out,
                 "Active clients: %d | Total requests: %lu | Uptime: %ld s",
                 n_clients, total_requests, (long)(time(NULL) - start_time));
        send_response(req, STATUS_OK, out);

    } else if (strcmp(req->command, "EXIT") == 0) {
        remove_client(req->client_id);
        send_response(req, STATUS_BYE, "Goodbye!");

    } else {
        snprintf(out, sizeof out,
                 "Error: unknown command '%s'. Type HELP for the list.",
                 req->command);
        send_response(req, STATUS_ERROR, out);
    }
}

/* ---------- main ---------- */
int main(void)
{
    start_time = time(NULL);
    logfile = fopen(LOG_FILE, "a");     /* optional activity log */

    /* Signals: no SA_RESTART so read() returns EINTR on Ctrl+C.
     * SIGPIPE ignored so writing to a dead client gives EPIPE, not a crash. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* 1. Create the well-known FIFO */
    if (mkfifo(SERVER_FIFO, 0666) == -1) {
        struct stat st;
        if (errno != EEXIST) {
            perror("mkfifo");
            return 1;
        }
        if (stat(SERVER_FIFO, &st) == -1 || !S_ISFIFO(st.st_mode)) {
            fprintf(stderr, "%s exists but is not a FIFO\n", SERVER_FIFO);
            return 1;
        }
        logmsg("FIFO %s already exists (old run?) - reusing it", SERVER_FIFO);
    }
    chmod(SERVER_FIFO, 0666);   /* mkfifo mode is reduced by umask */

    /* 2. Open it. Opening O_RDONLY normally blocks until a writer appears,
     * so open non-blocking first, then open a DUMMY writer. The dummy writer
     * keeps the FIFO "alive": without it, read() would return 0 (EOF) every
     * time the last client closes its end. */
    int rfd = open(SERVER_FIFO, O_RDONLY | O_NONBLOCK);
    if (rfd == -1) { perror("open server fifo (read)"); unlink(SERVER_FIFO); return 1; }
    int dummy_wfd = open(SERVER_FIFO, O_WRONLY);
    if (dummy_wfd == -1) { perror("open server fifo (dummy write)"); close(rfd); unlink(SERVER_FIFO); return 1; }
    fcntl(rfd, F_SETFL, fcntl(rfd, F_GETFL) & ~O_NONBLOCK);  /* back to blocking reads */

    logmsg("Server started. Waiting for requests on %s (Ctrl+C to stop)", SERVER_FIFO);

    /* 3. Main loop: read one Request at a time, process, respond */
    Request req;
    while (running) {
        ssize_t n = read_full(rfd, &req, sizeof req);
        if (n < 0) {
            if (errno == EINTR) continue;       /* signal: re-check 'running' */
            perror("read");
            break;
        }
        if ((size_t)n != sizeof req) break;     /* EOF (should not happen) */
        handle_request(&req);
    }

    /* 4. Clean shutdown */
    logmsg("Shutting down. Total requests processed: %lu", total_requests);
    close(rfd);
    close(dummy_wfd);
    unlink(SERVER_FIFO);
    if (logfile) fclose(logfile);
    return 0;
}
