#include <intercept/codegen.hh>
#include <lcc/context.hh>

namespace intc = lcc::intercept;

/// Not in intercept namespace so `Type` etc. resolve to
/// lcc::Type instead of intc::Type.
namespace lcc {
namespace {
struct CG {
    std::unique_ptr<lcc::Module> mod{};
    std::unique_ptr<intc::Module> int_mod{};
    Context* ctx{};

    CG() = default;
    CG(CG&) = delete;
    CG(CG&&) = delete;
    CG& operator=(CG&) = delete;
    CG& operator=(CG&&) = delete;

    void GenerateModule();
};
} // namespace
} // namespace lcc

auto intc::CodeGen(std::unique_ptr<Module> intercept_module) -> std::unique_ptr<lcc::Module> {
    LCC_ASSERT(not intercept_module->context()->has_error());
    lcc::CG cg;
    cg.mod = std::make_unique<lcc::Module>(intercept_module->context());
    cg.int_mod = std::move(intercept_module);
    cg.ctx = cg.mod->context();
    cg.GenerateModule();
    return cg.int_mod->context()->has_error() ? std::move(cg.mod) : std::unique_ptr<lcc::Module>{};
}

void lcc::CG::GenerateModule() {
    /// Create all functions.
    for (auto* f : int_mod->functions()) {
        auto func = new (*mod) Function(
            ctx,
            f->mangled_name(),
            as<FunctionType>(f->type()->lcc()),
            f->linkage(),
            CallConv::C,
            f->location()
        );
    }
}
