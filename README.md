# Client–Server Communication Using Named Pipes (FIFOs)

Operating Systems semester project. A C client–server application on Linux
where multiple clients talk to one server using named pipes.

## Files

| File | Purpose |
|------|---------|
| `common.h` | Shared constants, `Request` / `Response` structs, `read_full()` helper |
| `server.c` | Server process |
| `client.c` | Client process |
| `Makefile` | Build, test and clean targets |
| `test.sh`  | Automated tests for the testing checklist |

## Compile

```bash
make            # builds ./server and ./client
```

## Run

Open one terminal for the server and at least three for clients:

```bash
# Terminal 1
./server

# Terminals 2, 3, 4 ...
./client C101
./client C102
./client C103
```

Stop the server with `Ctrl+C` (it removes its FIFO and exits cleanly).

## Supported commands

| Command | Description |
|---------|-------------|
| `TIME` | Returns the current server time |
| `ECHO <message>` | Returns the message |
| `ADD <a> <b>` | Returns the sum of two numbers |
| `HELP` | Lists the commands |
| `EXIT` | Closes the client session (server keeps running) |
| `STATS` | *(extra)* Active clients, total requests, server uptime |

Commands are case-insensitive. Invalid commands and malformed input return an
error message.

## Run the automated tests

```bash
chmod +x test.sh
make test       # or: bash test.sh
```

## Design summary

* **Well-known FIFO:** `/tmp/os_server_fifo` – all clients write requests here.
* **Private FIFO per client:** `/tmp/os_<id>_fifo` – the server writes that client's responses here, so responses are never mixed up.
* **Request format:** fixed-size C struct `{client_id, fifo_path, command, args}` (see `common.h`). Logical form: `C101|/tmp/os_C101_fifo|ADD|10 20`.
* **Concurrency safety:** each request/response is one `write()` smaller than `PIPE_BUF` (4096 bytes), which POSIX guarantees to be atomic – requests from different clients can't interleave.
* **No hanging:**
  * Server keeps a dummy writer open on its own FIFO so `read()` doesn't return EOF when the last client leaves.
  * Server opens client FIFOs with `O_NONBLOCK`; a dead client gives `ENXIO` and is simply removed.
  * Client opens the server FIFO with `O_NONBLOCK`, so it prints "server not running" instead of blocking.
  * Client waits for replies with `poll()` and a 5 s timeout.
* **Cleanup:** `SIGINT`/`SIGTERM` handled with `sigaction()`; `unlink()` removes FIFOs on exit. `SIGPIPE` is ignored.
* **Extras implemented:** `STATS` command, server activity log (`os_server.log`), client timeout, active-client tracking.

## Assumptions and limitations

* Linux only (uses `mkfifo`, `poll`, `sigaction`).
* The server is single-process and handles requests one by one (simple and safe; no `fork()`).
* Client IDs must be unique, using letters, digits and `_` only. If a client is killed with `kill -9`, remove its leftover FIFO: `rm /tmp/os_<id>_fifo`.
* Maximum 64 clients are tracked at once.
