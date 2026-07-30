# cpusim

A cycle-level CPU simulator in C++17. It runs real programs written for a small
custom instruction set through an in-order 5-stage pipeline, a virtual memory
system with demand paging, and a pluggable process scheduler — then reports where
every cycle went and which part of the machine is the bottleneck.

The point of the project is not to be fast. It is to make the cost of a
microarchitectural decision *visible*: turn forwarding off and watch CPI double,
shrink the cache and watch the same program take 45% longer, shrink physical
memory and watch page faults eat the run.

```
$ make && ./cpusim programs/matmul.asm
...
==============================================================================
WHERE THE CYCLES WENT
------------------------------------------------------------------------------
  useful issue (1 instr/cycle)         10534   85.7%
  data memory stalls                     520    4.2%
  instruction fetch stalls               298    2.4%
  load-use hazard stalls                 507    4.1%
  branch mispredict flushes              192    1.6%
  pipeline fill / other bubbles          236    1.9%
```

## Build and run

```sh
make              # builds ./cpusim
make test         # builds and runs the test suite (181 assertions)
make demo         # a couple of representative runs

# or with CMake
cmake -B build && cmake --build build && ctest --test-dir build
```

Needs only a C++17 compiler. No external dependencies.

```sh
./cpusim programs/vector_add.asm                        # single process
./cpusim --sched mlfq --quantum 60 \
         programs/spinner.asm programs/yielder.asm      # two processes
./cpusim --no-forwarding programs/hazard_chain.asm      # ablation
./cpusim --trace --fault-penalty 0 programs/fib.asm     # per-cycle pipeline view
./cpusim --csv out.csv programs/matmul.asm              # every counter as CSV
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

`make test` runs 181 assertions covering encode/decode round trips, assembler
layout and error handling, ALU semantics, the memory hierarchy, faults, all three
schedulers, and cycle accounting. Several are differential rather than absolute:
they assert that enabling forwarding reduces cycles *without changing the
result*, that a 64-byte stride misses far more than a 4-byte stride, and that a
thrashing configuration still computes the same answer.

One of them, `testBtbAliasingBetweenProcesses`, is a regression test for a bug
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

## Layout

```
include/     isa.h assembler.h memory.h process.h pipeline.h scheduler.h simulator.h stats.h
src/         isa.cpp assembler.cpp memory.cpp pipeline.cpp scheduler.cpp
             simulator.cpp report.cpp stats.cpp main.cpp
programs/    assembly benchmarks
tests/       test_sim.cpp
```

Every performance counter is declared once, in an X-macro list in `stats.h`, so
the struct, the per-process merge, the report and the CSV export cannot drift
apart.
