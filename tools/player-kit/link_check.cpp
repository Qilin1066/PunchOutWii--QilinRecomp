// Stands in for the code qilin lift makes from main.dol (empty tables), so
// the player kit can be linked without it: CI builds this to show that
// libpunchout_kit.a and its system libraries are complete.
#include "qilin/rt/system.hpp"

namespace qilin::gen {
extern const ::qilin::rt::FunctionEntry kFunctions[] = {{0, nullptr}};
extern const unsigned kFunctionCount = 0;
extern const ::qilin::rt::ReplacedEntry kReplaced[] = {{0, nullptr, false}};
extern const unsigned kReplacedCount = 0;
}  // namespace qilin::gen
