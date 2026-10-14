// fuzz_assembler.cpp -- fuzz the assembler and simulator with hostile input.
//
// A .asm file is untrusted input: the assembler must reject anything it can't
// parse with an AssembleError, never crash or read out of bounds. A program
// that does assemble is then run for a bounded number of cycles, so the
// simulated MMU's isolation is exercised by arbitrary guest code too.
//
// Two ways to run it:
//
//   make fuzz                        built-in mutation driver (any compiler),
//                                    seeded from programs/*.asm; build with
//                                    sanitizers so memory errors abort
//   clang++ -fsanitize=fuzzer,address -DCPUSIM_LIBFUZZER ...
//                                    coverage-guided, via libFuzzer
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "assembler.h"
#include "simulator.h"

using namespace cpusim;

namespace {
unsigned long gAssembled = 0;  // inputs that reached the simulator
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string source(reinterpret_cast<const char*>(data), size);
  Program prog;
  try {
    prog = assemble(source, "fuzz");
  } catch (const AssembleError&) {
    return 0;  // rejected cleanly
  }

  ++gAssembled;
  SimConfig cfg;
  cfg.maxCycles = 5'000;
  cfg.mem.frames = 4;  // small, so eviction and swap paths run too
  Simulator sim(cfg);
  sim.load(prog);
  sim.run();
  return 0;
}

#ifndef CPUSIM_LIBFUZZER

namespace {

const char* const kTokens[] = {
    "add",  "sub",   "lw",     "sw",   "beq",  "bne",    "jal", "jr",
    "li",   "la",    "addi",   "halt", "yield", ".word", ".space", ".data",
    ".text", "r0",   "r15",    "r16",  "r-1",  "label:", "label", "0x",
    "0xFFFFFFFF", "-2147483648", "4294967296", "99999999999999999999",
    "label+8", "label-4", "(", ")", ",", ":", "#", ";", "\n", " ", "\t",
    "\0", "\xff",
};

std::string mutate(const std::string& in, std::mt19937& rng) {
  std::string s = in;
  std::uniform_int_distribution<int> op(0, 5);
  const int edits = 1 + static_cast<int>(rng() % 8);
  for (int e = 0; e < edits; ++e) {
    const size_t pos = s.empty() ? 0 : rng() % (s.size() + 1);
    switch (op(rng)) {
      case 0:  // flip a byte
        if (!s.empty() && pos < s.size()) s[pos] = static_cast<char>(rng());
        break;
      case 1:  // insert a token
        s.insert(pos, kTokens[rng() % (sizeof(kTokens) / sizeof(*kTokens))]);
        break;
      case 2:  // delete a span
        if (pos < s.size()) s.erase(pos, 1 + rng() % 16);
        break;
      case 3: {  // duplicate a span
        if (pos < s.size()) s.insert(pos, s.substr(pos, 1 + rng() % 64));
        break;
      }
      case 4:  // truncate
        s.resize(pos);
        break;
      default:  // random bytes
        for (int i = 0, n = 1 + static_cast<int>(rng() % 8); i < n; ++i) {
          s.insert(s.begin() + static_cast<std::ptrdiff_t>(pos), static_cast<char>(rng()));
        }
        break;
    }
  }
  return s;
}

std::string readFile(const char* path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

// usage: fuzz_assembler ITERATIONS SEED seed.asm [seed.asm ...]
int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: %s ITERATIONS SEED seed.asm [...]\n", argv[0]);
    return 2;
  }
  const unsigned long iterations = std::strtoul(argv[1], nullptr, 10);
  std::mt19937 rng(static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 10)));

  std::vector<std::string> corpus;
  for (int i = 3; i < argc; ++i) corpus.push_back(readFile(argv[i]));
  corpus.emplace_back();  // the empty file

  for (unsigned long i = 0; i < iterations; ++i) {
    const std::string input = mutate(corpus[rng() % corpus.size()], rng);
    LLVMFuzzerTestOneInput(reinterpret_cast<const uint8_t*>(input.data()), input.size());
  }
  std::printf("fuzz: %lu inputs (%lu assembled and ran), no crashes\n",
              iterations, gAssembled);
  return 0;
}

#endif  // CPUSIM_LIBFUZZER
