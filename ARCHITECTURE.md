# How cpusim works, and how to debug it

Written for the person holding the debugger. The README says what the simulator
does; this says how it is put together, which invariants hold, and what to reach
for when something looks wrong.

## The whole pipeline of data

```
programs/foo.asm
      │  assemble()                         src/assembler.cpp
      ▼
Program { text[], data[], labels, entry, textBase, dataBase }
      │  Simulator::load()                  src/simulator.cpp
      ▼
Process { regs[16], pc, pageTable, regions[], stats }
      │      └── image is written to the MemorySystem *backing store*
      │          and its pages marked non-resident, so it faults in on use
      │  Scheduler::pick()                  src/scheduler.cpp
      ▼
Core::attach(p)  →  Core::cycle() × N       src/pipeline.cpp
      │                    │
      │                    ├── IF/ID/EX/MEM/WB read and write the latches
      │                    └── MEM and IF call into MemorySystem
      │                                            src/memory.cpp
      ▼
Stats (per process, summed at the end)      include/stats.h
      │
      ├── printReport()                     src/report.cpp
      ├── writeCsv()
      └── TraceRecorder → JSON → viewer/    src/trace.cpp, viewer/
```

Nothing flows backwards. If a number in the report looks wrong, walk that chain
from the bottom: the report only ever sums counters, the counters are only ever
incremented in the stage that owns the event, and the stages only ever read the
latches.

## One cycle, exactly

`Core::cycle()` is the heart of it. Everything else is bookkeeping.

```cpp
if (memStall_ > 0) { --memStall_; ++stallMemory; return; }   // whole pipe frozen

stallIF_ = false;
flushedThisCycle_ = false;

writebackStage();   // cur_.memwb → register file, retire
memoryStage();      // cur_.exmem → nxt_.memwb, D-cache / MMU access
executeStage();     // cur_.idex  → nxt_.exmem, ALU, forwarding, branch resolve
decodeStage();      // cur_.ifid  → nxt_.idex, regfile read, hazard detect
fetchStage();       //             → nxt_.ifid, I-cache / MMU access, predict

cur_ = nxt_;        // commit
```

Two separate mechanisms are at work here, and confusing them is the source of
most pipeline bugs:

**Double buffering.** Every stage reads `cur_` and writes `nxt_`. This is what
makes the pipeline correct regardless of the order the stage functions run in —
no stage can see another stage's output from the same cycle.

**Reverse call order.** WB runs before ID *specifically* so that a register
written this cycle is visible to a register read this cycle. That is the classic
"write in the first half, read in the second half" register file, and it is why
a hazard with three instructions of distance needs no forwarding at all.

So: double buffering handles the latches, call order handles the register file.

### The four flags that couple stages together

Latches carry instructions forward; these flags carry control backwards. They are
the only back-channel in the design, and the call order above is what makes them
work.

| Flag | Set by | Read by | Meaning |
| --- | --- | --- | --- |
| `memStall_` | MEM | top of `cycle()` | data access missed; freeze the entire pipeline for N cycles |
| `ifStall_` | IF | IF | instruction fetch missed; fetch nothing for N cycles |
| `stallIF_` | ID | IF | load-use hazard; hold IF/ID, do not fetch |
| `flush_` | EX | IF | mispredict; squash and redirect |

EX runs before ID and IF, so a flush reaches both in the same cycle. ID runs
before IF, so a stall reaches fetch in the same cycle. Reorder those calls and
the pipeline still *runs*, but stalls and flushes land one cycle late.

### Where the two-cycle mispredict penalty comes from

EX resolves the branch and sets `flush_`. In that same cycle, ID throws away
whatever was in IF/ID (wrong path), and IF declines to fetch and instead points
`pc_` at the correct target. So two bubbles enter the pipeline and the correct
instruction is fetched the following cycle. If you ever see a mispredict cost
something other than 2, that arithmetic is where to look.

### Forwarding, precisely

EX reads its operands from `cur_.exmem` (one instruction ahead) and `cur_.memwb`
(two ahead). A **load** in `cur_.exmem` is deliberately excluded — its data is
still being fetched this cycle. That exclusion is exactly the load-use hazard,
and ID stalls one cycle to cover it. After that stall the load has moved to
`cur_.memwb` and forwards normally.

## The memory hierarchy

One call, five things that can go wrong:

```
va → region check      → Segmentation / Protection fault
   → alignment check   → Misaligned fault
   → TLB (pid-tagged)  → miss costs tlbMissPenalty
   → page table        → not resident? page fault, allocate frame,
                         maybe evict FIFO victim (write back if dirty,
                         invalidate its TLB entry and cache lines)
   → paddr
   → L1 I/D cache      → miss costs missPenalty (+ writebackPenalty if the
                         victim line was dirty)
   → physical memory   → the actual bytes
```

Caches are **tag-only**: they model timing, the bytes always come from
`physical_`. So a cache bug can make the simulator report wrong *cycles* but
never wrong *data*. That narrows things down fast: if a program computes the
wrong answer, the cache is not your problem.

Programs are demand-paged for real. `loadIntoBackingStore()` puts the image in a
swap map and marks pages invalid, so the first fetch and the first data access
each take a page fault. At the default 200-cycle penalty that is ~430 cycles of
cold start, which dominates any program that retires only a few hundred
instructions.

## Troubleshooting playbook

### A program computes the wrong answer

Start here, because this property must hold by construction:

```sh
make invariants                    # every shipped program, 11 configurations
./tools/check_invariants.sh programs/foo.asm    # or just yours
```

**Results must never depend on microarchitecture.** Only cycle counts may change.
The script runs each program with forwarding off, prediction off, tiny caches, a
2-entry TLB, 2 physical frames, zero memory latency, and two schedulers, and
compares the printed output. If any configuration disagrees, the flag that
disagrees names the subsystem that owns the bug:

| Disagreeing flag | Look at |
| --- | --- |
| `--no-forwarding` | `forwardOperand()` or the load-use stall in `decodeStage()` |
| `--no-predictor` | branch resolve and flush in `executeStage()`, redirect in `fetchStage()` |
| `--frames 2` | eviction, writeback or TLB shootdown in `src/memory.cpp` |
| `--dcache 4x1` / `--line 128` | cache indexing or tagging |
| `--tlb 2` | TLB replacement or the stale-PTE path |
| `--quantum 7` / `--sched mlfq` | context switch, pipeline drain, saved PC |

This is a real detector, not decoration. Disabling the MEM/WB bypass by one line
makes it report 11 failures across 5 programs, with `--no-forwarding` flagged
first. The C++ suite catches the same mutation, but this localises it faster.

Then watch the instruction stream with the noise turned off:

```sh
./cpusim --trace --fault-penalty 0 --miss-penalty 0 programs/foo.asm | head -60
```

The `note` column names any stall. Columns are the four latches, so an
instruction should appear in each in turn; if one vanishes, it was squashed.

### It is unexpectedly slow

The `WHERE THE CYCLES WENT` table sums to the total cycle count by construction,
so nothing can hide. `BOTTLENECK ANALYSIS` names the dominant term. In the
viewer, the activity strip under the scrubber shows the same thing positionally.

### It looks frozen for a long stretch

Almost always demand paging. Check `paging  N faults` in the report. Two faults
at 200 cycles each is the normal cold start. Use `--fault-penalty 0` to remove
it, or a longer-running program to amortise it.

### It hangs or hits the cycle limit

```sh
./cpusim --max-cycles 2000 --trace programs/foo.asm | tail -40
```

The report prints `(cycle limit reached)` rather than pretending it finished.
A tight loop with a branch that never flips shows up immediately in the tail.

### A process died

```
!! bad terminated: segmentation fault at 0x00000000 during load
```

Faults are precise and name the address and the stage. Page 0 is deliberately
left unmapped, so a null dereference faults instead of silently reading zeros.
Text pages are read-only, so self-modifying code faults too.

### The assembler rejects something

Errors carry line numbers: `line 2: unknown label or immediate 'nowhere'`.
Remember the ISA's limits — an 18-bit signed immediate, and a branch
displacement relative to `pc + 4`.

### The viewer looks wrong

Bisect between the trace and the page, in this order:

```sh
node viewer/verify_trace.js run.json    # is the trace itself sound?
node viewer/test_viewer.js  run.json    # does the page render it correctly?
```

`verify_trace.js` replays every delta from cycle zero and checks it lands on the
final register state that the C++ wrote out independently. If it passes, the
trace is faithful and the bug is in the page. If it fails, the recorder is at
fault and the viewer is innocent.

## Trace timing semantics

The one thing to internalise before touching `src/trace.cpp`:

> **A snapshot is taken *before* a cycle executes.** So `records[N]` is the state
> *entering* cycle N, and the deltas it carries describe what happened during
> cycle N−1.

Two bugs came from getting this wrong. The last cycle's deltas were originally
never recorded at all, because each snapshot only reports changes relative to the
*next* one — hence the trailing "pipeline empty" record that `finish()` emits.
And `finalRegs` for a truncated trace was captured from the live `Process`
objects, which are already one cycle ahead of the last record; it now comes from
the recorder's own `prev_` map.

Deltas also carry an explicit `pid`, because every process is diffed every cycle,
not just the running one — evicting a frame edits the page table of whichever
process owned it.

## Where to change what

| You want to | Edit |
| --- | --- |
| Add an instruction | `include/isa.h` enum, the `kOps` table and `executeStage()` |
| Change hazard or forwarding logic | `decodeStage()` / `forwardOperand()` |
| Change branch prediction | `fetchStage()` (predict) and `executeStage()` (resolve, update) |
| Change cache, TLB or paging | `src/memory.cpp` |
| Add a scheduler | subclass `Scheduler`, register it in `makeScheduler()` |
| Add a counter | the X-macro list in `include/stats.h` — **one place only**; the struct, per-process merge, CSV and trace all follow |
| Change the report | `src/report.cpp` |
| Change the trace format | `src/trace.cpp`, then `viewer/replay.js`, then `make traces` |

## Gotchas that have actually bitten

* **Adding a counter anywhere but `stats.h`.** The X-macro list generates the
  struct, the merge, the CSV and the trace keys. Add it once, in that list.
* **Changing the trace format without running `make traces`.** The bundled
  `viewer/demo-trace.js` is a committed artifact and goes stale.
* **Assuming a new prediction source is safe.** The BTB is shared across
  processes and indexed by PC with no tag, and every process's text starts at
  `0x1000`. EX therefore validates predictions for *non-control* instructions
  too; without that, a jump in one process silently makes another skip
  instructions. See `testBtbAliasingBetweenProcesses`.
* **Stale objects after a header edit.** The Makefile generates header
  dependencies with `-MMD -MP`, so this is handled — but if you ever see a
  segfault immediately after changing a struct, `make clean` first and confirm.
* **`make test` looking green while doing less than you think.** It runs three
  suites; check that you see three `N checks, 0 failures` lines, not one.

## Test layout

| Suite | Covers | Run |
| --- | --- | --- |
| `tests/test_sim.cpp` | ISA, assembler, pipeline, memory, schedulers | `make test` |
| `viewer/verify_trace.js` | trace faithfully reconstructs the run | `node viewer/verify_trace.js f.json` |
| `viewer/test_viewer.js` | the page renders and navigates correctly | `node viewer/test_viewer.js f.json` |
| `tools/check_invariants.sh` | results independent of microarchitecture | `make invariants` |

Many assertions are **differential** rather than absolute — forwarding must
reduce cycles without changing results, a 64-byte stride must miss more than a
4-byte stride, a thrashing configuration must still compute the same answer.
Differential tests survive intentional changes to the model, where hardcoded
cycle counts would just break and get updated without being read.
