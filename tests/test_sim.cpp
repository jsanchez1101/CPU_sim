// test_sim.cpp -- self-checking test suite.
//
// No framework: a CHECK macro, a counter, and a non-zero exit status if
// anything fails. Run with `make test`.
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "assembler.h"
#include "isa.h"
#include "simulator.h"

using namespace cpusim;

namespace {

int gChecks = 0;
int gFailures = 0;
std::string gCase;

void beginCase(const std::string& name) {
  gCase = name;
  std::printf("[ run  ] %s\n", name.c_str());
}

void checkImpl(bool cond, const char* expr, int line, const std::string& note) {
  ++gChecks;
  if (cond) return;
  ++gFailures;
  std::printf("[ FAIL ] %s:%d  %s  %s\n", gCase.c_str(), line, expr, note.c_str());
}

#define CHECK(cond) checkImpl((cond), #cond, __LINE__, "")
#define CHECK_EQ(actual, expected)                                             \
  checkImpl((actual) == (expected), #actual " == " #expected, __LINE__,         \
            "(got " + std::to_string(actual) + ", want " +                     \
                std::to_string(expected) + ")")

struct Run {
  std::unique_ptr<Simulator> sim;
  Process* proc = nullptr;
};

Run runSource(const std::string& src, SimConfig cfg = SimConfig{}) {
  Run r;
  cfg.maxCycles = 2'000'000;
  r.sim = std::make_unique<Simulator>(cfg);
  const Program prog = assemble(src, "t");
  r.proc = r.sim->load(prog);
  r.sim->run();
  return r;
}

// ---------------------------------------------------------------------------

void testEncodeDecodeRoundTrip() {
  beginCase("encode/decode round trip");
  struct Sample { Op op; uint8_t rd, rs1, rs2; int32_t imm; };
  const Sample samples[] = {
      {Op::ADD, 3, 4, 5, 0},
      {Op::SUB, 15, 1, 2, 0},
      {Op::SLT, 7, 0, 9, 0},
      {Op::ADDI, 2, 3, 0, -1234},
      {Op::LI, 8, 0, 0, 131071},
      {Op::LI, 8, 0, 0, -131072},
      {Op::LW, 6, 11, 0, -8},
      {Op::SW, 0, 11, 6, 2044},
      {Op::BEQ, 0, 4, 5, -64},
      {Op::BGE, 0, 1, 2, 128},
  };
  for (const Sample& s : samples) {
    Instr in;
    in.op = s.op;
    in.rd = s.rd;
    in.rs1 = s.rs1;
    in.rs2 = s.rs2;
    in.imm = s.imm;
    const Instr back = decode(encode(in), 0x1000);
    CHECK(back.op == s.op);
    if (formatOf(s.op) != Fmt::Jump) {
      CHECK_EQ(static_cast<int>(back.rs1), static_cast<int>(s.rs1));
      CHECK_EQ(back.imm, formatOf(s.op) == Fmt::R ? 0 : s.imm);
    }
  }
  // Jumps carry a 26-bit absolute target and an implicit link register.
  Instr jal;
  jal.op = Op::JAL;
  jal.imm = 0x1234;
  const Instr backJal = decode(encode(jal), 0);
  CHECK(backJal.op == Op::JAL);
  CHECK_EQ(backJal.imm, 0x1234);
  CHECK_EQ(static_cast<int>(backJal.rd), REG_RA);
}

void testAssemblerLayout() {
  beginCase("assembler layout and label resolution");
  const Program p = assemble(
      ".data\n"
      "x: .word 7, -1\n"
      "y: .space 8\n"
      ".text\n"
      "main:\n"
      "  la r1, x\n"
      "  lw r2, 4(r1)\n"
      "  beq r2, r2, main\n"
      "  halt\n",
      "layout");
  CHECK_EQ(p.instructionCount, 4u);
  CHECK_EQ(p.textBase, TEXT_BASE);
  CHECK_EQ(p.entry, TEXT_BASE);
  CHECK(p.dataBase > p.textBase);
  CHECK_EQ(p.dataBase % PAGE_SIZE, 0u);
  CHECK_EQ(p.labels.at("x"), p.dataBase);
  CHECK_EQ(p.labels.at("y"), p.dataBase + 8);
  CHECK_EQ(p.data.size(), 16u);          // two words plus eight bytes of space
  CHECK_EQ(p.text.size(), 16u);

  // The branch at slot 2 targets slot 0, so the displacement is -12.
  const uint32_t raw = static_cast<uint32_t>(p.text[8]) |
                       (static_cast<uint32_t>(p.text[9]) << 8) |
                       (static_cast<uint32_t>(p.text[10]) << 16) |
                       (static_cast<uint32_t>(p.text[11]) << 24);
  const Instr br = decode(raw, TEXT_BASE + 8);
  CHECK(br.op == Op::BEQ);
  CHECK_EQ(br.imm, -12);
  CHECK_EQ(br.pc + 4 + br.imm, TEXT_BASE);
}

void testAssemblerErrors() {
  beginCase("assembler rejects bad input");
  bool threwUnknownOp = false;
  try {
    assemble("main: frobnicate r1, r2\n", "bad");
  } catch (const AssembleError&) {
    threwUnknownOp = true;
  }
  CHECK(threwUnknownOp);

  bool threwUnknownLabel = false;
  try {
    assemble("main: j nowhere\n", "bad");
  } catch (const AssembleError&) {
    threwUnknownLabel = true;
  }
  CHECK(threwUnknownLabel);

  bool threwDuplicate = false;
  try {
    assemble("main: nop\nmain: halt\n", "bad");
  } catch (const AssembleError&) {
    threwDuplicate = true;
  }
  CHECK(threwDuplicate);
}

// Regression tests for two bugs found by libFuzzer. A .space near 2^32 wrapped
// the data offset, so a following .word was written far outside the data
// buffer; and a large but in-range .space produced a data region overlapping
// the stack. Both must be rejected before anything is allocated.
void testAssemblerBoundsTheImage() {
  beginCase("assembler rejects programs that overflow the address space");
  auto rejects = [](const std::string& src) {
    try {
      assemble(src, "big");
    } catch (const AssembleError&) {
      return true;
    }
    return false;
  };
  CHECK(rejects(".data\nbig: .space 0xFFFFFFFC\nx: .word 1\n.text\nmain: halt\n"));
  CHECK(rejects(".data\nbig: .space 99999999999\n.text\nmain: halt\n"));
  CHECK(rejects(".data\nbig: .space 300000\n.text\nmain: halt\n"));
  CHECK(rejects(".data\na: .space 131072\nb: .space 131072\n.text\nmain: halt\n"));

  // The largest data section that still leaves room for the heap is accepted.
  const uint32_t room = IMAGE_LIMIT - 2 * PAGE_SIZE - HEAP_SIZE;
  CHECK(!rejects(".data\nbig: .space " + std::to_string(room) +
                 "\n.text\nmain: halt\n"));
  CHECK(rejects(".data\nbig: .space " + std::to_string(room + 4) +
                "\n.text\nmain: halt\n"));

  // A hand-built Program that would overlap the stack is refused at load.
  Program prog = assemble("main: halt\n", "p");
  prog.data.assign(IMAGE_LIMIT, 0);
  Simulator sim(SimConfig{});
  bool threw = false;
  try {
    sim.load(prog);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
}

void testAluSemantics() {
  beginCase("ALU semantics");
  Run r = runSource(
      "main:\n"
      "  li   r1, 100\n"
      "  li   r2, 7\n"
      "  add  r3, r1, r2\n"     // 107
      "  sub  r4, r1, r2\n"     // 93
      "  mul  r5, r1, r2\n"     // 700
      "  and  r6, r1, r2\n"     // 100 & 7 = 4
      "  or   r7, r1, r2\n"     // 103
      "  xor  r8, r1, r2\n"     // 99
      "  slli r9, r2, 3\n"      // 56
      "  srli r10, r1, 2\n"     // 25
      "  slt  r11, r2, r1\n"    // 1
      "  slt  r12, r1, r2\n"    // 0
      "  li   r13, -5\n"
      "  slt  r13, r13, r2\n"   // signed compare -> 1
      "  halt\n");
  const auto& g = r.proc->regs;
  CHECK_EQ(g[3], 107u);
  CHECK_EQ(g[4], 93u);
  CHECK_EQ(g[5], 700u);
  CHECK_EQ(g[6], 4u);
  CHECK_EQ(g[7], 103u);
  CHECK_EQ(g[8], 99u);
  CHECK_EQ(g[9], 56u);
  CHECK_EQ(g[10], 25u);
  CHECK_EQ(g[11], 1u);
  CHECK_EQ(g[12], 0u);
  CHECK_EQ(g[13], 1u);
  CHECK(r.proc->state == ProcState::Terminated);
  CHECK(r.proc->faultMessage.empty());
}

void testRegisterZeroIsHardwired() {
  beginCase("r0 is hardwired to zero");
  Run r = runSource(
      "main:\n"
      "  li   r0, 42\n"
      "  add  r0, r0, r0\n"
      "  addi r1, r0, 5\n"
      "  halt\n");
  CHECK_EQ(r.proc->regs[0], 0u);
  CHECK_EQ(r.proc->regs[1], 5u);
}

void testMemoryRoundTrip() {
  beginCase("load/store round trip through the hierarchy");
  Run r = runSource(
      ".data\n"
      "buf: .space 32\n"
      ".text\n"
      "main:\n"
      "  la   r1, buf\n"
      "  li   r2, 1234\n"
      "  sw   r2, 0(r1)\n"
      "  li   r3, -77\n"
      "  sw   r3, 12(r1)\n"
      "  lw   r4, 0(r1)\n"
      "  lw   r5, 12(r1)\n"
      "  halt\n");
  CHECK_EQ(r.proc->regs[4], 1234u);
  CHECK_EQ(static_cast<int32_t>(r.proc->regs[5]), -77);
  CHECK(r.proc->stats.loads == 2);
  CHECK(r.proc->stats.stores == 2);
}

void testStackAndCalls() {
  beginCase("jal/ret and the stack region");
  Run r = runSource(
      "main:\n"
      "  li   r1, 5\n"
      "  addi sp, sp, -4\n"
      "  sw   r1, 0(sp)\n"
      "  jal  triple\n"
      "  lw   r3, 0(sp)\n"
      "  addi sp, sp, 4\n"
      "  halt\n"
      "triple:\n"
      "  add  r2, r1, r1\n"
      "  add  r2, r2, r1\n"
      "  ret\n");
  CHECK_EQ(r.proc->regs[2], 15u);
  CHECK_EQ(r.proc->regs[3], 5u);
  CHECK(r.proc->state == ProcState::Terminated);
}

const char* kLoadUseProgram =
    ".data\n"
    "v: .word 1, 2, 3, 4, 5, 6, 7, 8\n"
    ".text\n"
    "main:\n"
    "  la   r1, v\n"
    "  li   r2, 0\n"
    "  li   r3, 8\n"
    "  li   r4, 0\n"
    "loop:\n"
    "  bge  r2, r3, done\n"
    "  slli r5, r2, 2\n"
    "  add  r6, r1, r5\n"
    "  lw   r7, 0(r6)\n"
    "  add  r8, r7, r7\n"
    "  add  r4, r4, r8\n"
    "  addi r2, r2, 1\n"
    "  j    loop\n"
    "done:\n"
    "  print r4\n"
    "  halt\n";

void testForwardingReducesStalls() {
  beginCase("forwarding cuts stall cycles without changing results");
  SimConfig withFwd;
  SimConfig noFwd;
  noFwd.core.forwarding = false;

  Run a = runSource(kLoadUseProgram, withFwd);
  Run b = runSource(kLoadUseProgram, noFwd);

  CHECK_EQ(a.proc->regs[4], 72u);   // 2 * (1+..+8)
  CHECK_EQ(b.proc->regs[4], 72u);
  CHECK(a.proc->output.size() == 1 && a.proc->output[0] == 72);
  CHECK(b.proc->output.size() == 1 && b.proc->output[0] == 72);

  CHECK(a.proc->stats.forwards > 0);
  CHECK(b.proc->stats.forwards == 0);
  CHECK(a.proc->stats.stallLoadUse > 0);
  CHECK(b.proc->stats.stallDataHazard > a.proc->stats.stallLoadUse);
  CHECK(a.sim->cycles() < b.sim->cycles());
  CHECK(a.proc->stats.instructions == b.proc->stats.instructions);
}

void testBranchPredictorHelps() {
  beginCase("2-bit predictor beats predict-not-taken on a loop");
  const char* loop =
      "main:\n"
      "  li  r1, 0\n"
      "  li  r2, 200\n"
      "loop:\n"
      "  addi r1, r1, 1\n"
      "  blt  r1, r2, loop\n"
      "  halt\n";
  SimConfig on;
  SimConfig off;
  off.core.predictor = false;

  Run a = runSource(loop, on);
  Run b = runSource(loop, off);
  CHECK_EQ(a.proc->regs[1], 200u);
  CHECK_EQ(b.proc->regs[1], 200u);
  CHECK(a.proc->stats.branches == b.proc->stats.branches);
  CHECK(a.proc->stats.mispredicts < b.proc->stats.mispredicts);
  CHECK(a.sim->cycles() < b.sim->cycles());
  // A backward branch taken 199 times out of 200 should be nearly perfect.
  CHECK(a.proc->stats.mispredictRate() < 5.0);
}

void testCachePatternMatters() {
  beginCase("stride pattern drives the D-cache miss rate");
  const char* tmpl =
      ".data\n"
      "buf: .space 4096\n"
      ".text\n"
      "main:\n"
      "  la   r1, buf\n"
      "  li   r2, 0\n"
      "  li   r3, 64\n"
      "  li   r5, 0\n"
      "loop:\n"
      "  bge  r2, r3, done\n"
      "  slli r4, r2, SHIFT\n"
      "  add  r6, r1, r4\n"
      "  lw   r7, 0(r6)\n"
      "  add  r5, r5, r7\n"
      "  addi r2, r2, 1\n"
      "  j    loop\n"
      "done:\n"
      "  halt\n";
  auto withShift = [&](const char* shift) {
    std::string src = tmpl;
    const size_t at = src.find("SHIFT");
    src.replace(at, 5, shift);
    return src;
  };

  Run seq = runSource(withShift("2"));   // 4-byte stride
  Run str = runSource(withShift("6"));   // 64-byte stride, one line each

  CHECK(seq.proc->stats.dcacheAccesses == 64);
  CHECK(str.proc->stats.dcacheAccesses == 64);
  CHECK(seq.proc->stats.dcacheMissRate() < 20.0);
  CHECK(str.proc->stats.dcacheMissRate() > 90.0);
  CHECK(seq.sim->cycles() < str.sim->cycles());
}

void testDemandPagingAndEviction() {
  beginCase("demand paging, eviction and thrashing stay correct");
  const char* prog =
      ".data\n"
      "buf: .space 12288\n"      // three pages
      ".text\n"
      "main:\n"
      "  la   r1, buf\n"
      "  li   r2, 0\n"
      "  li   r3, 24\n"
      "  li   r5, 0\n"
      "  li   r8, 3\n"
      "loop:\n"
      "  bge  r2, r3, done\n"
      "  slli r4, r2, 9\n"       // 512-byte stride across all three pages
      "  add  r6, r1, r4\n"
      "  sw   r8, 0(r6)\n"
      "  lw   r7, 0(r6)\n"
      "  add  r5, r5, r7\n"
      "  addi r2, r2, 1\n"
      "  j    loop\n"
      "done:\n"
      "  halt\n";

  SimConfig roomy;
  SimConfig tight;
  tight.mem.frames = 3;          // text + data pages cannot all be resident

  Run a = runSource(prog, roomy);
  Run b = runSource(prog, tight);

  CHECK_EQ(a.proc->regs[5], 72u);
  CHECK_EQ(b.proc->regs[5], 72u);      // same answer despite thrashing
  CHECK(a.proc->stats.pageFaults > 0);
  CHECK(a.proc->stats.pageEvictions == 0);
  CHECK(b.proc->stats.pageEvictions > 0);
  CHECK(b.proc->stats.pageFaults > a.proc->stats.pageFaults);
  CHECK(b.sim->cycles() > a.sim->cycles());
  CHECK(b.proc->faultMessage.empty());
}

void testWarmStartRemovesColdFaults() {
  beginCase("warm start removes compulsory faults without changing results");
  SimConfig cold;
  SimConfig warm;
  warm.prefault = true;

  Run c = runSource(kLoadUseProgram, cold);
  Run w = runSource(kLoadUseProgram, warm);

  CHECK_EQ(c.proc->regs[4], 72u);
  CHECK_EQ(w.proc->regs[4], 72u);            // warm start is a timing knob only
  CHECK(c.proc->stats.pageFaults > 0);
  CHECK_EQ(w.proc->stats.pageFaults, 0u);
  CHECK_EQ(w.proc->stats.pageFaultCycles, 0u);
  CHECK(w.sim->cycles() < c.sim->cycles());
  CHECK_EQ(w.proc->stats.instructions, c.proc->stats.instructions);

  // Only paging is skipped: the caches and the TLB are still walked exactly as
  // many times, so warm start cannot be hiding memory accesses.
  CHECK_EQ(w.proc->stats.dcacheAccesses, c.proc->stats.dcacheAccesses);
  CHECK_EQ(w.proc->stats.icacheAccesses, c.proc->stats.icacheAccesses);
  CHECK_EQ(w.proc->stats.tlbAccesses, c.proc->stats.tlbAccesses);
  CHECK(w.proc->stats.tlbMisses > 0);        // still a cold TLB
}

void testWarmStartUnderMemoryPressure() {
  beginCase("warm start degrades to demand paging when memory is too small");
  const char* prog =
      ".data\n"
      "buf: .space 12288\n"           // three pages, plus a text page
      ".text\n"
      "main:\n"
      "  la   r1, buf\n"
      "  li   r2, 0\n"
      "  li   r3, 24\n"
      "  li   r5, 0\n"
      "  li   r8, 3\n"
      "loop:\n"
      "  bge  r2, r3, done\n"
      "  slli r4, r2, 9\n"
      "  add  r6, r1, r4\n"
      "  sw   r8, 0(r6)\n"
      "  lw   r7, 0(r6)\n"
      "  add  r5, r5, r7\n"
      "  addi r2, r2, 1\n"
      "  j    loop\n"
      "done:\n"
      "  halt\n";

  SimConfig roomy;
  roomy.prefault = true;
  SimConfig tight;
  tight.prefault = true;
  tight.mem.frames = 2;               // cannot hold the whole image

  Run a = runSource(prog, roomy);
  Run b = runSource(prog, tight);

  CHECK_EQ(a.proc->regs[5], 72u);
  CHECK_EQ(b.proc->regs[5], 72u);     // correct either way
  CHECK_EQ(a.proc->stats.pageFaults, 0u);
  // Warming must never evict to make room, so the pages it could not map are
  // simply demand paged as before.
  CHECK(b.proc->stats.pageFaults > 0);
  CHECK(b.proc->faultMessage.empty());
  CHECK(b.sim->cycles() > a.sim->cycles());
}

void testTlbCounters() {
  beginCase("TLB counts a lookup per access and misses on cold pages");
  Run r = runSource(
      ".data\n"
      "buf: .space 32\n"
      ".text\n"
      "main:\n"
      "  la   r1, buf\n"
      "  li   r2, 9\n"
      "  sw   r2, 0(r1)\n"
      "  lw   r3, 0(r1)\n"
      "  halt\n");
  const Stats& s = r.proc->stats;
  CHECK_EQ(s.tlbAccesses, s.icacheAccesses + s.dcacheAccesses);
  CHECK(s.tlbMisses >= 2);       // at least one text page and one data page
  CHECK(s.pageFaults >= 2);
  CHECK_EQ(r.proc->regs[3], 9u);
}

void testSegmentationFault() {
  beginCase("faults terminate the offending process");
  Run r = runSource(
      "main:\n"
      "  li   r1, 0\n"
      "  lw   r2, 0(r1)\n"       // page 0 is intentionally unmapped
      "  halt\n");
  CHECK(r.proc->state == ProcState::Terminated);
  CHECK(!r.proc->faultMessage.empty());
  CHECK(!r.sim->hitCycleLimit());
}

void testMisalignedAccess() {
  beginCase("misaligned word access faults");
  Run r = runSource(
      ".data\n"
      "buf: .space 32\n"
      ".text\n"
      "main:\n"
      "  la   r1, buf\n"
      "  addi r1, r1, 2\n"
      "  lw   r2, 0(r1)\n"
      "  halt\n");
  CHECK(r.proc->state == ProcState::Terminated);
  CHECK(r.proc->faultMessage.find("misaligned") != std::string::npos);
}

void testWriteToTextIsRejected() {
  beginCase("text pages are not writable");
  Run r = runSource(
      "main:\n"
      "  li   r1, 0x1000\n"
      "  li   r2, 1\n"
      "  sw   r2, 0(r1)\n"
      "  halt\n");
  CHECK(r.proc->state == ProcState::Terminated);
  CHECK(r.proc->faultMessage.find("protection") != std::string::npos);
}

std::unique_ptr<Simulator> runTwo(SchedPolicy policy, uint64_t quantum,
                                  int prioA, int prioB) {
  SimConfig cfg;
  cfg.policy = policy;
  cfg.quantum = quantum;
  cfg.maxCycles = 2'000'000;
  auto sim = std::make_unique<Simulator>(cfg);
  const char* spin =
      "main:\n"
      "  li r1, 0\n"
      "  li r2, 400\n"
      "loop:\n"
      "  bge r1, r2, done\n"
      "  addi r1, r1, 1\n"
      "  j loop\n"
      "done:\n"
      "  halt\n";
  const char* yield =
      "main:\n"
      "  li r1, 0\n"
      "  li r2, 40\n"
      "loop:\n"
      "  bge r1, r2, done\n"
      "  addi r1, r1, 1\n"
      "  yield\n"
      "  j loop\n"
      "done:\n"
      "  halt\n";
  Program a = assemble(spin, "spin");
  Program b = assemble(yield, "yield");
  sim->load(a, prioA);
  sim->load(b, prioB);
  sim->run();
  return sim;
}

void testSchedulersMakeProgress() {
  beginCase("every scheduler runs both processes to completion");
  const SchedPolicy policies[] = {SchedPolicy::RoundRobin, SchedPolicy::Priority,
                                  SchedPolicy::MLFQ};
  for (SchedPolicy policy : policies) {
    auto sim = runTwo(policy, 60, 0, 1);
    CHECK(!sim->hitCycleLimit());
    for (const auto& p : sim->processes()) {
      CHECK(p->state == ProcState::Terminated);
      CHECK(p->faultMessage.empty());
    }
    CHECK_EQ(sim->processes()[0]->regs[1], 400u);
    CHECK_EQ(sim->processes()[1]->regs[1], 40u);
    CHECK(sim->totals().contextSwitches > 0);
    // Every switch costs real cycles, so wall cycles exceed the sum of the
    // per-process cycle counts.
    uint64_t onCpu = 0;
    for (const auto& p : sim->processes()) onCpu += p->stats.cycles;
    CHECK(sim->cycles() > onCpu);
  }
}

void testQuantumControlsSwitchCount() {
  beginCase("a longer quantum means fewer context switches");
  auto small = runTwo(SchedPolicy::RoundRobin, 25, 0, 0);
  auto large = runTwo(SchedPolicy::RoundRobin, 400, 0, 0);
  CHECK(small->totals().contextSwitches > large->totals().contextSwitches);
  CHECK(small->cycles() > large->cycles());
}

void testPriorityOrdering() {
  beginCase("priority scheduling favours the lower priority number");
  // The spinner is given the better priority, so it should finish first even
  // though the yielder keeps becoming runnable.
  auto sim = runTwo(SchedPolicy::Priority, 100, 0, 5);
  const Process& spin = *sim->processes()[0];
  const Process& yield = *sim->processes()[1];
  CHECK(spin.finishCycle < yield.finishCycle);

  auto flipped = runTwo(SchedPolicy::Priority, 100, 5, 0);
  CHECK(flipped->processes()[1]->firstRunCycle <=
        flipped->processes()[0]->firstRunCycle);
}

void testMlfqDemotesCpuBoundJob() {
  beginCase("MLFQ demotes the CPU-bound job below the yielder");
  SimConfig cfg;
  cfg.policy = SchedPolicy::MLFQ;
  cfg.quantum = 40;
  cfg.maxCycles = 2'000'000;
  Simulator sim(cfg);
  const char* spin =
      "main:\n"
      "  li r1, 0\n"
      "  li r2, 2000\n"
      "loop:\n"
      "  bge r1, r2, done\n"
      "  addi r1, r1, 1\n"
      "  j loop\n"
      "done:\n"
      "  halt\n";
  const char* yield =
      "main:\n"
      "  li r1, 0\n"
      "  li r2, 20\n"
      "loop:\n"
      "  bge r1, r2, done\n"
      "  addi r1, r1, 1\n"
      "  yield\n"
      "  j loop\n"
      "done:\n"
      "  halt\n";
  Program a = assemble(spin, "spin");
  Program b = assemble(yield, "yield");
  Process* pa = sim.load(a, 0);
  Process* pb = sim.load(b, 0);
  sim.run();
  CHECK(pa->state == ProcState::Terminated);
  CHECK(pb->state == ProcState::Terminated);
  CHECK(pa->mlfqLevel > pb->mlfqLevel);
  CHECK(pb->finishCycle < pa->finishCycle);
}

// Regression test. The BTB is shared between processes and indexed by PC with
// no tag, and every process's text starts at TEXT_BASE, so process B can hit a
// BTB entry that process A installed for a jump. If the core does not verify
// the prediction for non-control instructions, that bogus redirect is never
// corrected and B silently skips instructions -- it still produced the right
// answer here by luck, which is why this checks the retired count, not just the
// result.
void testBtbAliasingBetweenProcesses() {
  beginCase("a shared BTB cannot make one process skip another's instructions");
  // A `j` at the same address that B uses for a plain arithmetic instruction.
  const char* jumper =
      "main:\n"
      "  li   r1, 0\n"           // 0x1000
      "  li   r2, 60\n"          // 0x1004
      "loop:\n"
      "  bge  r1, r2, done\n"    // 0x1008
      "  addi r1, r1, 1\n"       // 0x100c
      "  j    loop\n"            // 0x1010  <- installs BTB[0x1010]
      "done:\n"
      "  halt\n";
  const char* straight =
      "main:\n"
      "  li   r1, 0\n"           // 0x1000
      "  li   r2, 20\n"          // 0x1004
      "loop:\n"
      "  bge  r1, r2, done\n"    // 0x1008
      "  addi r1, r1, 1\n"       // 0x100c
      "  addi r3, r3, 2\n"       // 0x1010  <- not a branch at all
      "  addi r4, r4, 3\n"       // 0x1014
      "  j    loop\n"            // 0x1018
      "done:\n"
      "  halt\n";

  // Baseline: run the second program on its own, with a pristine BTB.
  Run solo = runSource(straight);
  const uint64_t soloInstrs = solo.proc->stats.instructions;
  CHECK_EQ(solo.proc->regs[1], 20u);
  CHECK_EQ(solo.proc->regs[3], 40u);
  CHECK_EQ(solo.proc->regs[4], 60u);

  SimConfig cfg;
  cfg.quantum = 50;   // force interleaving so the BTB is polluted
  cfg.maxCycles = 2'000'000;
  Simulator sim(cfg);
  Process* a = sim.load(assemble(jumper, "jumper"));
  Process* b = sim.load(assemble(straight, "straight"));
  sim.run();

  CHECK(a->state == ProcState::Terminated);
  CHECK(b->state == ProcState::Terminated);
  CHECK_EQ(a->regs[1], 60u);
  // Every one of these three registers is only correct if no instruction was
  // skipped, and the retired count must match the uncontended run exactly.
  CHECK_EQ(b->regs[1], 20u);
  CHECK_EQ(b->regs[3], 40u);
  CHECK_EQ(b->regs[4], 60u);
  CHECK_EQ(b->stats.instructions, soloInstrs);
}

// Security property: a physical frame handed to a new owner must not carry the
// previous owner's data. Stack pages are anonymous (zero-filled on first touch
// rather than loaded from the program image), so they are where a missing
// zero fill would leak. The writer fills its whole stack with a secret and
// exits; with only 4 frames, the reader's stack lands in recycled frames and
// must still read as zero.
void testRecycledFramesDoNotLeakData() {
  beginCase("a recycled frame never exposes the previous process's data");
  const char* writer =
      "main:\n"
      "  mv   r1, sp\n"
      "  li   r2, 12345\n"
      "  li   r3, 0\n"
      "  li   r4, 4096\n"         // every word of the 4-page stack
      "loop:\n"
      "  bge  r3, r4, done\n"
      "  addi r1, r1, -4\n"
      "  sw   r2, 0(r1)\n"
      "  addi r3, r3, 1\n"
      "  j    loop\n"
      "done:\n"
      "  halt\n";
  const char* reader =
      "main:\n"
      "  mv   r1, sp\n"
      "  li   r3, 0\n"
      "  li   r4, 4096\n"
      "  li   r5, 0\n"            // OR of everything read
      "loop:\n"
      "  bge  r3, r4, done\n"
      "  addi r1, r1, -4\n"
      "  lw   r2, 0(r1)\n"
      "  or   r5, r5, r2\n"
      "  addi r3, r3, 1\n"
      "  j    loop\n"
      "done:\n"
      "  halt\n";

  SimConfig cfg;
  cfg.policy = SchedPolicy::Priority;  // writer runs to completion first
  cfg.mem.frames = 4;
  cfg.maxCycles = 20'000'000;
  Simulator sim(cfg);
  Process* w = sim.load(assemble(writer, "writer"), 0);
  Process* r = sim.load(assemble(reader, "reader"), 1);
  sim.run();

  CHECK(w->state == ProcState::Terminated);
  CHECK(r->state == ProcState::Terminated);
  CHECK(r->faultMessage.empty());
  CHECK(w->finishCycle < r->firstRunCycle);
  CHECK_EQ(r->regs[5], 0u);
}

void testConfigValidation() {
  beginCase("invalid configurations are rejected before allocation");
  CHECK(validateConfig(SimConfig{}).empty());

  auto rejects = [](void (*mutate)(SimConfig&)) {
    SimConfig cfg;
    mutate(cfg);
    if (validateConfig(cfg).empty()) return false;
    try {
      Simulator sim(cfg);
    } catch (const std::invalid_argument&) {
      return true;
    }
    return false;
  };
  CHECK(rejects([](SimConfig& c) { c.mem.frames = 0; }));
  CHECK(rejects([](SimConfig& c) { c.mem.frames = MAX_FRAMES + 1; }));
  CHECK(rejects([](SimConfig& c) { c.mem.lineSize = 0; }));
  CHECK(rejects([](SimConfig& c) { c.mem.lineSize = 3; }));
  CHECK(rejects([](SimConfig& c) { c.mem.lineSize = 48; }));
  CHECK(rejects([](SimConfig& c) { c.mem.lineSize = PAGE_SIZE * 2; }));
  CHECK(rejects([](SimConfig& c) { c.mem.dcacheSets = 0; }));
  CHECK(rejects([](SimConfig& c) { c.mem.icacheSets = 1u << 16; c.mem.icacheWays = 1u << 16; }));
  CHECK(rejects([](SimConfig& c) { c.mem.tlbEntries = MAX_TLB_ENTRIES + 1; }));
  CHECK(rejects([](SimConfig& c) { c.quantum = 0; }));
  CHECK(rejects([](SimConfig& c) { c.maxCycles = 0; }));

  SimConfig edge;
  edge.mem.frames = 1;
  edge.mem.lineSize = PAGE_SIZE;
  edge.mem.tlbEntries = 0;  // no TLB: every access walks the page table
  CHECK(validateConfig(edge).empty());
}

void testCycleAccounting() {
  beginCase("cycle accounting adds up");
  Run r = runSource(kLoadUseProgram);
  const Stats t = r.sim->totals();
  CHECK_EQ(t.cycles, r.sim->cycles());
  CHECK(t.instructions > 0);
  CHECK(t.cpi() >= 1.0);
  const uint64_t accounted = t.instructions + t.stallMemory + t.stallFetch +
                             t.stallLoadUse + t.stallDataHazard +
                             t.contextSwitchCycles;
  CHECK(accounted <= t.cycles);
  CHECK(t.pageFaultCycles <= t.stallMemory + t.stallFetch);
}

void testProgramFilesAssembleAndRun() {
  beginCase("shipped programs assemble and produce the documented answers");
  struct Expect { const char* path; int32_t value; };
  const Expect programs[] = {
      {"programs/vector_add.asm", 272},
      {"programs/fib.asm", 4181},
      {"programs/hazard_chain.asm", 288},
      {"programs/matmul.asm", 7},
      {"programs/pointer_chase.asm", 3584},
      {"programs/spinner.asm", 3000},
      {"programs/yielder.asm", 40},
  };
  for (const Expect& e : programs) {
    Program prog;
    try {
      prog = assembleFile(e.path);
    } catch (const AssembleError& err) {
      ++gChecks;
      ++gFailures;
      std::printf("[ FAIL ] %s: %s\n", e.path, err.what());
      continue;
    }
    SimConfig cfg;
    cfg.maxCycles = 5'000'000;
    Simulator sim(cfg);
    Process* p = sim.load(prog);
    sim.run();
    checkImpl(p->state == ProcState::Terminated, e.path, __LINE__,
              p->faultMessage);
    checkImpl(p->output.size() == 1, e.path, __LINE__,
              "expected exactly one printed value, got " +
                  std::to_string(p->output.size()));
    if (p->output.size() == 1) {
      checkImpl(p->output[0] == e.value, e.path, __LINE__,
                "(got " + std::to_string(p->output[0]) + ", want " +
                    std::to_string(e.value) + ")");
    }
  }
}

}  // namespace

int main() {
  testEncodeDecodeRoundTrip();
  testAssemblerLayout();
  testAssemblerErrors();
  testAssemblerBoundsTheImage();
  testAluSemantics();
  testRegisterZeroIsHardwired();
  testMemoryRoundTrip();
  testStackAndCalls();
  testForwardingReducesStalls();
  testBranchPredictorHelps();
  testCachePatternMatters();
  testDemandPagingAndEviction();
  testWarmStartRemovesColdFaults();
  testWarmStartUnderMemoryPressure();
  testTlbCounters();
  testSegmentationFault();
  testMisalignedAccess();
  testWriteToTextIsRejected();
  testSchedulersMakeProgress();
  testQuantumControlsSwitchCount();
  testPriorityOrdering();
  testMlfqDemotesCpuBoundJob();
  testBtbAliasingBetweenProcesses();
  testRecycledFramesDoNotLeakData();
  testConfigValidation();
  testCycleAccounting();
  testProgramFilesAssembleAndRun();

  std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
