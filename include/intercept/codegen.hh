#ifndef LCC_CODEGEN_HH
#define LCC_CODEGEN_HH

#include <intercept/ast.hh>
#include <lcc/module.hh>

namespace lcc::intercept {
/// Generate an LCC module from an intercept module. It is
/// UB to call this more than once on the same module.
auto CodeGen(std::unique_ptr<Module> intercept_module) -> std::unique_ptr<lcc::Module>;

} // namespace lcc::intercept

#endif // LCC_CODEGEN_HH
