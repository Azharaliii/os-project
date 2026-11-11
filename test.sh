#!/bin/bash
# test.sh - automated checks for the FIFO client-server project
# Run:  make test      (or ./test.sh after compiling)

cd "$(dirname "$0")" || exit 1
PASS=0; FAIL=0
check() {   # check "description" "command that returns 0 on success"
    if eval "$2"; then echo "  [PASS] $1"; PASS=$((PASS+1));
    else echo "  [FAIL] $1"; FAIL=$((FAIL+1)); fi
}

rm -f /tmp/os_server_fifo /tmp/os_*_fifo os_server.log
./server > /tmp/os_server_out.txt 2>&1 &
SPID=$!
sleep 1

echo "== Basic tests =="
check "Server creates the well-known FIFO" "[ -p /tmp/os_server_fifo ]"

OUT=$(printf 'ADD 10 20\nECHO Hello\nTIME\nHELP\nFOO\nADD 1\nECHO\nEXIT\n' | ./client C101 2>&1)
check "ADD 10 20 returns 30"          "echo \"\$OUT\" | grep -q 'Server response: 30'"
check "ECHO Hello returns Hello"      "echo \"\$OUT\" | grep -qx 'C101> Hello'"
check "TIME returns server time"      "echo \"\$OUT\" | grep -q 'Server time:'"
check "HELP lists commands"           "echo \"\$OUT\" | grep -q 'Available commands'"
check "Invalid command -> error"      "echo \"\$OUT\" | grep -q \"unknown command 'FOO'\""
check "Malformed ADD -> error"        "echo \"\$OUT\" | grep -q 'Usage: ADD'"
check "ECHO without text -> error"    "echo \"\$OUT\" | grep -q 'ECHO needs a message'"
check "Client private FIFO removed after EXIT" "[ ! -e /tmp/os_C101_fifo ]"
check "Server still running after client exit" "kill -0 $SPID 2>/dev/null"

echo "== Multi-client tests (3 clients at the same time) =="
for c in C201 C202 C203; do
    ( for i in 1 2 3 4 5; do echo "ECHO ${c}_msg$i"; echo "ADD $i ${c#C}"; done; echo EXIT ) \
        | ./client $c > /tmp/os_out_$c.txt 2>&1 &
done
wait $(jobs -p | grep -v "^$SPID$") 2>/dev/null
sleep 2
for c in C201 C202 C203; do
    check "$c got its own 5 echoes"  "[ \$(grep -c \"${c}_msg\" /tmp/os_out_$c.txt) -eq 5 ]"
    others=$(echo C201 C202 C203 | tr ' ' '\n' | grep -v $c | tr '\n' '|' | sed 's/|$//')
    check "$c got NO other client's data" "! grep -Eq \"${others}_msg\" /tmp/os_out_$c.txt"
done

echo "== Unexpected disconnection =="
# client killed with SIGKILL (cannot clean up) -> its FIFO is left behind
( sleep 30 ) | ./client C301 > /dev/null 2>&1 &
sleep 1
CPID=$(pgrep -f "./client C301" | head -1)
kill -9 "$CPID" 2>/dev/null
sleep 0.5
# another client keeps working, server must not hang
OUT=$(printf 'ADD 2 3\nSTATS\nEXIT\n' | ./client C302 2>&1)
check "Server alive & serving after a client was killed" "echo \"\$OUT\" | grep -q 'Server response: 5'"
rm -f /tmp/os_C301_fifo

echo "== Server shutdown =="
kill -INT $SPID
sleep 1
check "Server stopped on SIGINT"          "! kill -0 $SPID 2>/dev/null"
check "Well-known FIFO removed on shutdown" "[ ! -e /tmp/os_server_fifo ]"
OUT=$(echo "TIME" | ./client C401 2>&1)
check "Client reports 'server not running' (no hang)" "echo \"\$OUT\" | grep -q 'not running'"

rm -f /tmp/os_out_C*.txt /tmp/os_server_out.txt
echo; echo "Result: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
