#include "stats.h"

namespace cpusim {

std::vector<std::pair<std::string, uint64_t>> statPairs(const Stats& s) {
  std::vector<std::pair<std::string, uint64_t>> out;
#define CPUSIM_PAIR(name, desc) out.emplace_back(desc, s.name);
  CPUSIM_STAT_FIELDS(CPUSIM_PAIR)
#undef CPUSIM_PAIR
  return out;
}

}  // namespace cpusim
