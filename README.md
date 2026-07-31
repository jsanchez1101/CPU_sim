# cpusim

**A cycle-accurate CPU simulator in C++17** — an in-order 5-stage pipeline,
virtual memory with demand paging, and three process schedulers. It runs real
programs and reports where every cycle went and which part of the machine is the
bottleneck.

The point is not to be fast. It is to make the cost of a microarchitectural
decision *measurable*.

|  |  |
| --- | --- |
| **Pipeline** | 5 stages, operand forwarding, load-use hazard detection, 2-bit branch predictor + BTB, precise faults |
| **Memory** | per-process page tables, pid-tagged TLB, demand paging with FIFO eviction and swap, set-associative write-back L1 I/D caches |
| **OS** | round-robin / priority / MLFQ scheduling, pipeline-drained context switching |
| **Measured** | forwarding worth **1.96×** · cache line size worth **2.36×** · MLFQ cuts interactive turnaround **5.1×** |
| **Tested** | 253 assertions, 3 suites, plus a mutation-verified invariant checker across 91 configurations |
| **Tooling** | zero-dependency browser viewer that replays a run cycle by cycle |
| **Size** | ~4,300 lines of C++ (plus a 1,300-line viewer), no external dependencies |

```
$ make && ./cpusim programs/matmul.asm
==============================================================================
WHERE THE CYCLES WENT
------------------------------------------------------------------------------
  useful issue (1 instr/cycle)         10534   85.7%
  data memory stalls                     520    4.2%
  instruction fetch stalls               298    2.4%
  load-use hazard stalls                 507    4.1%
  branch mispredict flushes              192    1.6%
  pipeline fill / other bubbles          236    1.9%

BOTTLENECK ANALYSIS
------------------------------------------------------------------------------
  * Largest single overhead: data memory stalls (4% of all cycles).
```

There is also an [interactive viewer](#interactive-viewer): record a run to JSON
and step through it in the browser, watching instructions move through the
pipeline while the caches, TLB and page tables update.

<!-- Add a screenshot of viewer/index.html here -- it is the single highest-value
     thing you can put in this README. Save it as docs/viewer.png and uncomment:
![The cycle-by-cycle pipeline viewer](docs/viewer.png)
-->

Start with [ARCHITECTURE.md](ARCHITECTURE.md) if you want to know how it works
internally, or [What is modelled](#what-is-modelled) below for the design.

## Build and run

```sh
make              # builds ./cpusim
make test         # runs the full test suite (253 assertions)
make demo         # a couple of representative runs
make traces       # regenerates the viewer's bundled demo trace
make invariants   # results must not depend on microarchitecture (91 comparisons)

# or with CMake
cmake -B build && cmake --build build && ctest --test-dir build
```

Needs only a C++17 compiler. No external dependencies. Node is optional and used
only to test the viewer; `make test` skips those and still passes without it.

```sh
./cpusim programs/vector_add.asm                        # single process
./cpusim --sched mlfq --quantum 60 \
         programs/spinner.asm programs/yielder.asm      # two processes
./cpusim --no-forwarding programs/hazard_chain.asm      # ablation
./cpusim --trace --fault-penalty 0 programs/fib.asm     # per-cycle pipeline view
./cpusim --csv out.csv programs/matmul.asm              # every counter as CSV
./cpusim --prefault programs/hazard_chain.asm           # warm start, no cold faults
./cpusim --emit-trace run.json programs/matmul.asm      # record for the viewer
./cpusim --help
```

## What is modelled

### Pipeline (`src/pipeline.cpp`)

Five stages — IF, ID, EX, MEM, WB — connected by double-buffered latches. Every
stage reads the current latch set and writes the next, so the order in which the
stage functions run cannot leak a value forward by accident.

* **Forwarding.** Bypass paths from EX/MEM and MEM/WB into EX. Combined with a
  register file that is written in the first half of a cycle and read in the
  second, this covers every hazard except one.
* **Load-use stalls.** The single case forwarding cannot fix: a load's data is
  not available until the end of MEM, so a dependent instruction in ID is held
  for exactly one cycle and a bubble is inserted.
* **Branch prediction.** A 64-entry table of 2-bit saturating counters plus a
  BTB for targets. Branches resolve in EX, so a mispredict costs two cycles.
  `--no-predictor` falls back to static predict-not-taken for comparison.
* **Precise faults.** Illegal opcodes, misaligned accesses, writes to read-only
  text and accesses outside any mapped region terminate the process with a
  diagnostic instead of corrupting state.

`--trace` prints the contents of all four latches every cycle with a note
explaining any stall, which is the fastest way to see a hazard actually happen:

```
cycle  process       IF/ID                ID/EX                EX/MEM               MEM/WB                note
   13  hazard_chain  add   r6, r1, r5     slli  r5, r2, 2      bge  r2, r3, 0x1038  li    r4, 0
   14  hazard_chain  lw    r7, 0(r6)      add   r6, r1, r5     slli  r5, r2, 2      bge  r2, r3, 0x1038    instruction fetch stall
   15  hazard_chain  add   r8, r7, r7     lw    r7, 0(r6)      add   r6, r1, r5     slli  r5, r2, 2         load-use stall, IF/ID held
   16  hazard_chain  add   r8, r7, r7     --                   lw    r7, 0(r6)      add   r6, r1, r5        data memory stall
   17  hazard_chain  --                   add   r8, r7, r7     --                   lw    r7, 0(r6)         data memory stall
```

(from `--trace --fault-penalty 0 --miss-penalty 4`, which keeps cold-start
paging out of the way)

### Memory management (`src/memory.cpp`)

Each access walks the full hierarchy:

```
virtual address
  -> region permission check      segmentation / write-protection faults
  -> TLB                          pid-tagged, fully associative, LRU
  -> page table                   per process, 4 KiB pages, demand paged
       -> page fault handler      frame allocation, FIFO eviction, swap in/out
  -> physical address
  -> L1 I-cache or D-cache        set associative, LRU, write-back
  -> physical memory
```

Programs are not loaded into memory up front. The loader writes the image into a
backing store and marks the pages non-resident, so text and data are faulted in
on first touch exactly like real demand paging. When frames run out, a FIFO
victim is chosen, written back to the store if dirty, removed from the TLB, and
its cache lines invalidated. Shrink physical memory with `--frames` and the same
program keeps producing the right answer while thrashing.

Caches are tag-only: they model timing while the bytes always live in physical
memory. That keeps the data path trivially correct without weakening the hit/miss
statistics, which is all the performance counters need.

#### Cold start, and how to measure without it

Demand paging is deliberate — without it the fault handler, frame allocator,
eviction path and swap would never execute on any program. But it has a
consequence worth knowing: the first instruction fetch and the first data access
each take a page fault, so a program pays roughly 430 cycles before it is
properly running. Real programs execute billions of instructions and amortise
that to nothing; these benchmarks retire a few hundred, so it can dominate.

`--prefault` maps text and data resident at load time, modelling a process whose
pages are already warm. Nothing else changes — same results, same cache and TLB
accesses, only the compulsory faults disappear:

| Program | Cold cycles | CPI | Warm cycles | CPI |
| --- | --- | --- | --- | --- |
| `hazard_chain.asm` | 543 | 6.24 | **143** | **1.64** |
| `fib.asm` | 651 | 3.83 | **255** | **1.50** |
| `vector_add.asm` | 712 | 3.85 | **312** | **1.69** |
| `pointer_chase.asm` | 14584 | 3.53 | 13976 | 3.39 |
| `matmul.asm` | 12287 | 1.17 | 11889 | 1.13 |

The short programs' cold CPI says almost nothing about the pipeline; warm, they
land where a 5-stage machine should. `matmul` barely moves, because at 10,534
retired instructions it already amortises its own cold start — which is the
honest illustration of why the effect is a benchmark-length artefact rather than
a property of the design.

Note the default 200-cycle penalty is *generous*, not harsh: a real minor fault
costs on the order of a thousand cycles, and a major one from an SSD is
~100,000. It is a toy-scale figure chosen to be visible without swamping a short
run. Stack and heap pages are anonymous, so they still fault on first touch even
with `--prefault`, exactly as they would for a warm process.

### Process scheduling (`src/scheduler.cpp`)

One core, several processes, each with its own register file, program counter,
page table and counters. Before any switch the pipeline is **drained** — fetch
stops and in-flight instructions finish — so no architectural state can cross
between processes. Each switch then pays `--switch-cost` cycles and flushes the
TLB.

| Policy | Behaviour |
| --- | --- |
| `rr` | Fixed quantum, FIFO ready queue. |
| `priority` | Lowest priority number first, FIFO within a band. |
| `mlfq` | Three queues with quanta Q, 2Q, 4Q. Burn a whole quantum and you drop a level; yield and you keep it. Periodic boost prevents starvation. |

Running a CPU-bound job (`spinner.asm`) next to an interactive one
(`yielder.asm`) at `--quantum 60` reproduces the textbook result — MLFQ works out
on its own what explicit priorities have to be told:

| Policy | Total cycles | spinner turnaround | yielder turnaround | Switches |
| --- | --- | --- | --- | --- |
| round robin | 12440 | 12440 | 5298 | 84 |
| priority, spinner favoured | 10476 | 9827 | 10476 | 1 |
| priority, yielder favoured | 10474 | 10474 | 627 | 1 |
| **mlfq** | **10128** | 10128 | **1030** | 4 |

## Interactive viewer

```sh
./cpusim --emit-trace run.json programs/matmul.asm
open viewer/index.html          # then drag run.json onto the page
```

`viewer/index.html` opens straight off disk with no server and no build step. It
ships with the first 2000 cycles of `matmul.asm`, so opening it does something
immediately; drop your own JSON on it to replace that. `--trace-cycles N` caps
long runs, and a truncated trace stays internally consistent — its recorded
final state describes the last cycle actually recorded, not the end of the run.

What it shows, all synchronised to the cycle you are looking at:

* the five pipeline stages with the instruction in each, bubbles marked, and the
  reason for any stall spelled out (`load-use stall, IF/ID held`)
* the source listing with each stage's current PC highlighted in its own colour
* the register file, with registers written this cycle flashed
* both L1 caches as set × way grids, valid/dirty/tag per line, changes flashed
* the TLB and the page table, showing pages faulting in and being evicted
* running counters and the effective address of any load or store

Controls: step forward and back a cycle at a time, play at four speeds, scrub the
whole run, jump to a cycle number, and **run until** the next mispredict, page
fault, eviction, cache miss, load-use stall, context switch, retired instruction,
or simply the next cycle that isn't stalled. Arrow keys step, shift steps by ten,
space plays. With multiple processes you can pin the side panels to a process
that is not currently running and watch its state sit frozen while the other one
executes.

Under the scrubber is an **activity strip**: one column per slice of the run,
coloured by what the pipeline was doing. It makes the shape of a run obvious
before you scrub into it — a cold start appears as a solid block of fetch and
memory stall before any green. Click anywhere on it to jump there.

That cold start is worth understanding, because it dominates short programs. Text
and data pages are demand-paged, so the first instruction fetch and the first
data access each take a page fault. At the default 200-cycle penalty that is
roughly 430 cycles before the program is properly running. `fib` only retires 170
instructions, so **61% of its run is those two compulsory faults** — real
behaviour, and exactly what the bottleneck analysis reports, but it means a short
program tells you very little about the pipeline. Use a longer program, or pass
`--fault-penalty 0` to look at pipeline behaviour in isolation.

**How it works.** The C++ side is a pure observer: `TraceRecorder` takes read-only
snapshots once per cycle and writes only what changed since the previous cycle, so
a 12,000-cycle run of `matmul` is about 1.5 MB rather than tens of megabytes. The
viewer reconstructs the state at any cycle by replaying those deltas from the
start, with keyframes every 500 records so seeking backwards stays instant. There
is no second simulator: the browser never computes what the machine did, it only
replays what the C++ recorded.

## Measured effect of each design decision

All from the shipped programs, default configuration unless noted.

| Experiment | Cycles | CPI | vs. baseline |
| --- | --- | --- | --- |
| `matmul.asm` | 12287 | 1.17 | — |
| `matmul.asm --no-forwarding` | 24126 | 2.29 | **1.96x slower** |
| `matmul.asm --no-predictor` | 13694 | 1.30 | 1.11x slower |
| `matmul.asm --dcache 8x1` | 17838 | 1.69 | 1.45x slower |
| `pointer_chase.asm` | 14584 | 3.53 | — |
| `pointer_chase.asm --line 128` | 6167 | 1.49 | **2.36x faster** |
| `pointer_chase.asm --frames 2` | 15400 | 3.73 | 1.06x slower (11 evictions) |

Instruction counts are identical across each group — only the timing changes,
which is what you want from an ablation.

## Instruction set

32-bit fixed-width instructions, 16 registers (`r0` hardwired to zero, `r14`/`sp`
initialised to the top of the stack, `r15`/`ra` written by `jal`).

| Class | Instructions |
| --- | --- |
| ALU reg-reg | `add sub mul and or xor sll srl slt` |
| ALU reg-imm | `addi andi ori xori slli srli slti li lui` |
| Memory | `lw rd, off(rs)` · `sw rs2, off(rs1)` |
| Control | `beq bne blt bge` (PC-relative) · `jmp jal jr` |
| System | `yield` (give up the CPU) · `print` (emit a register) · `halt` · `nop` |
| Pseudo | `mv rd, rs` · `la rd, label` · `j label` · `ret` |

Encoding:

```
R-type   [31:26] op  [25:22] rd   [21:18] rs1  [17:14] rs2
I-type   [31:26] op  [25:22] rd   [21:18] rs1  [17:0]  imm18 (signed)
SW       [31:26] op  [25:22] rs2  [21:18] rs1  [17:0]  imm18
BRANCH   [31:26] op  [25:22] rs2  [21:18] rs1  [17:0]  imm18 (byte disp. from pc+4)
J-type   [31:26] op  [25:0]  absolute target
```

The assembler (`src/assembler.cpp`) is two-pass and supports `.text`, `.data`,
`.word`, `.space`, labels, `label+8` arithmetic, decimal and hex literals, and
`#`/`;` comments. Address space per process: text at `0x1000` (page 0 is left
unmapped so a null dereference faults), data page-aligned after text, stack
descending from `0x40000`.

## Sample programs

| Program | What it exercises | Output |
| --- | --- | --- |
| `vector_add.asm` | sequential access, spatial locality | 272 |
| `fib.asm` | tight predictable loop, stores | 4181 |
| `hazard_chain.asm` | back-to-back load-use and a serial dependency chain | 288 |
| `matmul.asm` | 8x8 multiply, column-major inner loop | 7 |
| `pointer_chase.asm` | 64-byte stride over 8 KiB, four passes | 3584 |
| `spinner.asm` / `yielder.asm` | CPU-bound vs. interactive, for schedulers | 3000 / 40 |

## Tests

`make test` runs 253 assertions across three suites.

**`tests/test_sim.cpp` (198)** covers encode/decode round trips, assembler layout
and error handling, ALU semantics, the memory hierarchy, faults, all three
schedulers, and cycle accounting. Several are differential rather than absolute:
they assert that enabling forwarding reduces cycles *without changing the
result*, that a 64-byte stride misses far more than a 4-byte stride, and that a
thrashing configuration still computes the same answer.

**`viewer/verify_trace.js` (15)** proves a recorded trace actually reconstructs
the run. The simulator writes its final register state into the trace directly
from the `Process` objects; the verifier replays every delta from cycle zero and
asserts it lands on exactly that state, that per-cycle counter deltas sum to the
independently written totals, that seeking backwards through keyframes gives
byte-identical state to replaying forwards, and that no page or TLB entry ever
points outside physical memory.

**`viewer/test_viewer.js` (40)** loads the real `index.html` in jsdom and drives
it: asserts each stage box shows the instruction the trace says is there, that
stepping forward then back returns identical DOM, that the scrubber, activity
strip and run-until breakpoints land on cycles that genuinely contain the
requested event, and that the register display matches the simulator's ground
truth. It skips cleanly if node or jsdom is absent.

One C++ test, `testBtbAliasingBetweenProcesses`, is a regression test for a bug
this design invites. The BTB is shared between processes and indexed by PC with
no tag, and every process's text starts at `0x1000` — so a `jmp` in one process
can produce a BTB hit on a completely unrelated instruction in another. Because
that instruction is not a branch, EX had no reason to check the prediction, the
bogus redirect was never corrected, and the second process silently skipped
instructions. It still printed the right answer by luck, which is exactly why the
test asserts on the retired instruction count and not just the result.

## Modelling simplifications

Stated plainly, since a simulator that hides its approximations is not much use:

* A data cache miss freezes the whole pipeline for the miss penalty rather than
  stalling only MEM. For a single-issue in-order machine where WB is one cycle
  the difference is negligible.
* On an instruction cache miss the instruction is delivered and *then* fetch
  stalls, which shifts the stall one cycle later than hardware would.
* Cache and TLB fill latencies are fixed penalties; there is no memory
  controller, no bank conflicts, no prefetcher, and no L2.
* `mul` is single-cycle in EX.
* Page replacement is FIFO, not clock or LRU.
* No interrupts, no I/O, no blocked state — processes leave the CPU only by
  yielding, being preempted, halting, or faulting.

## Internals and debugging

[ARCHITECTURE.md](ARCHITECTURE.md) covers the cycle-by-cycle mechanics, the four
flags that couple the pipeline stages, trace timing semantics, and a
troubleshooting playbook keyed by symptom.

## Layout

```
include/     isa.h assembler.h memory.h process.h pipeline.h scheduler.h
             simulator.h stats.h trace.h
src/         isa.cpp assembler.cpp memory.cpp pipeline.cpp scheduler.cpp
             simulator.cpp report.cpp stats.cpp trace.cpp main.cpp
programs/    assembly benchmarks
tests/       test_sim.cpp
tools/       check_invariants.sh  the microarchitecture-independence check
viewer/      index.html      the viewer, opens off disk with no server
             replay.js       delta replay engine, shared by page and tests
             verify_trace.js trace correctness checks
             test_viewer.js  DOM tests for the page
             demo-trace.js   bundled fib.asm trace, regenerate with `make traces`
```

Every performance counter is declared once, in an X-macro list in `stats.h`, so
the struct, the per-process merge, the report and the CSV export cannot drift
apart.
