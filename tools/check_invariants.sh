#!/usr/bin/env bash
#
# check_invariants.sh -- the master correctness property of the simulator:
#
#   A program's RESULT must not depend on microarchitecture.
#   Only its CYCLE COUNT may.
#
# Forwarding, branch prediction, cache geometry, TLB size and the number of
# physical frames are all performance features. If flipping any of them changes
# what a program computes, that is a bug, and the flag that disagrees names the
# subsystem that owns it. This is the first thing to run when a result looks
# wrong and the fastest way to localise a regression.
#
#   ./tools/check_invariants.sh            # or: make invariants
#
set -u

BIN="${BIN:-./cpusim}"
PROGRAMS="${*:-programs/vector_add.asm programs/fib.asm programs/hazard_chain.asm programs/matmul.asm programs/pointer_chase.asm programs/spinner.asm programs/yielder.asm}"

# Configurations that must not change the answer. Each stresses a different
# part of the machine.
CONFIGS=(
  ""                                  # baseline
  "--no-forwarding"                   # bypass network off, more stalls
  "--no-predictor"                    # static predict-not-taken
  "--frames 2"                        # page thrashing with eviction + writeback
  "--dcache 4x1"                      # tiny direct-mapped D-cache
  "--icache 4x1"                      # tiny direct-mapped I-cache
  "--line 128"                        # coarse lines
  "--tlb 2"                           # constant TLB pressure
  "--miss-penalty 0 --fault-penalty 0"  # all memory latency removed
  "--prefault"                        # warm start, no compulsory faults
  "--prefault --frames 2"             # warm start that cannot fit, falls back
  "--quantum 7"                       # preempt constantly, mid-pipeline
  "--sched mlfq --quantum 13"          # different scheduler and quantum
)

if [ ! -x "$BIN" ]; then
  echo "error: $BIN not found -- run make first" >&2
  exit 2
fi

fail=0
checks=0

for prog in $PROGRAMS; do
  base=$("$BIN" "$prog" 2>&1 | sed -n 's/.*output: //p')
  if [ -z "$base" ]; then
    echo "  SKIP $prog (produces no output to compare)"
    continue
  fi
  bad=""
  for cfg in "${CONFIGS[@]}"; do
    checks=$((checks + 1))
    got=$("$BIN" $cfg "$prog" 2>&1 | sed -n 's/.*output: //p')
    if [ "$got" != "$base" ]; then
      bad="$bad\n      [$cfg] produced '$got', expected '$base'"
      fail=$((fail + 1))
    fi
  done
  name=$(basename "$prog")
  if [ -z "$bad" ]; then
    printf "  ok    %-22s %-8s stable across %d configurations\n" "$name" "$base" "${#CONFIGS[@]}"
  else
    printf "  FAIL  %-22s %s\n" "$name" "$base"
    printf "$bad\n"
  fi
done

echo
echo "$checks comparisons, $fail failures"
[ "$fail" -eq 0 ] || exit 1
