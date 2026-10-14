# Security

cpusim is a teaching and measurement tool, not a sandbox for hostile code. It
is still built to treat its inputs as untrusted, and the boundaries below are
tested in CI.

## Threat model

| Input | Who controls it | What must hold |
|---|---|---|
| `.asm` source | Anyone who hands you a program | The assembler rejects malformed source with an error. It never crashes, hangs or reads out of bounds. |
| Guest code at run time | The program's author | A process sees only its own memory. Faults stop the faulting process, not the simulator. Physical frames are zeroed before reuse, and shared predictor state cannot redirect another process. |
| Command-line options | The user | Bad values are rejected with an error. None can trigger an oversized allocation or an out-of-range physical address. |
| Trace files (`--emit-trace` output) | Anyone who sends you a trace | The viewer renders every field as text. A crafted trace cannot inject markup or script. |

Out of scope: timing side channels inside the simulated machine, which it
models deliberately, and resource use by a program that stays within
`--max-cycles`.

## How it is checked

- **Fuzzing.** `tests/fuzz_assembler.cpp` mutates the sample programs, then
  assembles the result and runs whatever assembles. CI runs it for 100,000
  inputs under AddressSanitizer and UBSan, plus 2 minutes of coverage-guided
  libFuzzer.
- **Sanitizers.** `make sanitize` runs the unit tests, the invariant checker and
  the fuzzer with ASan and UBSan, with undefined behaviour made fatal.
- **Isolation tests.** `tests/test_sim.cpp` covers segmentation, protection and
  alignment faults, BTB aliasing between processes, and frame reuse.
- **Input validation.** `validateConfig` (in `src/simulator.cpp`) and
  `tools/check_cli.sh`.
- **Viewer.** `viewer/test_viewer.js` loads a trace with markup in every string
  field and asserts that none of it becomes DOM.
- **Static analysis.** CodeQL `security-extended` queries run on C++ and
  JavaScript for every pull request and weekly.

## Reporting a vulnerability

Use **Report a vulnerability** on this repository's
[Security tab](https://github.com/jsanchez1101/CPU_sim/security). That opens a
private advisory. Please don't open a public issue for security problems.
Include the input that triggers it and the command you ran.
