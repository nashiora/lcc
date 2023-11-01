#ifndef LCC_CODEGEN_INSTRUCTION_SELECTION_HH
#define LCC_CODEGEN_INSTRUCTION_SELECTION_HH

#include <lcc/utils.hh>
#include <lcc/context.hh>
#include <lcc/codegen/mir.hh>

namespace lcc {
namespace isel {

/// `while`-like iteration over a template parameter pack.
template<typename... pack>
constexpr void While(bool& cond, auto&& lambda) {
    auto impl = [&]<typename t>() {
        if (not cond) return false;
        lambda.template operator()<t>();
        return true;
    };

    (impl.template operator()<pack>() and ...);
}

template<typename... pack>
constexpr void Foreach(auto&& lambda) {
    (lambda.template operator()<pack>(), ...);
}

template<i64 imm = 0>
struct Immediate {
    static constexpr i64 immediate = imm;
};

template<typename kind, typename value>
struct Operand{};

// Operand reference, by index.
template <usz idx>
struct o {
    static constexpr usz index = idx;
};

template<usz opcode_, typename... operands>
struct Inst {
    static constexpr usz opcode = opcode_;
};

template <typename in, typename out>
struct Pattern {
    using input = in;
    using output = out;

    // FIXME: Ideally, we would not iterate each function entirely for every
    // pattern we want to match. Instead, we would iterate each pattern while
    // we are iterating instructions until we find a matching one. However, I
    // cannot figure out how to iterate types, so I'll leave that for somebody
    // with double my IQ.
    static void rewrite(MFunction& function) {
        for (auto& block : function.blocks()) {
            for (auto& instruction : block.instructions()) {
                if (instruction.opcode() == in::opcode) {
                    fmt::print("Matching {}!\n", ToString(instruction.kind()));
                }
            }
        }
    }
};

template<typename... Patterns>
struct PatternList {
    static void rewrite(MFunction& function) {
        (Patterns::rewrite(function), ...);
    }
};


} // namespace isel

void select_instructions(const Context* const ctx, MFunction& function);

} // namespace lcc

#endif /* LCC_CODEGEN_INSTRUCTION_SELECTION_HH */
