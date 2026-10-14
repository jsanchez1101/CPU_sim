#!/usr/bin/env bash
# check_cli.sh -- the command line must reject bad values instead of guessing.
#
# Each case runs cpusim with one bad option and expects exit status 2 and an
# "error:" message, never a crash or a silent fallback to some default.
set -u

BIN=${BIN:-./cpusim}
PROG=programs/fib.asm
failures=0
count=0

expect_reject() {
  count=$((count + 1))
  local out status
  out=$("$BIN" "$@" "$PROG" 2>&1 >/dev/null)
  status=$?
  if [ "$status" -ne 2 ] || ! grep -q '^error:' <<<"$out"; then
    echo "[ FAIL ] cpusim $* -> exit $status: ${out:-<no message>}"
    failures=$((failures + 1))
  fi
}

expect_reject --quantum abc
expect_reject --quantum 0
expect_reject --quantum -1
expect_reject --quantum 10x
expect_reject --quantum 99999999999999999999999
expect_reject --frames 0
expect_reject --frames 4294967295
expect_reject --line 0
expect_reject --line 3
expect_reject --line 8192
expect_reject --tlb 100000
expect_reject --icache 0x2
expect_reject --icache 65536x65536
expect_reject --dcache 4x
expect_reject --priority 1.5
expect_reject --max-cycles 0
expect_reject --dump-mem 0x2000:abc
expect_reject --no-such-option

count=$((count + 1))
if ! "$BIN" --frames 1 --line 4096 --tlb 0 "$PROG" >/dev/null 2>&1; then
  echo "[ FAIL ] smallest valid configuration was rejected"
  failures=$((failures + 1))
fi

echo "$count CLI checks, $failures failures"
[ "$failures" -eq 0 ]
