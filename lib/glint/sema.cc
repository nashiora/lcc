#include <lcc/context.hh>
#include <lcc/utils.hh>
#include <lcc/utils/macros.hh>
#include <object/elf.h>
#include <object/elf.hh>

#include <glint/ast.hh>
#include <glint/module_description.hh>
#include <glint/sema.hh>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

/// ===========================================================================
///  Helpers
/// ===========================================================================
auto lcc::glint::Sema::AnalyseAndDiscard(Expr** expr) -> bool {
    if (not Analyse(expr)) return false;
    Discard(expr);
    return true;
}

auto lcc::glint::Sema::Convert(Expr** expr, Type* type) -> bool {
    if ((*expr)->sema_errored()) return true;
    return ConvertImpl<true>(expr, type) >= 0;
}

/// For an explanation of the return value of this function, see
/// the comment on the declaration of TryConvert().
template <bool PerformConversion>
auto lcc::glint::Sema::ConvertImpl(lcc::glint::Expr** expr_ptr, lcc::glint::Type* to) -> int {
    LCC_ASSERT(expr_ptr and *expr_ptr and to, "Pointers mustn't be null");

    enum : int {
        TypesContainErrors = -2,
        ConversionImpossible = -1,
        NoOp = 0,
    };

    // Caching from always caused a whole bunch of problems so this is the
    // never-cache solution while still providing a nice name
#define from ((*expr_ptr)->type())

    /// Cannot convert if the types contain errors.
    if (from->sema_errored() or to->sema_errored())
        return TypesContainErrors;

    /// This is so we don’t forget that we’ve applied lvalue-to-rvalue
    /// conversion and raised the score by one.
    int score = 0;
    auto Score = [&](int i) {
        LCC_ASSERT(i, "Score must be 1 or greater. Use the enum constants above for values <= 0");
        return i + int(score);
    };

    /// Any type can be converted to void.
    if (to->is_void()) return NoOp;

    /// Any type can be converted to itself.
    if (Type::Equal(from, to)) return NoOp;

    /// All conversions beside reference binding require lvalue-to-rvalue conversion.
    if (to->is_reference() and Type::Equal(from, to->elem())) {
        if ((*expr_ptr)->is_lvalue()) {
            if constexpr (PerformConversion)
                WrapWithCast(expr_ptr, to, CastKind::LValueToReference);
            return NoOp;
        }

        return ConversionImpossible;
    }

    /// Lvalue to rvalue conversion is required.
    score += (*expr_ptr)->is_lvalue();
    if constexpr (PerformConversion)
        LValueToRValue(expr_ptr, false);

    /// Get reference-to-reference conversions out of the way early.
    if (from->is_reference() and to->is_reference()) {
        /// A reference can be converted to the same reference.
        if (Type::Equal(from, to)) return NoOp;

        /// References to arrays can be converted to references to
        /// the first element.
        auto* arr = cast<ArrayType>(from->elem());
        if (arr and Type::Equal(arr->element_type(), to->elem())) {
            if constexpr (PerformConversion) InsertImplicitCast(expr_ptr, to);
            return Score(1);
        }

        return ConversionImpossible;
    }

    /// Strip reference from `from` if need be.
    if (auto* ref = cast<ReferenceType>(from)) {
        score += 1;
        if constexpr (PerformConversion) LValueToRValue(expr_ptr);
    }

    // Function types can be converted to their corresponding function pointer
    // types.
    if (from->is_function() and to->is_pointer() and Type::Equal(to->elem(), from)) {
        if constexpr (PerformConversion)
            InsertImplicitCast(expr_ptr, to);
        return NoOp;
    }

    // Try deproceduring (convert a function into a call to that function).
    if (Deproceduring(expr_ptr)) return Score(1);

    /// Now check if the types are equal. In many cases, lvalue-to-rvalue
    /// conversion is all we need.
    if (Type::Equal(from, to)) return NoOp;

    /// Pointer to pointer conversions.
    if (from->is_pointer() and to->is_pointer()) {
        /// Pointers to arrays are convertible to pointers to the first element.
        auto* arr = cast<ArrayType>(from->elem());
        if (arr and Type::Equal(arr->element_type(), to->elem())) {
            if constexpr (PerformConversion)
                InsertImplicitCast(expr_ptr, to);
            return Score(1);
        }

        /// Any pointer is convertible to `@void`.
        if (Type::Equal(to, Type::VoidPtr)) {
            if constexpr (PerformConversion)
                InsertImplicitCast(expr_ptr, to);
            return Score(1);
        }
    }

    // Array to array conversions.
    if (from->is_array() and to->is_array()) {
        auto* from_arr = as<ArrayType>(from);
        auto* to_arr = as<ArrayType>(to);

        // If the array we are converting from is larger than the resulting array,
        // it wouldn't fit and that conversion is impossible.
        if (from_arr->dimension() > to_arr->dimension())
            return ConversionImpossible;

        // FIXME: We kind of need to check that the base types are convertible,
        // but, uhhh, we can't really do that right now without an expression of
        // that type due to how Convert works ... I wonder what idiot built it
        // that way.

        if constexpr (PerformConversion)
            InsertImplicitCast(expr_ptr, to);
        return Score(1);
    }

    /// Function types can be converted to their corresponding function types.
    if (
        from->is_function() and to->is_pointer()
        and Type::Equal(to->elem(), from)
    ) {
        if constexpr (PerformConversion)
            InsertImplicitCast(expr_ptr, to);
        return NoOp;
    }

    /// Integer to boolean and vis versa implicit conversions.
    if (
        (from->is_integer() and to->is_bool())
        or (from->is_bool() and to->is_integer())
    ) {
        if constexpr (PerformConversion)
            InsertImplicitCast(expr_ptr, to);
        return Score(1);
    }

    // Integer to integer
    //
    // For portability, we would ideally not make any assumptions about
    // the size of `int`, but the issue with that is that it would make
    // most code rather cumbersome to write as you’d have to, e.g., cast
    // an `i16` to `int` manually. C FFI types suffer from similar problems,
    // so we just use their width on the target.
    if (from->is_integer() and to->is_integer()) {
        // Integer types are always convertible to each other if the value is
        // known at compile time and in range for the type it is being converted
        // to.
        EvalResult res;
        if ((*expr_ptr)->evaluate(context, res, false)) {
            /// Note: We currently don’t support integer constants larger than 64
            /// bits internally, so if the type has a bit width larger than 64, it
            /// will always fit.
            auto val = res.as_int();

            /// Signed to Unsigned Conversion
            if (val.slt(0) and to->is_unsigned_int(context)) return ConversionImpossible;

            /// Unsigned to Unsigned Conversion
            auto bits = to->size(context);
            if (from->is_unsigned_int(context)
                and bits < 64
                and val > u64(utils::MaxBitValue(bits)))
                return ConversionImpossible;

            if constexpr (PerformConversion) {
                InsertImplicitCast(expr_ptr, to);
                *expr_ptr = new (mod) ConstantExpr(*expr_ptr, res);
            }
            return Score(1);
        }

        // Otherwise, if not known at compile-time, we will just go by what
        // doesn't cause a memory error. If it fits, it ships.
        if (from->size(context) <= to->size(context)) {
            if constexpr (PerformConversion)
                InsertImplicitCast(expr_ptr, to);
            return Score(1);
        }

        return ConversionImpossible;
    }

    /// Try deproceduring one last time.
    if (Deproceduring(expr_ptr)) return Score(1);

#undef from

    return ConversionImpossible;
}

void lcc::glint::Sema::ConvertOrError(Expr** expr, Type* to) {
    if (not Convert(expr, to)) Error(
        (*expr)->location(),
        "Expression is not convertible to type {}",
        to
    );
}

auto lcc::glint::Sema::ConvertToCommonType(Expr** a, Expr** b) -> bool {
    // An integer literal should always be converted into the type of the
    // other side, favoring the left hand side when ambiguous.
    bool a_is_literal = is<IntegerLiteral>(*a);
    bool b_is_literal = is<IntegerLiteral>(*b);
    bool both_literals = a_is_literal and b_is_literal;
    if (not both_literals) {
        if (a_is_literal)
            return Convert(a, (*b)->type());
        if (b_is_literal)
            return Convert(b, (*a)->type());
    }
    return Convert(a, (*b)->type()) or Convert(b, (*a)->type());
}

auto lcc::glint::Sema::DeclTypeDecay(Type* type) -> Type* {
    return type->is_function() ? Ptr(type) : type;
}

auto lcc::glint::Sema::Deproceduring(Expr** expr_ptr) -> bool {
    /// This conversion only applies to functions and function pointers.
    auto* expr = *expr_ptr;
    auto* ty = expr->type();
    if (not ty->is_function()
        and (not ty->is_pointer() or not ty->elem()->is_function()))
        return false;

    /// Declarations are never deprocedured automatically.
    if (is<Decl>(expr)) return false;
    /// Block expressions are never deprocedured automatically.
    if (is<BlockExpr>(expr)) return false;

    /// Functions that take arguments are not affected.
    auto* ftype = cast<FuncType>(ty->is_function() ? ty : ty->elem());
    if (not ftype->params().empty()) return false;

    /// Otherwise, insert a call.
    *expr_ptr = new (mod) CallExpr(expr, {}, expr->location());
    Analyse(expr_ptr);
    return true;
}

void lcc::glint::Sema::Discard(Expr** expr_ptr) {
    auto* expr = *expr_ptr;

    /// If the expression returns void, or has an error, ignore it.
    if (not expr->ok() or expr->type()->is_void()) return;

    /// If the expression is a call to a function not marked
    /// as discardable, issue an error.
    if (auto* call = cast<CallExpr>(expr)) {
        auto* ftype = call->callee_type();
        if (not ftype->has_attr(FuncAttr::Discardable)) Error(
            call->location(),
            "Discarding return value of function not marked as 'discardable'"
        );
    }

    /// Otherwise, perform deproceduring. For now, we only apply
    /// deproceduring exactly once. If you need more, you can always
    /// use `()` to call the function.
    if (Deproceduring(expr_ptr)) return;

    /// Otherwise, issue a warning if this expression does not have
    /// side effects.
    if (not HasSideEffects(expr)) Warning(
        expr->location(),
        "Expression result unused"
    );
}

auto lcc::glint::Sema::EvaluateAsInt(Expr* expr, Type* int_type, aint& out) -> bool {
    EvalResult res;
    if (not expr->evaluate(context, res, true)) return false;

    /// Must be an int.
    if (not res.is_int()) {
        Error(expr->location(), "Expression is not an integer constant expression");
        return false;
    }

    /// Print a diagnostic if the thing doesn’t fit.
    bool ok = true;
    auto bits = int_type->size(context);
    aint val = res.as_int();
    auto TooLarge = [&]<typename Int>(auto cb) {
        out = std::invoke(cb, val, bits);
        if (std::invoke(cb, out, 64) != std::invoke(cb, val, 64)) {
            ok = false;
            Error(
                expr->location(),
                "Value {} of integer constant does not fit in an {}",
                Int(val),
                int_type
            );
        }
    };

    /// Check that the value fits in the integer type.
    bool is_signed = int_type->is_signed_int(context);
    LCC_ASSERT(bits <= 64, "Bit width of integer type in constant expression must be 64 or less");
    if (is_signed) utils::invoke_template<i64>(TooLarge, &aint::sext);
    else utils::invoke_template<u64>(TooLarge, &aint::zext);
    return ok;
}

auto lcc::glint::Sema::HasSideEffects(Expr* expr) -> bool {
    switch (expr->kind()) {
        /// These always have side effects.
        case Expr::Kind::While:
        case Expr::Kind::For:
        case Expr::Kind::Return:
        case Expr::Kind::TypeDecl:
        case Expr::Kind::TypeAliasDecl:
        case Expr::Kind::VarDecl:
        case Expr::Kind::FuncDecl:
        case Expr::Kind::EnumeratorDecl:
            return true;

        /// These never have side effects.
        case Expr::Kind::IntegerLiteral:
        case Expr::Kind::StringLiteral:
        case Expr::Kind::OverloadSet:
        case Expr::Kind::NameRef:
        case Expr::Kind::Module:
        case Expr::Kind::Type:
        case Expr::Kind::Sizeof:
        case Expr::Kind::Alignof:
            return false;

        /// For these, it depends.
        case Expr::Kind::Cast:
            return HasSideEffects(as<CastExpr>(expr)->operand());

        case Expr::Kind::Unary:
            return HasSideEffects(as<UnaryExpr>(expr)->operand());

        case Expr::Kind::MemberAccess:
            return HasSideEffects(as<MemberAccessExpr>(expr)->object());

        case Expr::Kind::CompoundLiteral:
            return rgs::any_of(as<CompoundLiteral>(expr)->values(), HasSideEffects);

        case Expr::Kind::Block:
            return rgs::any_of(as<BlockExpr>(expr)->children(), HasSideEffects);

        case Expr::Kind::EvaluatedConstant: {
            auto* c = as<ConstantExpr>(expr);
            return c->expr() and HasSideEffects(c->expr());
        }

        case Expr::Kind::Binary: {
            auto* b = as<BinaryExpr>(expr);
            if (HasSideEffects(b->lhs()) or HasSideEffects(b->rhs())) return true;
            return b->op() == TokenKind::ColonEq;
        }

        case Expr::Kind::If: {
            auto* i = as<IfExpr>(expr);
            if (HasSideEffects(i->condition())) return true;
            if (HasSideEffects(i->then())) return true;
            return i->otherwise() and HasSideEffects(i->otherwise());
        }

        case Expr::Kind::Call: {
            auto* c = as<CallExpr>(expr);

            if (HasSideEffects(c->callee())) return true;
            if (rgs::any_of(c->args(), HasSideEffects)) return true;

            // Function calls
            auto* callee_ty = c->callee()->type()->strip_pointers_and_references();
            if (callee_ty->is_function()) {
                auto* f = c->callee_type();
                return not f->has_attr(FuncAttr::Pure) and not f->has_attr(FuncAttr::Const);
            }

            return false;
        }

        case Expr::Kind::IntrinsicCall: {
            auto* c = as<IntrinsicCallExpr>(expr);
            switch (c->intrinsic_kind()) {
                case IntrinsicKind::BuiltinDebugtrap:
                case IntrinsicKind::BuiltinMemCopy:
                case IntrinsicKind::BuiltinMemSet:
                case IntrinsicKind::BuiltinSyscall:
                    return true;

                case IntrinsicKind::BuiltinFilename:
                case IntrinsicKind::BuiltinLine:
                    return false;

                case IntrinsicKind::BuiltinInline:
                    if (c->sema_errored()) return true;
                    return HasSideEffects(c->args()[0]);
            }

            LCC_UNREACHABLE();
        }
    }

    LCC_UNREACHABLE();
}

auto lcc::glint::Sema::ImplicitDe_Reference(Expr** expr) -> bool {
    if (is<ReferenceType>((*expr)->type())) {
        /// Don’t strip reference here since we want an lvalue.
        LValueToRValue(expr, false);
        WrapWithCast(
            expr,
            as<TypeWithOneElement>((*expr)->type())->element_type(),
            CastKind::ReferenceToLValue
        );
    }

    return (*expr)->is_lvalue();
}

auto lcc::glint::Sema::ImplicitDereference(Expr** expr) -> bool {
    if (is<ReferenceType>((*expr)->type())) {
        /// Don’t strip reference here since we want an lvalue.
        LValueToRValue(expr, false);
        WrapWithCast(
            expr,
            as<TypeWithOneElement>((*expr)->type())->element_type(),
            CastKind::ReferenceToLValue
        );
    }

    while (is<PointerType>((*expr)->type())) {
        *expr = new (mod) UnaryExpr(
            TokenKind::At,
            *expr,
            false,
            (*expr)->location()
        );

        LCC_ASSERT(Analyse(expr));
    }

    return (*expr)->is_lvalue();
}

void lcc::glint::Sema::InsertImplicitCast(Expr** expr_ptr, Type* ty) {
    WrapWithCast(expr_ptr, ty, CastKind::ImplicitCast);
}

void lcc::glint::Sema::InsertPointerToIntegerCast(Expr** operand) {
    if ((*operand)->type()->is_pointer())
        InsertImplicitCast(operand, Type::Int);
}

void lcc::glint::Sema::LValueToRValue(Expr** expr, bool strip_ref) {
    if ((*expr)->sema_errored()) return;

    // This converts the type of a member access of a sum type into the type
    // of the member it is accessing.
    // This matters because when we do something like `bar.x := 69;`, we need
    // to access both the `tag` and `data` of `bar`, so we need the member
    // access of it's member to actually return an lvalue to `bar`, rather
    // than an lvalue to the member itself. But, when we do lvalue to rvalue
    // conversion on this member access, we actually want to access the member
    // itself (and not the value of `bar`), so the type is changed to reflect
    // the fact that we are only accessing the single member (even though we
    // will likely end up accessing the underlying object in order to check
    // that the tag is valid, for example). This is just a reflection of the
    // type of the value this member access expression returns.
    // NOTE: This may not be /exactly/ correct when it comes to the type
    // semantics of the language /iff/ we didn't have ways to know that the
    // underlying object the member access is accessing is of a sum type.
    {
        if (auto* m = cast<MemberAccessExpr>(*expr)) {
            if (auto* s = cast<SumType>(m->type())) {
                auto mindex = m->member();
                // TODO: "1" is actually index of ".data" in underlying struct type.
                m->finalise(s->struct_type(), 1);
                m->type(s->members().at(mindex).type);
            }
        }
    }

    if ((*expr)->is_lvalue())
        WrapWithCast(expr, (*expr)->type(), CastKind::LValueToRValueConv);

    if (strip_ref and is<ReferenceType>((*expr)->type())) {
        WrapWithCast(
            expr,
            as<TypeWithOneElement>((*expr)->type())->element_type(),
            CastKind::ReferenceToLValue
        );

        LValueToRValue(expr);
    }
}

auto lcc::glint::Sema::Ptr(Type* ty) -> PointerType* {
    Type* ptr = new (mod) PointerType(ty, ty->location());
    Analyse(&ptr);
    return as<PointerType>(ptr);
}

auto lcc::glint::Sema::Ref(Type* ty) -> ReferenceType* {
    Type* ref = new (mod) ReferenceType(ty, ty->location());
    Analyse(&ref);
    return as<ReferenceType>(ref);
}

auto lcc::glint::Sema::TryConvert(Expr** expr, Type* type) -> int {
    return ConvertImpl<false>(expr, type);
}

void lcc::glint::Sema::WrapWithCast(Expr** expr_ptr, Type* type, CastKind kind) {
    Expr* expr = new (mod) CastExpr(
        *expr_ptr,
        type,
        kind,
        (*expr_ptr)->location()
    );

    Analyse(&expr);
    *expr_ptr = expr;
}

/// ===========================================================================
///  Core
/// ===========================================================================
void lcc::glint::Sema::Analyse(Context* ctx, Module& m, bool use_colours) {
    if (ctx->has_error()) return;
    Sema s{ctx, m, use_colours};
    return s.AnalyseModule();
}

auto lcc::glint::Sema::try_get_metadata_blob_from_object(
    const Module::Ref& import,
    const std::string& include_dir,
    std::vector<std::string>& paths_tried
) -> bool {
    auto path_base0 = include_dir + std::filesystem::path::preferred_separator + import.name;
    auto path_base1 = include_dir + std::filesystem::path::preferred_separator + "lib" + import.name;
    auto paths = {
        path_base0 + ".o",
        path_base0 + ".obj",
        path_base0 + ".a",
        path_base1 + ".o",
        path_base1 + ".obj",
        path_base1 + ".a",
    };
    for (auto p : paths) {
        paths_tried.push_back(p);
        if (std::filesystem::exists(p)) {
            fmt::print("Found IMPORT {} at {}\n", import.name, p);
            // Open file, get contents
            auto object_file = File::Read(p);
            LCC_ASSERT(
                not object_file.empty(),
                "Found object file for module {} at {}, but the file is empty",
                import.name,
                p
            );
            // Determine file-type via magic bytes or extension
            std::vector<u8> metadata_blob{};
            if (
                object_file.size() >= sizeof(elf64_header)
                and object_file.at(0) == 0x7f and object_file.at(1) == 'E'
                and object_file.at(2) == 'L' and object_file.at(3) == 'F'
            ) {
                auto section = elf::get_section_from_blob(
                    object_file,
                    metadata_section_name
                );
                metadata_blob = std::move(section.contents());
            } else LCC_ASSERT(
                false,
                "Unrecognized file format of module {} at {}",
                import.name,
                p
            );
            // Very basic validation pass
            LCC_ASSERT(
                not metadata_blob.empty(),
                "Didn't properly get metadata (it's empty) for module {} at {}",
                import.name,
                p
            );
            LCC_ASSERT(
                metadata_blob.at(0) == ModuleDescription::default_version
                    and metadata_blob.at(1) == ModuleDescription::magic_byte0
                    and metadata_blob.at(2) == ModuleDescription::magic_byte1
                    and metadata_blob.at(3) == ModuleDescription::magic_byte2,
                "Metadata for module {} at {} has invalid magic bytes",
                import.name,
                p
            );
            // Deserialise metadata blob into a module
            // FIXME: (this module? or a new module?)
            return mod.deserialise(context, metadata_blob);
        }
    }
    return false;
}

auto lcc::glint::Sema::try_get_metadata_blob_from_gmeta(
    const Module::Ref& import,
    const std::string& include_dir,
    std::vector<std::string>& paths_tried
) -> bool {
    auto path = include_dir
              + std::filesystem::path::preferred_separator
              + import.name + std::string(metadata_file_extension);

    paths_tried.push_back(path);
    if (std::filesystem::exists(path)) {
        fmt::print("Found IMPORT {} at {}\n", import.name, path);

        // Open file, get contents
        auto gmeta_file = File::Read(path);

        std::vector<u8> metadata_blob{};
        metadata_blob.insert(metadata_blob.end(), gmeta_file.begin(), gmeta_file.end());
        LCC_ASSERT(
            not metadata_blob.empty(),
            "Found gmeta file for module {} at {}, but the file is empty",
            import.name,
            path
        );
        LCC_ASSERT(
            metadata_blob.at(0) == ModuleDescription::default_version
                and metadata_blob.at(1) == ModuleDescription::magic_byte0
                and metadata_blob.at(2) == ModuleDescription::magic_byte1
                and metadata_blob.at(3) == ModuleDescription::magic_byte2,
            "Metadata for module {} at {} has invalid magic bytes",
            import.name,
            path
        );
        return mod.deserialise(context, metadata_blob);
    }

    return false;
}

auto lcc::glint::Sema::try_get_metadata_blob_from_assembly(
    const Module::Ref& import,
    const std::string& include_dir,
    std::vector<std::string>& paths_tried
) -> bool {
    auto path = include_dir
              + std::filesystem::path::preferred_separator
              + import.name + ".s";

    paths_tried.push_back(path);
    if (std::filesystem::exists(path)) {
        // TODO: We can kind of cheat and just direct seek to `.section .glint`,
        // then `.byte`, then parse the whole line as comma-separated integer
        // literals forming a stream of bytes.
        LCC_TODO("Parse Glint module metadata from assembly file (alternatively, provide a gmeta or object file)");
    }
    return false;
}

void lcc::glint::Sema::AnalyseModule() {
    // Load imported modules.
    for (auto& import : mod.imports()) {
        bool loaded{false};
        std::vector<std::string> paths_tried{};

        for (const auto& include_dir : context->include_directories()) {
            loaded = try_get_metadata_blob_from_gmeta(import, include_dir, paths_tried)
                  or try_get_metadata_blob_from_object(import, include_dir, paths_tried)
                  or try_get_metadata_blob_from_assembly(import, include_dir, paths_tried);
            if (loaded) break;
        }

        if (not loaded) {
            // TODO: Link/reference help documentation on how to point the compiler to
            // look in the proper place for Glint metadata, and how to produce it.
            Error(
                {},
                "Could not find imported module {} in any include directory.\n"
                "Paths tried:\n"
                "{}",
                import.name,
                fmt::join(paths_tried, "\n")
            );
            Diag::Note(
                context,
                import.location,
                "Imported here"
            );
            std::exit(1);
        }
    }

    /// Analyse the signatures of all functions. This must be done
    /// before analysing bodies since, in order to perform overload
    /// resolution properly, we first need to apply decltype decay
    /// to all parameters (e.g. convert parameters of function type
    /// to function pointers etc.).
    for (auto& func : mod.functions()) AnalyseFunctionSignature(func);

    /// Analyse function bodies.
    for (auto& func : mod.functions()) AnalyseFunctionBody(func);
}

void lcc::glint::Sema::AnalyseFunctionBody(FuncDecl* decl) {
    tempset curr_func = decl;
    auto ty = as<FuncType>(decl->type());

    /// If the function has no body, then we’re done.
    if (not decl->body()) return;

    /// Create variable declarations for the parameters.
    for (auto& param : ty->params()) {
        if (param.name.empty()) continue;

        // Check that we don’t already have a declaration with that
        // name in the function scope.
        auto decls = decl->scope()->find(param.name);
        if (not decls.empty()) {
            Error(decls.at(0)->location(), "Declaration conflicts with parameter name");
            Diag::Note(context, param.location, "Parameter declared here");
            continue;
        }

        /// Declare the parameter.
        Expr* d = new (mod) VarDecl(
            param.name,
            param.type,
            {},
            &mod,
            Linkage::LocalVar,
            param.location
        );

        LCC_ASSERT(decl->scope()->declare(context, auto(param.name), as<VarDecl>(d)).is_value());
        Analyse(&d);
        decl->param_decls().push_back(as<VarDecl>(d));
    }

    // Gets rid of parameter dynamic array declarations that were falsely
    // recorded as dangling (parameters owned by caller).
    decl->dangling_dynarrays().clear();

    /// Analyse the body.
    Analyse(&decl->body(), ty->return_type());

    if (context->has_error()) return;

    // Report every dynamic array declared in this function (and that is not
    // returned) which doesn't have NoLongerViable status (aka freed).
    // Parameters are owned by caller, don't count those.
    for (auto dynarray : decl->dangling_dynarrays()) {
        // TODO: Maybe a warning?
        Error(
            dynarray->location(),
            "You forgot to free this dynamic array"
        );
    }

    /// The last expression in a function must be a return expression or convertible
    /// to the return type of the function. If it is a return expression, then it has
    /// already been checked for that, so ignore that case.
    ///
    /// Note that the body may be a block, in which case we should check the last
    /// expression of the block rather than just the block itself.
    if (not ty->return_type()->is_void()) {
        Expr** last{};
        if (auto block = cast<BlockExpr>(decl->body())) {
            if (block->children().empty()) {
                // For anything except the top-level function, if there is an expected
                // return value, there has to be one, otherwise it's an error.
                if (decl->name() != "main") {
                    Error(decl->location(), "Function `{}` has non-void return type, and must return a value", decl->name());
                    return;
                }

                // For the top level function of executable programs, a return value is be
                // created if a valid one is not present.
                auto inserted_return_value = new (mod) IntegerLiteral(0, {});
                block->add(new (mod) ReturnExpr(inserted_return_value, {}));
            }

            last = block->last_expr();
        } else last = &decl->body();

        if (is<ReturnExpr>(*last)) return;

        if (not Convert(last, ty->return_type())) {
            Error(
                (*last)->location(),
                "Type of last expression {} is not convertible to return type {}",
                (*last)->type(),
                ty->return_type()
            );
            context->set_error();
            return;
        }

        LValueToRValue(last);

        // Insert a `ReturnExpr` which returns `last`.
        if (is<BlockExpr>(decl->body())) {
            *last = new (mod) ReturnExpr(*last, {});
        } else {
            decl->body() = new (mod) ReturnExpr(*last, {});
        }
    } else {
        if (auto block = cast<BlockExpr>(decl->body())) {
            if (not block->children().size() or not is<ReturnExpr>(*block->last_expr()))
                block->add(new (mod) ReturnExpr(nullptr, {}));
        } else {
            // TODO: If a function with void return type and a non-block body
            // (i.e. `foo : void() = bar 42;`) does not have a return expression, we
            // must replace the body with a block containing the non-block body
            // followed by an empty return expression.
        }

        Discard(&decl->body());
    }
}

void lcc::glint::Sema::AnalyseFunctionSignature(FuncDecl* decl) {
    /// Set a name for the decl if it’s empty.
    if (decl->name().empty()) decl->name(mod.unique_function_name());

    /// Typecheck the function type.
    Analyse(decl->type_ref());

    /// Used attribute is ignored on functions that aren’t internal. If
    /// the function is internal, then set the linkage to used so it isn’t
    /// deleted by the optimiser.
    auto ty = as<FuncType>(decl->type());
    if (ty->has_attr(FuncAttr::Used)) {
        if (decl->linkage() != Linkage::Internal)
            Warning(decl->location(), "'used' has no effect on this function");
        else decl->linkage(Linkage::Used);
    }
}

/// ===========================================================================
///  Analysing Expressions
/// ===========================================================================
/// Invariants:
///
///   - If an expression is marked as `Done` or `Errored`, it will
///     not be analysed again.
///
///   - If an expression is a `TypedExpr`, its type is analysed first.
///
///   - When this function returns, the expression pointed to by
///     `expr_pointer` will be marked as `Done`, unless it is already
///     marked as `Errored`. This may not end up being the same
///     expression as `expr` in the body of this function.
///
/// \param expr_ptr A pointer to the expression to analyse.
/// \param expected_type The type used for top-down inference. May be null.
/// \return (*expr_ptr)->ok().
auto lcc::glint::Sema::Analyse(Expr** expr_ptr, Type* expected_type) -> bool {
    auto expr = *expr_ptr;

    /// Don’t analyse the same expression twice.
    if (expr->sema() != SemaNode::State::NotAnalysed) return expr->ok();
    expr->set_sema_in_progress();

    /// Analyse the type if there is one.
    if (auto tc = cast<TypedExpr>(expr))
        Analyse(tc->type_ref());

    /// Analyse the expression itself.
    switch (expr->kind()) {
        /// The condition of a loop must be convertible to bool.
        case Expr::Kind::For: {
            auto f = as<ForExpr>(expr);
            AnalyseAndDiscard(&f->init());
            AnalyseAndDiscard(&f->increment());
            [[fallthrough]];
        }

        case Expr::Kind::While: {
            auto l = as<Loop>(expr);
            Analyse(&l->condition());
            if (not Convert(&l->condition(), Type::Bool)) Error(
                l->location(),
                "Invalid type for loop condition: {}",
                l->condition()->type()
            );
            LValueToRValue(&l->condition());
            AnalyseAndDiscard(&l->body());
        } break;

        /// For return expressions, make sure that the type of the
        /// argument, if any, matches that of the function containing
        /// the return expression.
        case Expr::Kind::Return: {
            /// Check the return value.
            auto r = as<ReturnExpr>(expr);
            auto ret_type = as<FuncType>(curr_func->type())->return_type();
            if (r->value()) Analyse(&r->value(), ret_type);

            // NOTE: Just for forget-to-free diagnostics.
            // If returned value is a dynamic array, remove that dynamic array's
            // declaration from the list of dangling dynamic arrays.
            if (r->value()->type()->is_dynamic_array()) {
                if (auto* nameref = cast<NameRefExpr>(r->value()))
                    std::erase(curr_func->dangling_dynarrays(), nameref->target());
            }

            /// Make sure that it matches the return type.
            if (ret_type->is_void()) {
                /// Note we allow return expressions to have an operand so long
                /// as that operand has type void; this can be the case for e.g.
                /// calls to functions returning void.
                if (r->value() and r->value()->ok() and not r->value()->type()->is_void())
                    Error(r->location(), "Function returning void must not return a value");
            } else {
                if (not r->value()) Error(
                    r->location(),
                    "Non-void function must return a value"
                );
                else if (not Convert(&r->value(), ret_type)) Error(
                    r->location(),
                    "Type of return expression is not convertible to return type {}",
                    ret_type
                );
                LValueToRValue(&r->value());
            }
        } break;

        /// The condition of an if statement must be convertible to bool, and
        /// its type is the common type of the two branches.
        case Expr::Kind::If: {
            auto i = as<IfExpr>(expr);
            Analyse(&i->condition());
            if (not Convert(&i->condition(), Type::Bool)) {
                Error(
                    i->condition()->location(),
                    "Invalid type for if condition: {}",
                    i->condition()->type()
                );
            }
            LValueToRValue(&i->condition());

            /// Analyse the branches.
            Analyse(&i->then());
            if (i->otherwise()) Analyse(&i->otherwise());

            if (not i->then()->ok() or (i->otherwise() and not i->otherwise()->ok()))
                i->set_sema_errored();

            // If both branches exist, and both branches are convertible to a common
            // type, then this IfExpr returns that common type. Otherwise, it's a void
            // expression.
            i->type(Type::Void);
            if (i->then() and i->otherwise()
                and not i->then()->type()->is_void()
                and not i->otherwise()->type()->is_void()) {
                if (ConvertToCommonType(&i->then(), &i->otherwise())) {
                    // fmt::print("THEN\n");
                    // i->then()->print(true);
                    // fmt::print("OTHERWISE\n");
                    // i->otherwise()->print(true);
                    // fmt::print("Common type: {}\n", *i->then()->type());

                    i->type(i->then()->type());
                    // Do LValueToRValue conversion iff one branch is an lvalue.
                    // Otherwise, match lvalue-ness.
                    if (i->then()->is_lvalue() and i->otherwise()->is_lvalue())
                        i->set_lvalue();
                    else if (i->then()->is_lvalue())
                        LValueToRValue(&i->then());
                    else if (i->otherwise()->is_lvalue())
                        LValueToRValue(&i->otherwise());
                }
            }

            if (i->type()->is_void()) {
                Discard(&i->then());
                if (i->otherwise()) Discard(&i->otherwise());
            }
        } break;

        /// The type of a block is the type of its last expression. Type
        /// inference is only used for the last expression in the block.
        case Expr::Kind::Block: {
            auto block = as<BlockExpr>(expr);
            if (block->children().empty()) {
                block->type(Type::Void);
                break;
            }

            for (auto*& child : block->children()) {
                const bool last = &child == block->last_expr();
                if (not Analyse(&child, last ? expected_type : nullptr))
                    block->set_sema_errored();
                if (not last and child->ok())
                    Discard(&child);
            }

            if (not block->sema_errored()) {
                block->set_lvalue(block->children().back()->is_lvalue());
                block->type(block->children().back()->type());
            }
        } break;

        /// This mainly handles explicit casts, which allow more
        /// conversions than implicit casts.
        ///
        /// We don’t ever mark this as errored because there is no
        /// type that we *cannot* cast to, and the type this expr
        /// is supposed to have is known.
        case Expr::Kind::Cast:
            AnalyseCast(as<CastExpr>(expr));
            break;

        /// Intrinsics need to be analysed individually.
        case Expr::Kind::IntrinsicCall:
            AnalyseIntrinsicCall(expr_ptr, as<IntrinsicCallExpr>(expr));
            break;

        /// This is handled by the overload resolution code. We do *not*
        /// pass in an expected type because we do not perform overload
        /// resolution on return types.
        case Expr::Kind::Call:
            AnalyseCall(expr_ptr, as<CallExpr>(expr));
            break;

        /// Analyse local and global variable declarations.
        case Expr::Kind::VarDecl: {
            auto v = as<VarDecl>(expr);

            /// If this has an initialiser, analyse it.
            if (v->init()) {
                /// Obviously, we can only perform top-down type inference
                /// if we’re not already performing bottom-up inference. If
                /// the type is known, make sure that we use a type that is
                /// legal in a declaration for inference.
                const bool infer_type = v->type()->is_unknown();
                Analyse(
                    &v->init(),
                    infer_type ? nullptr : DeclTypeDecay(v->type())
                );

                /// If we’re using type inference, break if there was an
                /// error since we can’t validate the type of this if we
                /// don’t know it. Otherwise, set the type of this to the
                /// type of the initialiser.
                if (infer_type) {
                    if (v->init()->ok()) v->type(v->init()->type());
                    else {
                        v->set_sema_errored();
                        break;
                    }
                }
            }

            /// Check that the type makes sense. In particular, if it is
            /// a function type, convert it to a function pointer type.
            v->type(DeclTypeDecay(v->type()));

            /// Make sure the initialiser is convertible to that type. Note
            /// that, if this fails, we do not mark this node as errored as
            /// its type is well-formed; it’s just the initialiser that has
            /// a problem.
            if (v->init()) {
                if (not Convert(&v->init(), v->type())) Error(
                    v->init()->location(),
                    "Type of initialiser, {}, is not convertible to variable type {}",
                    v->init()->type(),
                    v->type()
                );

                LValueToRValue(&v->init());
            }

            if (v->type()->is_dynamic_array())
                curr_func->dangling_dynarrays().push_back(v);

            v->set_lvalue();
        } break;

        /// These are handled by the code that also handles enums.
        case Expr::Kind::EnumeratorDecl: LCC_UNREACHABLE();

        case Expr::Kind::CompoundLiteral: {
            auto* c = as<CompoundLiteral>(expr);

            /// Analyse all subexpressions.
            for (auto*& child : c->values()) {
                if (Analyse(&child)) LValueToRValue(&child);
                else c->set_sema_errored();
            }

            if (not c->type() and not expected_type) {
                Error(
                    c->location(),
                    "Cannot infer type of Untyped Compound Literal"
                );
            }
            // TODO: If both c->type() and expected_type, Convert to expected_type.
            LCC_ASSERT(Analyse(c->type_ref()));
        } break;

        /// LHS must be a (pointer to a) struct, and the identifier must
        /// exist in the struct.
        case Expr::Kind::MemberAccess: {
            auto* m = as<MemberAccessExpr>(expr);
            /// If there is an error analysing the object, we don’t know
            /// its type and can thus not continue checking this.
            if (not Analyse(&m->object())) {
                m->set_sema_errored();
                break;
            }

            /// Accessing ‘members’ of modules.
            if (is<NameRefExpr>(m->object())
                and is<ModuleExpr>(as<NameRefExpr>(m->object())->target())) {
                // m->name() == name of member we are accessing
                // m->object() == NameRef to module we are accessing
                auto* name_ref = as<NameRefExpr>(m->object());
                auto* module_expr = as<ModuleExpr>(name_ref->target());
                auto* referenced_module = module_expr->mod();
                auto* scope = referenced_module->global_scope();
                // Replace member access with a name ref
                *expr_ptr = new (mod) NameRefExpr(m->name(), scope, m->location());
                AnalyseNameRef(as<NameRefExpr>(*expr_ptr));
                break;
            }

            /// ‘object’ is actually a type name.
            if (is<NameRefExpr>(m->object())
                and is<TypeDecl>(as<NameRefExpr>(m->object())->target())) {
                auto* t = as<TypeDecl>(as<NameRefExpr>(m->object())->target());

                if (is<StructType>(t->type())) {
                    LCC_TODO(
                        "Type introspection for {}; what type do we want to actually return here?\n"
                        "Some sort of struct with type info probably, but only compile-time constants like integer or string literals for now, I'd guess.",
                        *t->type()
                    );
                }

                /// Handle accessing enumerators.
                if (auto e = cast<EnumType>(t->type())) {
                    auto it = rgs::find_if(
                        e->enumerators(),
                        [&](auto&& en) { return en->name() == m->name(); }
                    );
                    if (it == e->enumerators().end()) {
                        Error(m->location(), "Type {} has no enumerator named '{}'", e, m->name());
                        m->set_sema_errored();
                        break;
                    }

                    auto* enumerator = *it;
                    if (enumerator->sema_errored()) {
                        m->set_sema_errored();
                        break;
                    }

                    if (not enumerator->ok()) {
                        Error(
                            m->location(),
                            "Enumerator {} cannot be used before it is defined",
                            enumerator->name()
                        );
                        m->set_sema_errored();
                        break;
                    }

                    m->type(enumerator->type());
                    m->set_sema_done();
                    *expr_ptr = new (mod) ConstantExpr(expr, enumerator->value());
                    break;
                }

                LCC_UNREACHABLE();
            }

            /// Type must be a struct type (or something that represents one, like a
            /// DynamicArrayType or SumType)
            auto* stripped_object_type = m->object()->type()->strip_pointers_and_references();

            // Access to union member
            if (auto* union_type = cast<UnionType>(stripped_object_type)) {
                auto& members = union_type->members();
                auto it = rgs::find_if(
                    members,
                    [&](auto& member) { return member.name == m->name(); }
                );
                if (it == members.end()) {
                    Error(m->location(), "Union {} has no member named '{}'", union_type, m->name());
                    m->set_sema_errored();
                    break;
                }

                auto* cast = new (mod) CastExpr(m->object(), it->type, CastKind::HardCast, m->location());
                cast->set_lvalue(m->object()->is_lvalue());
                *expr_ptr = cast;
                break;
            }

            // Access to sum type member
            if (auto* sum_type = cast<SumType>(stripped_object_type)) {
                auto& members = sum_type->members();
                auto it = rgs::find_if(members, [&](auto& member) { return member.name == m->name(); });
                if (it == members.end()) {
                    Error(m->location(), "Sum type {} has no member named '{}'", sum_type, m->name());
                    m->set_sema_errored();
                    break;
                }

                // NOTE: While the actual type of this member access is the type of the
                // member (as an lvalue), we don't set that here so that we can properly
                // generate the code needed during IRGen by just checking if the lhs of an
                // assignment is a sum type or if a member access itself is of a sum type
                // then we know to add the tag check and default expression path.
                m->type(sum_type);
                // m->type(it->type);

                m->finalise(
                    sum_type->struct_type(),
                    usz(std::distance(members.begin(), it))
                );

                m->set_lvalue();

                // fmt::print("\nOOHWEE\n");
                // mod.print(true);

                // LCC_TODO("SumType Member Access AST Generation");

                // The following
                //   foo : sum { x :cint 0, y :uint 0 };
                // turns into
                //   foo : struct { tag :enum { x:0 y:1 }; data :union { :cint :uint }; }
                //
                // bar :foo;
                //
                // The following
                //   bar.x := 69;
                // should turn into
                //   bar.tag := foo.tag.x;
                //   (:cint.ptr &bar.data) := 69;
                //
                // The following
                //   bar.x;
                // should turn into (if tag, then access)
                //   if (bar.tag = foo.tag.x)
                //     @(:cint.ptr &bar.data);
                //   else default_constant_expression foo.x;
                //
                // It might be interesting to require a constant expression initialiser in
                // sum type declarations and then have an `else` that returns that if the
                // accessed sum type has the wrong data in it.
                //
                // The following
                //   has bar.x;
                // should turn into
                //   bar.tag = foo.tag.x;

                break;
            }

            auto* struct_type = cast<StructType>(stripped_object_type);

            if (not struct_type and is<DynamicArrayType>(stripped_object_type))
                struct_type = as<DynamicArrayType>(stripped_object_type)->struct_type(mod);

            if (not struct_type) {
                Error(
                    m->object()->location(),
                    "LHS of member access must be a struct, but was {}",
                    m->object()->type()
                );

                m->set_sema_errored();
                break;
            }

            /// The struct type must contain the member.
            auto& members = struct_type->members();
            auto it = rgs::find_if(members, [&](auto& member) { return member.name == m->name(); });
            if (it == members.end()) {
                Error(m->location(), "Struct {} has no member named '{}'", struct_type, m->name());
                m->set_sema_errored();
                break;
            }

            /// Set the struct and member index.
            m->finalise(struct_type, usz(std::distance(members.begin(), it)));

            /// Dereference pointers until we have an lvalue to struct. The
            /// member access is an lvalue, iff the struct is an lvalue.
            m->set_lvalue(ImplicitDereference(&m->object()));
            m->type(it->type);
        } break;

        case Expr::Kind::Sizeof: {
            auto* sizeof_expr = as<SizeofExpr>(expr);
            Analyse(sizeof_expr->expr_ref());

            aint value{};
            if (auto* typed_expr = cast<TypedExpr>(sizeof_expr->expr()))
                value = typed_expr->type()->size(context);
            else Error(sizeof_expr->location(), "Unhandled expression in sizeof");

            *expr_ptr = new (mod) IntegerLiteral(value, expr->location());
        } break;

        case Expr::Kind::Alignof: {
            auto* alignof_expr = as<AlignofExpr>(expr);
            Analyse(alignof_expr->expr_ref());

            aint value{};
            if (auto* typed_expr = cast<TypedExpr>(alignof_expr->expr()))
                value = typed_expr->type()->align(context);
            else Error(alignof_expr->location(), "Unhandled expression in alignof");

            *expr_ptr = new (mod) IntegerLiteral(value, expr->location());
        } break;

        /// Validate overload sets.
        case Expr::Kind::OverloadSet: {
            const auto& os = as<OverloadSet>(expr)->overloads();

            /// An overload set must not contain two overloads with the
            /// same parameter types. All function signatures have already
            /// been analysed, so we just need to compare them.
            for (usz i = 0; i < os.size(); i++) {
                auto* oi = os[i];
                auto oi_params = oi->param_types();
                for (usz j = i + 1; j < os.size(); j++) {
                    auto* oj = os[j];
                    auto oj_params = oj->param_types();

                    /// Different number of parameters means these two can’t be the same.
                    if (oi_params.size() != oj_params.size()) continue;

                    /// Compare the parameters.
                    usz k = 0;
                    for (; k < oi_params.size(); k++)
                        if (not Type::Equal(oi_params[isz(k)], oj_params[isz(k)]))
                            break;

                    /// If all of them are equal, then we have a problem.
                    if (k != oi_params.size()) {
                        Error(
                            oi->location(),
                            "Overload set contains two overloads with the same parameter types"
                        );
                        Diag::Note(context, oj->location(), "Conflicting overload is here");
                        expr->set_sema_errored();
                    }
                }
            }
        } break;

        /// Unary prefix and postfix expressions.
        case Expr::Kind::Unary:
            AnalyseUnary(as<UnaryExpr>(expr));
            break;

        /// Binary expressions.
        case Expr::Kind::Binary:
            AnalyseBinary(as<BinaryExpr>(expr));
            break;

        /// Reference to a declared entity.
        case Expr::Kind::NameRef:
            AnalyseNameRef(as<NameRefExpr>(expr));
            break;

        /// Functions are analysed separately.
        case Expr::Kind::FuncDecl:
            LCC_ASSERT(expr->type()->is_function());
            break;

        /// The actual work here is analysing the type, so this is a no-op.
        case Expr::Kind::Type:
        case Expr::Kind::TypeDecl:
        case Expr::Kind::TypeAliasDecl:
        /// There isn’t really a way these could be malformed.
        case Expr::Kind::IntegerLiteral:
        case Expr::Kind::StringLiteral:
        /// These should only be created by sema and are thus no-ops.
        case Expr::Kind::Module:
        case Expr::Kind::EvaluatedConstant:
            break;
    }

    /// Do *not* use `expr` here, as it may have been replaced by something else.
    if (not(*expr_ptr)->sema_done_or_errored()) (*expr_ptr)->set_sema_done();
    return (*expr_ptr)->ok();
}

void lcc::glint::Sema::AnalyseBinary(BinaryExpr* b) {
    // Give up if there is an error in either operand.
    if (not Analyse(&b->lhs()) or not Analyse(&b->rhs())) {
        b->set_sema_errored();
        return;
    }

    switch (b->op()) {
        default: Diag::ICE("Invalid binary operator '{}'", ToString(b->op()));

        case TokenKind::And:
        case TokenKind::Or: {
            LValueToRValue(&b->lhs());
            LValueToRValue(&b->rhs());
            auto lhs = b->lhs()->type();
            auto rhs = b->rhs()->type();

            /// Both types must be integers or booleans.
            if (not lhs->is_integer(true) or not rhs->is_integer(true)) {
                Error(b->location(), "Cannot perform arithmetic on {} and {}", lhs, rhs);
                b->set_sema_errored();
                return;
            }

            /// Convert both operands to booleans.
            if (not Convert(&b->lhs(), Type::Bool)) {
                Error(
                    b->location(),
                    "Binary logical operator {} on {} and {}: cannot convert lhs, of type {}, to {}",
                    ToString(b->op()),
                    lhs,
                    rhs,
                    lhs,
                    Type::Bool
                );
                b->set_sema_errored();
                return;
            }
            if (not Convert(&b->rhs(), Type::Bool)) {
                Error(
                    b->location(),
                    "Binary logical operator {} on {} and {}: cannot convert rhs, of type {}, to {}",
                    ToString(b->op()),
                    lhs,
                    rhs,
                    lhs,
                    Type::Bool
                );
                b->set_sema_errored();
                return;
            }

            /// The result type is bool.
            b->type(Type::Bool);
        } break;

        /// Pointer or array subscript.
        case TokenKind::LBrack: {
            ImplicitDe_Reference(&b->lhs());
            auto* ty = b->lhs()->type();
            if (not is<PointerType, ArrayType>(ty)) {
                Error(
                    b->location(),
                    "LHS of subscript must be a pointer or array, but was {}",
                    b->lhs()->type()
                );
                b->set_sema_errored();
                return;
            }

            /// Result type is the pointer type or a pointer to the array element.
            b->type(is<PointerType>(ty) ? ty : Ptr(as<ArrayType>(ty)->element_type()));

            /// The RHS must be an integer.
            LValueToRValue(&b->rhs());
            if (not Convert(&b->rhs(), Type::Int)) {
                Error(b->rhs()->location(), "RHS of subscript must be an integer");
                return;
            }

            /// If it is an integer, try to evaluate it for bounds checking.
            if (auto* arr = cast<ArrayType>(ty); arr and arr->size() and arr->size()->ok() and arr->size()->kind() == Expr::Kind::EvaluatedConstant) {
                EvalResult res;
                if (b->rhs()->evaluate(context, res, false)) {
                    if (res.as_int().is_negative()
                        or res.as_int() >= as<ConstantExpr>(arr->size())->value().as_int().value())
                        Error(b->location(), "Array subscript out of bounds");

                    /// Since we already have the result, store it for later.
                    b->rhs() = new (mod) ConstantExpr(b->rhs(), res);
                }
            }
        } break;

        /// Pointer arithmetic is handled by the subscript operator,
        /// so these are all just regular arithmetic.
        case TokenKind::Star:
        case TokenKind::Slash:
        case TokenKind::Percent:
        case TokenKind::Plus:
        case TokenKind::Minus:
        case TokenKind::Shl:
        case TokenKind::Shr:
        case TokenKind::Ampersand:
        case TokenKind::Pipe:
        case TokenKind::Caret: {
            LValueToRValue(&b->lhs());
            LValueToRValue(&b->rhs());
            auto* lhs = b->lhs()->type();
            auto* rhs = b->rhs()->type();

            /// Both types must be integers.
            if (not lhs->is_integer() or not rhs->is_integer()) {
                Error(b->location(), "Cannot perform arithmetic on {} and {}", lhs, rhs);
                b->set_sema_errored();
                return;
            }

            /// Convert both operands to their common type.
            if (not ConvertToCommonType(&b->lhs(), &b->rhs())) {
                Error(b->location(), "Cannot perform arithmetic on {} and {}", lhs, rhs);
                b->set_sema_errored();
                return;
            }

            /// The result type is the common type.
            b->type(b->lhs()->type());
        } break;

        /// Comparisons are all handled the same.
        case TokenKind::Eq:
        case TokenKind::Ne:
        case TokenKind::Lt:
        case TokenKind::Gt:
        case TokenKind::Le:
        case TokenKind::Ge: {
            LValueToRValue(&b->lhs());
            LValueToRValue(&b->rhs());
            auto* lhs = b->lhs()->type();
            auto* rhs = b->rhs()->type();

            /// If both operands are integers, convert them to their common type.
            if (lhs->is_integer() and rhs->is_integer()) {
                if (not ConvertToCommonType(&b->lhs(), &b->rhs())) {
                    Error(b->location(), "Cannot compare {} and {}", lhs, rhs);
                    b->set_sema_errored();
                    return;
                }
            }

            /// Bool can only be compared with bool.
            else if (lhs->is_bool() and rhs->is_bool()) { /** No-op **/
            }

            /// If both operands are pointers, they must be the same type.
            else if (lhs->is_pointer() and rhs->is_pointer()) {
                if (not Type::Equal(lhs, rhs)) Error(
                    b->location(),
                    "Cannot compare unrelated pointer types {} and {}",
                    lhs,
                    rhs
                );
            }

            /// Other comparisons are not allowed.
            else { Error(b->location(), "Cannot compare {} and {}", lhs, rhs); }

            /// Comparisons return bool.
            b->type(Type::Bool);
        } break;

        /// Assignment.
        case TokenKind::ColonEq: {
            LValueToRValue(&b->rhs());
            ImplicitDe_Reference(&b->lhs());
            if (not b->lhs()->is_lvalue()) {
                Error(b->location(), "LHS of assignment must be an lvalue");
                b->set_sema_errored();
                return;
            }

            /// The type of the assignment is the same lvalue. Note that if
            /// the lhs is indeed an lvalue, we don’t ever mark this as errored
            /// because we know what its type is going to be, irrespective of
            /// whether the assignment is valid or not.
            b->type(b->lhs()->type());

            // Assignment always yields an lvalue.
            b->set_lvalue();

            // Disallow assigning to a sum type directly.
            auto* lhs_type = b->lhs()->type();
            if (auto* sum_type = cast<SumType>(b->lhs()->type())) {
                if (auto* m = cast<MemberAccessExpr>(b->lhs())) {
                    // Use member access to fetch type from sum type
                    lhs_type = sum_type->members().at(m->member()).type;
                } else {
                    // FIXME This isn't perfect, as ideally referencing a sum type anywhere
                    // except a member access should be an error, but we shouldn't have to add
                    // explicit checks absolutely everywhere that the thing we're dealing with
                    // isn't a sum type that isn't a member access.
                    Error(
                        b->lhs()->location(),
                        "Cannot assign to a sum type; access one of it's members using ``.''"
                    );
                    b->set_sema_errored();
                    return;
                }
            }

            /// The RHS must be assignable to the LHS.
            if (not Convert(&b->rhs(), lhs_type)) {
                Error(
                    b->rhs()->location(),
                    "Type of expression {} is not convertible to variable type {}",
                    b->rhs()->type(),
                    lhs_type
                );
                return;
            }

        } break;
    }
}

void lcc::glint::Sema::AnalyseCall(Expr** expr_ptr, CallExpr* expr) {
    /// If the callee is a name ref, check for builtins first.
    if (auto* name = cast<NameRefExpr>(expr->callee())) {
        static const StringMap<IntrinsicKind> builtin_names{
            {"__builtin_debugtrap", IntrinsicKind::BuiltinDebugtrap},
            {"__builtin_filename", IntrinsicKind::BuiltinFilename},
            {"__builtin_inline", IntrinsicKind::BuiltinInline},
            {"__builtin_line", IntrinsicKind::BuiltinLine},
            {"__builtin_memcpy", IntrinsicKind::BuiltinMemCopy},
            {"__builtin_memset", IntrinsicKind::BuiltinMemSet},
            {"__builtin_syscall", IntrinsicKind::BuiltinSyscall},
        };

        /// Check if this is the name of a builtin.
        auto n = name->name();
        if (auto kind = builtin_names.find(n); kind != builtin_names.end()) {
            /// We copy the arguments and leave the original expression unchanged
            /// since this node may be references in multiple places, all of which
            /// may need to be patched, and there is no good way of doing that
            /// without copying each use individually.
            auto* intrinsic = new (mod) IntrinsicCallExpr(
                kind->second,
                expr->args()
            );

            /// Make sure to actually analyse this intrinsic, as it will otherwise
            /// just be marked as done without actually being analysed.
            *expr_ptr = intrinsic;
            Analyse(expr_ptr);
            return;
        }
    }

    /// Analyse the callee and the arguments.
    for (auto*& arg : expr->args()) Analyse(&arg);

    /// If analysing the callee fails, we can’t do anything else.
    if (not Analyse(&expr->callee())) {
        expr->set_sema_errored();
        return;
    }

    /// If the callee is an overload set, perform overload resolution.
    if (is<OverloadSet>(expr->callee())) {
        /// If any of the arguments errored, we can’t resolve this.
        if (rgs::any_of(expr->args(), &Expr::sema_errored)) {
            expr->set_sema_errored();
            return;
        }

        /// TODO: Overload resolution.
        // See `docs/function_overload_resolution.org`
        Diag::ICE("Sorry, overload resolution is currently not implemented. Annoy the developer if you don't want it.");
    }

    // If the callee is a type expression, this is a type instantiation.
    // TODO: This NameRefExpr check is probably a sign of something more
    // sinister going on, but I can't exactly pinpoint it right now.
    if (is<TypeExpr>(expr->callee()) or (is<NameRefExpr>(expr->callee()) and is<TypeDecl>(as<NameRefExpr>(expr->callee())->target()))) {
        const usz s = expr->args().size();
        for (usz i = 0; i < s; ++i)
            LValueToRValue(expr->args().data() + i);

        if (expr->args().size() == 1) {
            *expr_ptr = new (mod) CastExpr(
                expr->args().at(0),
                expr->callee()->type(),
                CastKind::HardCast,
                expr->location()
            );
        } else {
            *expr_ptr = new (mod) CompoundLiteral(
                expr->args(),
                expr->location(),
                expr->callee()->type()
            );
        }
        return;
    }

    /// If the callee is a function pointer, dereference it.
    if (auto* ty = expr->callee()->type(); ty->is_pointer() and ty->elem()->is_function())
        InsertImplicitCast(&expr->callee(), ty->elem());

    // if the callee is an integer, multiply all the arguments.
    // `100 x;` -> 100 * x
    //   CallExpr(
    //       ConstantExpr 100
    //       NameRefExpr x
    //   )
    // becomes
    //   BinaryExpr(
    //       '*'
    //       ConstantExpr 100
    //       NameRefExpr x
    // )
    //
    // `100 x y` -> 100 * x * y
    //   CallExpr(
    //       ConstantExpr 100
    //       NameRefExpr x
    //       NameRefExpr y
    //   )
    // becomes
    //   BinaryExpr(
    //       '*'
    //       ConstantExpr 100
    //       BinaryExpr(
    //           NameRefExpr x
    //           NameRefExpr y
    //       )
    // )
    else if (auto* callee_ty = expr->callee()->type(); callee_ty->is_integer()) {
        // NOTE: Call of integer with zero arguments by deproceduring should not
        // be valid syntax, but this handles `100();` just in case.
        if (expr->args().empty() and not HasSideEffects(expr)) {
            Warning(
                expr->location(),
                "Expression result unused"
            );
            return;
        }

        auto* rhs = expr->args().back();
        // NOTE: Relies on unsigned underflow
        for (usz i = expr->args().size() - 2; i < expr->args().size(); --i) {
            auto* lhs = expr->args().at(i);
            rhs = new (mod) BinaryExpr(
                TokenKind::Star,
                lhs,
                rhs,
                {lhs->location(), rhs->location()}
            );
        }

        *expr_ptr = new (mod) BinaryExpr(
            TokenKind::Star,
            expr->callee(),
            rhs,
            expr->location()
        );

        Analyse(expr_ptr);

        return;
    }

    /// Otherwise, if the type is not already a function type, we can’t call this.
    else if (not callee_ty->is_function()) {
        Error(expr->callee()->location(), "Cannot call non-function(-pointer) type {}", callee_ty);
        expr->set_sema_errored();
        return;
    }

    /// The type of the call is the return type of the function.
    auto* func_type = cast<FuncType>(expr->callee()->type());
    expr->type(func_type->return_type());

    /// Check that there are as many arguments as parameters.
    if (expr->args().size() != func_type->params().size()) {
        Error(
            expr->location(),
            "Incorrect number of arguments for function. Expected {} instead of {}",
            func_type->params().size(),
            expr->args().size()
        );
    }

    /// Check that the arguments are convertible to the parameter types. This
    /// is one of the few places where we allow reference binding, so perform
    /// lvalue-to-rvalue conversion only if the parameter type is not a reference
    /// type. This is all handled transparently by Convert().
    for (usz i = 0, end = std::min(expr->args().size(), func_type->params().size()); i < end; i++) {
        LValueToRValue(expr->args().data() + i);
        if (not Convert(expr->args().data() + i, func_type->params()[i].type)) Error(
            expr->args()[i]->location(),
            "Type of argument {} is not convertible to parameter type {}",
            expr->args()[i]->type(),
            func_type->params()[i].type
        );
    }
}

void lcc::glint::Sema::AnalyseCast(CastExpr* c) {
    /// Implicit casts and lvalue-to-rvalue conversions are
    /// only ever created by sema, so we know they’re fine.
    if (c->is_implicit_cast() or c->is_lvalue_to_rvalue()
        or c->is_lvalue_to_ref() or c->is_ref_to_lvalue()) {
        c->set_lvalue(c->is_ref_to_lvalue());
        return;
    }

    /// If analysis of the operand failed, we don’t know its
    /// type and thus have no way of checking whether the cast
    /// makes sense.
    if (not Analyse(&c->operand(), c->type())) return;

    /// If the types are implicitly convertible, then the cast
    /// is fine. If this fails, it will still perform lvalue to
    /// rvalue conversion on the operand, which is exactly what
    /// we want.
    if (Convert(&c->operand(), c->type())) return;

    /// All conversions that rely on references have already been
    /// taken care of by Convert(), so we don’t care about references
    /// anymore at this point.
    ///
    /// Thus, the type we’re casting to must not be a reference type.
    auto* from = c->operand()->type();
    auto* to = c->type();
    if (to->is_reference()) {
        Error(c->location(), "Invalid cast of rvalue to reference type");
        return;
    }

    /// Explicitly casting from enums/integers to integers and
    /// enums/integers to booleans and booleans to integers is allowed.
    if ((from->is_integer(true) or from->is_enum()) and to->is_integer(true)) return;

    /// Casting from pointers to integers and pointers to booleans is allowed.
    if (from->is_pointer() and to->is_integer(true)) return;

    /// Helper to allow only hard casts.
    auto HardCast = [&] {
        if (not c->is_hard_cast()) Error(
            c->location(),
            "Cast from {} to {} is unsafe. If this is intended, use 'as!' instead",
            from,
            to
        );
    };

    /// Hard casts from integers to enums are allowed.
    if (from->is_integer(true) and to->is_enum()) return HardCast();

    /// Hard casts between pointers and from pointers to integers are
    /// allowed. Note that, if the pointers are compatible, the call to
    /// Convert() above will have already taken care of this case, so we
    /// don’t need to check for that here.
    if (to->is_pointer() and (from->is_integer() or from->is_pointer())) return HardCast();

    /// Hard casts between types that have the same size are allowed.
    if (from->size(context) == to->size(context) and c->is_hard_cast()) return;

    /// Any other casts are currently not allowed.
    Error(c->location(), "Invalid cast from {} to {}", from, to);
}

void lcc::glint::Sema::AnalyseIntrinsicCall(Expr** expr_ptr, IntrinsicCallExpr* expr) {
    switch (expr->intrinsic_kind()) {
        case IntrinsicKind::BuiltinDebugtrap: {
            if (not expr->args().empty())
                Error(expr->location(), "__builtin_debugtrap() takes no arguments");
            expr->type(Type::Void);
        } break;

        case IntrinsicKind::BuiltinFilename: {
            if (not expr->args().empty())
                Error(expr->location(), "__builtin_filename() takes no arguments");

            /// Get the name of the file containing this call.
            std::string filename = "<unknown>";
            if (expr->location().seekable(context))
                filename = context->files()[expr->location().file_id]->path().filename().string();

            /// Create a string literal containing the filename.
            auto* str = new (mod) StringLiteral(mod, filename, expr->location());
            expr->type(str->type());
            expr->set_sema_done();
            *expr_ptr = new (mod) ConstantExpr(expr, str);
        } break;

        case IntrinsicKind::BuiltinInline: {
            /// This takes one argument, and it must be a call expression.
            if (expr->args().size() != 1)
                Error(expr->location(), "__builtin_inline() takes exactly one argument");

            /// Analyse the call.
            auto*& call = expr->args().front();
            if (not Analyse(&call)) expr->set_sema_errored();
            if (not is<CallExpr>(call)) Error(
                call->location(),
                "Argument to __builtin_inline() must be a (non-builtin) function call"
            );

            /// Return type is the type of the callee.
            if (call->ok()) expr->type(call->type());
        } break;

        case IntrinsicKind::BuiltinLine: {
            if (not expr->args().empty()) Error(
                expr->location(),
                "__builtin_line() takes no arguments"
            );
            expr->type(Type::Int);
            expr->set_sema_done();

            /// If possible, seek to the location, if not we just insert 0.
            i64 line = 0;
            if (expr->location().seekable(context)) line = i64(expr->location().seek_line_column(context).line);
            *expr_ptr = new (mod) ConstantExpr(expr, line);
        } break;

        case IntrinsicKind::BuiltinMemCopy: {
            /// This takes two pointer and a size argument.
            if (expr->args().size() != 3)
                Error(expr->location(), "__builtin_memcpy() takes exactly three arguments");

            /// Analyse the arguments.
            for (auto*& arg : expr->args()) Analyse(&arg);
            ConvertOrError(&expr->args()[0], Type::VoidPtr);
            ConvertOrError(&expr->args()[1], Type::VoidPtr);
            ConvertOrError(&expr->args()[2], Type::Int);
            LValueToRValue(&expr->args()[0]);
            LValueToRValue(&expr->args()[1]);
            LValueToRValue(&expr->args()[2]);

            /// Unlike C’s memcpy()/memmove(), this returns nothing.
            expr->type(Type::Void);
        } break;

        case IntrinsicKind::BuiltinMemSet: {
            /// This takes two pointer and a size argument.
            if (expr->args().size() != 3)
                Error(expr->location(), "__builtin_memset() takes exactly three arguments");

            /// Analyse the arguments.
            for (auto*& arg : expr->args()) Analyse(&arg);
            ConvertOrError(&expr->args()[0], Type::VoidPtr);
            ConvertOrError(&expr->args()[1], Type::Byte);
            ConvertOrError(&expr->args()[2], Type::Int);
            LValueToRValue(&expr->args()[0]);
            LValueToRValue(&expr->args()[1]);
            LValueToRValue(&expr->args()[2]);

            /// Unlike C’s memset(), this returns nothing.
            expr->type(Type::Void);
        } break;

        case IntrinsicKind::BuiltinSyscall: {
            /// This has 1-7 integer-sized arguments and returns an integer.
            if (expr->args().empty() or expr->args().size() > 7)
                Error(expr->location(), "__builtin_syscall() takes between 1 and 7 arguments");

            /// Arguments must be integers or pointers.
            for (auto*& arg : expr->args()) {
                Analyse(&arg);
                InsertPointerToIntegerCast(&arg);
                ConvertOrError(&arg, Type::Int);
                LValueToRValue(&arg);
            }

            /// Syscalls all return integer.
            expr->type(Type::Int);
        } break;
    }
}

auto optimal_string_alignment_distance(
    std::string_view s,
    std::string_view t
) -> size_t {
    auto m = s.size();
    auto n = t.size();
    // Allocate 2d array
    // d :: [uint (m + 1) * (n + 1)]; <- equivalent in Glint
    size_t* d = (decltype(d))
        calloc(
            (m + 1) * (n + 1),
            sizeof(typeof(*d))
        );

    auto ref = [d, n](size_t i, size_t j) -> size_t& {
        return d[(i * n) + j];
    };

    for (size_t i = 0; i <= m; ++i) ref(i, 0) = i;
    for (size_t j = 0; j <= n; ++j) ref(0, j) = j;

    for (size_t j = 1; j <= n; ++j) {
        auto j_i = j - 1;
        for (size_t i = 1; i <= m; ++i) {
            auto i_i = i - 1;
            // SUBSTITUTION CHECK
            size_t cost{0};
            if (s.at(i_i) != t.at(j_i))
                cost = 1;

            ref(i, j) = std::min(
                {ref(i - 1, j) + 1,
                 ref(i, j - 1) + 1,
                 ref(i - 1, j - 1) + cost}
            );

            // TRANSPOSITION CHECK
            // abcd and acbd are very likely closer in distance, so we do that.
            if (i > 1 and j > 1
                and s.at(i_i) == t.at(j_i - 1)
                and s.at(i_i - 1) == t.at(j_i)) {
                ref(i, j) = std::min(
                    {ref(i, j),
                     ref(i - 2, j - 2) + 1}
                );
            }
        }
    }
    size_t out = ref(m, n);
    free(d);
    return out;
}

void lcc::glint::Sema::AnalyseNameRef(NameRefExpr* expr) {
    // Look up the thing in its scope, if there is no definition of the symbol
    // in its scope, search its parent scopes until we find one.
    auto* scope = expr->scope();
    std::vector<Decl*> syms = expr->scope()->find_recursive(expr->name());

    // If we’re at the global scope and there still is no symbol, then this
    // symbol is apparently not declared.
    if (syms.empty()) {
        /// Search imported modules here.
        for (const auto& ref : mod.imports()) {
            if (expr->name() == ref.name) {
                // Set expr->target() and expr->type() to something reasonable.
                auto* module_expr = new (mod) ModuleExpr(&mod, expr->location());
                expr->target(module_expr);
                expr->type(Type::Void);
                return;
            }
        }

        // Attempt to help out the Glint programmer by finding the closest match
        // of an existing declaration to what they typed.
        // NOTE: The more similar two strings are, the more their distances
        // approach zero.
        Decl* least_distance_decl = nullptr;
        size_t least_distance{size_t(-1)};
        for (auto* decl : scope->all_symbols_recursive()) {
            auto distance = optimal_string_alignment_distance(expr->name(), decl->name());
            LCC_ASSERT(
                distance,
                "If distance from '{}' to '{}' was zero, then symbol would have been found. Likely error in distance calculation.\n",
                expr->name(),
                decl->name()
            );
            if (distance < least_distance) {
                least_distance_decl = decl;
                least_distance = distance;
            }
        }
        // ¡AUTO-SPELLCHECK!
        // For identifiers that are unknown yet so, so close to an existing, valid
        // declaration, we just treat them like they were spelled right,
        // targetting that declaration.
        // This doesn't work well with strings below three characters, as the
        // maximum possible distance is often below or equal to our threshold
        // distance, so we don't apply it to short identifiers.
        // Also, it is confusing when it changes the length, so we require that
        // the replaced declaration has the same length as the given identifier.
        // Basically, this means that the only real possible swap is when two
        // single characters within a word are transposed (acbd instead of abcd).
        if (least_distance == 1
            and expr->name().size() > 2
            and expr->name().size() == least_distance_decl->name().size()) {
            Warning(
                expr->location(),
                "You typed '{}'; we are treating it as '{}' because it's so close",
                expr->name(),
                least_distance_decl->name()
            )
                .attach(
                    false,
                    Diag::Note(
                        context,
                        least_distance_decl->location(),
                        "Declared here"
                    )
                );
            expr->target(least_distance_decl);
            expr->type(least_distance_decl->type());
            if (least_distance_decl->is_lvalue()) expr->set_lvalue();
            return;
        }

        auto err = Error(expr->location(), "Unknown symbol '{}'", expr->name());

        // If there is a declaration of this variable in the top-level scope, tell
        // the user that they may have forgotten to make it static.
        auto top_level = mod.top_level_scope()->find(expr->name());
        if (not top_level.empty()) {
            err.attach(
                false,
                Diag::Note(
                    context,
                    top_level.at(0)->location(),
                    "A declaration exists at the top-level. Did you mean to make it 'static'?"
                )
            );
        }

        bool short_name = least_distance_decl and least_distance_decl->name().size() < 5;
        constexpr usz short_name_distance_max = 1;

        // If there is a short name, ensure it's distance is below or equal to the
        // maximum distance. Without this, things like `bar` get suggested to be
        // replaced with `fas`, and that just doesn't really make sense to me.
        if (least_distance_decl and (not short_name or least_distance <= short_name_distance_max)) {
            err.attach(
                false,
                Diag::Note(
                    context,
                    least_distance_decl->location(),
                    "Maybe you meant '{}', defined here?",
                    least_distance_decl->name()
                )
            );
        }

        expr->set_sema_errored();
        return;
    }

    // Either there is exactly one node that is not a function, or, there may
    // be one or more nodes with that name that are functions. In the case of
    // a non-function node, resolve to that node.
    if (not is<FuncDecl>(syms.at(0))) {
        // Make a copy of the pointer so we don't accidentally overwrite the
        // declaration's pointer in the following analysation.
        Expr* e = syms.at(0);
        Analyse(&e);
        // FIXME: What in the fuck is this for? The assert would mean we wouldn't
        // need the following line and the line following means we wouldn't need
        // the assert. A fucking idiot wrote this, clearly.
        LCC_ASSERT(syms.at(0) == e);
        syms.at(0) = as<Decl>(e);

        if (e->sema() == SemaNode::State::NoLongerViable) {
            Error(
                expr->location(),
                "Reference to a name, {}, that is no longer viable; probably a use-after-free thing",
                expr->name()
            );
        }

        // If sema is in progress for the declaration, and there is a name ref we
        // are trying to resolve that points to the declaration, it means the
        // declared object is being used in it's own initialiser, which doesn't
        // make sense.
        if (e->sema() == SemaNode::State::InProgress) {
            Error(
                expr->location(),
                "Cannot use '{}' in its own initialiser",
                expr->name()
            );
            expr->set_sema_errored();
            return;
        }

        expr->target(syms.at(0));
        expr->type(syms.at(0)->type());
        if (syms.at(0)->is_lvalue()) expr->set_lvalue();
        return;
    }

    // In the other case, collect all functions with that name and create an
    // overload set for them.
    std::vector<FuncDecl*> overloads{};
    overloads.reserve(syms.size());
    for (auto& sym : syms)
        overloads.emplace_back(as<FuncDecl>(sym));

    // If there is only one function, resolve it directly to that function.
    if (overloads.size() == 1) {
        expr->target(overloads[0]);
        expr->type(overloads[0]->type());
        return;
    }

    // Create a new overload set and analyse it. This will make sure there are
    // no redeclarations etc.
    Expr* overload_set = new (mod) OverloadSet(overloads, expr->location());
    Analyse(&overload_set);
    if (overload_set->sema_errored()) expr->set_sema_errored();

    // The type of an overload set is special because its actual type will depend
    // on the context. Roughly, the `OverloadSet` type is convertible to any
    // of the function types in the set, or pointers to them.
    expr->target(overload_set);
    expr->type(Type::OverloadSet);
}

void lcc::glint::Sema::AnalyseUnary(UnaryExpr* u) {
    /// Give up if there is an error in the operand.
    if (not Analyse(&u->operand())) {
        u->set_sema_errored();
        return;
    }

    /// Postfix operators.
    if (u->is_postfix()) {
        /// We currently don’t have postfix operators.
        LCC_UNREACHABLE();
    }

    /// Prefix operators.
    switch (u->op()) {
        default: Diag::ICE("Invalid prefix operator '{}'", ToString(u->op()));

        /// Get the address of an lvalue or function.
        case TokenKind::Ampersand: {
            if (not u->operand()->is_lvalue()) {
                Error(u->location(), "Cannot take address of rvalue");
                u->set_sema_errored();
                break;
            }

            u->type(Ptr(u->operand()->type()));
        } break;

        /// Convert a pointer to an lvalue.
        case TokenKind::At: {
            /// The pointer itself must be an rvalue.
            LValueToRValue(&u->operand());
            auto* ty = u->operand()->type();
            if (not is<PointerType>(ty)) {
                Error(u->location(), "Cannot dereference non-pointer type {}", ty);
                u->set_sema_errored();
                break;
            }

            u->type(as<PointerType>(ty)->element_type());
            u->set_lvalue();
        } break;

        // Negate an integer or free a dynamic array.
        case TokenKind::Minus: {
            if (u->operand()->type()->is_dynamic_array()) {
                u->type(Type::Void);
                LCC_ASSERT(
                    is<NameRefExpr>(u->operand()),
                    "Sorry, only handle NameRefExpr when freeing dynamic arrays"
                );
                auto* target = as<NameRefExpr>(u->operand())->target();

                // NOTE: If referenced again, will cause a used-but-no-longer-viable
                // diagnostic (catches use-after-free).
                target->set_sema_no_longer_viable();

                // NOTE: For forget-to-free diagnostics.
                std::erase(curr_func->dangling_dynarrays(), target);

                break;
            }

            LValueToRValue(&u->operand());
            auto* ty = u->operand()->type();

            if (not ty->is_integer()) {
                Error(
                    u->location(),
                    "Operand of unary prefix operator '-' must be an integer type, but was {}",
                    ty
                );
                u->set_sema_errored();
                break;
            }

            u->type(ty);
        } break;

        /// Bitwise-not an integer.
        case TokenKind::Tilde: {
            LValueToRValue(&u->operand());
            auto* ty = u->operand()->type();
            if (not ty->is_integer()) {
                Error(
                    u->location(),
                    "Operand of operator '~' must be an integer type, but was {}",
                    ty
                );
                u->set_sema_errored();
                break;
            }

            u->type(ty);
        } break;

        /// Negate a bool, integer, or pointer.
        case TokenKind::Exclam: {
            LValueToRValue(&u->operand());
            auto* ty = u->operand()->type();
            if (not is<PointerType>(ty) and not ty->is_integer(true)) {
                Error(
                    u->location(),
                    "Operand of operator '!' must be a bool, integer, or pointer type, but was {}",
                    ty
                );

                /// No need to mark this as errored because the
                /// result type is always bool.
                break;
            }

            /// The result of '!' is always a bool.
            u->type(Type::Bool);
        } break;

        // Check if a sum type currently stores a given member.
        case TokenKind::Has: {
            if (not is<SumType>(u->operand()->type())) {
                Error(
                    u->operand()->location(),
                    "Operand of 'has' must be a sum type"
                );
                u->set_sema_errored();
                break;
            }

            if (not is<MemberAccessExpr>(u->operand())) {
                Error(
                    u->operand()->location(),
                    "Operand of 'has' must be a member access to a sum type"
                );
                u->set_sema_errored();
                break;
            }

            // The result of 'has' is boolean.
            u->type(Type::Bool);
        } break;
    }
}

/// ===========================================================================
///  Analysing Types
/// ===========================================================================
auto lcc::glint::Sema::Analyse(Type** type_ptr) -> bool {
    auto* type = *type_ptr;

    // Don’t analyse the same type twice.
    if (type->sema() != SemaNode::State::NotAnalysed) return type->ok();
    type->set_sema_in_progress();

    switch (type->kind()) {
        // These are marked as done in the constructor.
        case Type::Kind::Builtin: LCC_UNREACHABLE();

        // These are no-ops.
        case Type::Kind::FFIType: break;

        // Named types need to be resolved to a type.
        case Type::Kind::Named: {
            auto* n = as<NamedType>(type);
            LCC_ASSERT(not n->name().empty(), "NamedType has empty name");
            LCC_ASSERT(n->scope(), "NamedType {} has NULL scope", n->name());

            // This code is similar to name resolution for expressions,
            // except that we don’t need to worry about overloads.
            Type* ty{};
            for (auto* scope = n->scope(); scope; scope = scope->parent()) {
                auto syms = scope->find(n->name());
                // If we don't find the symbol in this scope, continue searching the
                // parent scope.
                if (syms.empty()) continue;
                if (auto* s = cast<TypeDecl>(syms.at(0))) {
                    Expr* e = s;
                    Analyse(&e);
                    ty = s->type();
                    break;
                }

                if (auto* a = cast<TypeAliasDecl>(syms.at(0))) {
                    Expr* e = a;
                    Analyse(&e);
                    ty = a->type();
                    break;
                }

                auto err = Error(n->location(), "'{}' is not a type", n->name());
                err.attach(
                    false,
                    Diag::Note(
                        context,
                        syms.at(0)->location(),
                        "Because of declaration here",
                        n->name()
                    )
                );

                n->set_sema_errored();
                break;
            }

            if (not ty) {
                Error(n->location(), "'{}' does not name a type", n->name());
                n->set_sema_errored();
            } else {
                *type_ptr = ty;
            }
        } break;

        /// Pointers to any non-reference types are fine.
        case Type::Kind::Pointer: {
            auto* p = as<PointerType>(type);
            LCC_ASSERT(p->element_type(), "PointerType has NULL element type");
            Analyse(&p->element_type());

            auto* elem = p->element_type();
            if (is<ReferenceType>(elem)) {
                if (elem->ok()) Error(
                    p->location(),
                    "Cannot create pointer to reference type {}",
                    elem
                );
                p->set_sema_errored();
            }
        } break;

        /// References to references are collapsed to a single reference.
        case Type::Kind::Reference: {
            auto* r = as<ReferenceType>(type);
            LCC_ASSERT(r->element_type(), "ReferenceType has NULL element type");
            Analyse(&r->element_type());

            /// Collapse refs.
            while (is<ReferenceType>(r->element_type()))
                r->element_type(r->element_type()->elem());
        } break;

        /// Apply decltype decay to the element type and prohibit
        /// arrays of references. Also check the size.
        case Type::Kind::Array: {
            auto* a = as<ArrayType>(type);
            LCC_ASSERT(a->element_type(), "Array has NULL element type");
            Analyse(&a->element_type());
            a->element_type(DeclTypeDecay(a->element_type()));

            auto* elem = a->element_type();
            if (is<ReferenceType>(elem)) {
                if (elem->ok()) Error(
                    a->location(),
                    "Cannot create array of reference type {}",
                    elem
                );
                a->set_sema_errored();
            }

            usz size = 0;
            LCC_ASSERT(a->size(), "Array has NULL size expression");
            Analyse(&a->size());
            if (a->size()->ok()) {
                EvalResult res;
                if (a->size()->evaluate(context, res, false)) {
                    if (res.as_int().slt(1)) {
                        Error(a->location(), "Array size must be greater than 0");
                        a->set_sema_errored();
                    }

                    size = res.as_int().value();
                    a->size() = new (mod) ConstantExpr(a->size(), EvalResult(size));
                } else {
                    // Should be an ICE
                    Error(a->location(), "Array with variable size should have been made a dynamic array by the parser");
                    a->set_sema_errored();
                }
            }
        } break;

        // Apply decltype decay to the element type, prohibit arrays of
        // references, and, if there is an initial size expression, analyse that.
        // Also set cached struct type for IRGen by calling struct_type().
        case Type::Kind::DynamicArray: {
            auto* a = as<DynamicArrayType>(type);
            LCC_ASSERT(a->element_type(), "DynamicArray has NULL element type");
            Analyse(&a->element_type());
            a->element_type(DeclTypeDecay(a->element_type()));

            auto* elem = a->element_type();
            if (is<ReferenceType>(elem)) {
                if (elem->ok()) Error(
                    a->location(),
                    "Cannot create dynamic array of reference type {}",
                    elem
                );
                a->set_sema_errored();
            }

            // Cache struct type for IRGen.
            (void) a->struct_type(mod);

            if (a->initial_size()) Analyse(&a->initial_size());
        } break;

        // Apply decltype decay to the element type, prohibit arrays of
        // references, and, if there is an initial size expression, analyse that.
        // Also set cached struct type for IRGen by calling struct_type().
        case Type::Kind::Sum: {
            auto* s = as<SumType>(type);
            if (s->members().size() == 0) {
                Error(
                    s->location(),
                    "Sum type empty!\n"
                    "A sum type must have more than one member (otherwise, use a struct, or something)"
                );
                return false;
            }
            if (s->members().size() == 1) {
                Error(
                    s->location(),
                    "Sum type has a single member.\n"
                    "A sum type must have more than one member (otherwise, use a struct, or something)"
                );
                return false;
            }

            // Finalise members
            for (auto& member : s->members()) {
                // Analyse member type
                Analyse(&member.type);
                member.type = DeclTypeDecay(member.type);
                if (member.type->sema_errored()) {
                    type->set_sema_errored();
                    continue;
                }

                auto msize = member.type->size(context) / 8;
                auto malign = member.type->align(context) / 8;
                s->byte_size(std::max(s->byte_size(), msize));
                s->alignment(std::max(s->alignment(), malign));
            }

            // Cache struct type for IRGen.
            (void) s->struct_type(mod);
        } break;

        // Set cached struct type for IRGen by calling array_type().
        case Type::Kind::Union: {
            auto* u = as<UnionType>(type);
            usz byte_size = 0;
            usz alignment = 1;

            // Finalise members
            for (auto& member : u->members()) {
                // Analyse member type
                Analyse(&member.type);
                member.type = DeclTypeDecay(member.type);
                if (member.type->sema_errored()) {
                    type->set_sema_errored();
                    continue;
                }

                auto msize = member.type->size(context) / 8;
                auto malign = member.type->align(context) / 8;
                byte_size = std::max(byte_size, msize);
                alignment = std::max(alignment, malign);
            }

            u->byte_size(byte_size);
            u->alignment(alignment);

            // Cache struct type for IRGen
            (void) u->array_type(mod);
        } break;

        /// Analyse the parameters, the return type, and attributes.
        case Type::Kind::Function: {
            auto* ty = as<FuncType>(type);
            LCC_ASSERT(ty->return_type(), "Function type has NULL return type");
            Analyse(&ty->return_type());

            for (auto& param : ty->params()) {
                LCC_ASSERT(param.type, "Function type has parameter with NULL type");
                param.type = DeclTypeDecay(param.type);
                Analyse(&param.type);
            }

            /// If the function returns void, it must not be marked discardable.
            if (ty->return_type()->ok() and ty->return_type()->is_void()) {
                if (ty->has_attr(FuncAttr::Discardable))
                    Error(type->location(), "Function returning void cannot be 'discardable'");
            }

            /// Noreturn functions always have side effects.
            if (ty->has_attr(FuncAttr::NoReturn)) {
                if (ty->has_attr(FuncAttr::Const)) Error(
                    type->location(),
                    "'noreturn' function cannot be 'const'"
                );

                if (ty->has_attr(FuncAttr::Pure)) Error(
                    type->location(),
                    "'noreturn' function cannot be 'pure'"
                );
            }

            /// Check for conflicting inline/noinline attributes.
            if (ty->has_attr(FuncAttr::Inline) and ty->has_attr(FuncAttr::NoInline))
                Error(type->location(), "Function cannot be both 'inline' and 'noinline'");
        } break;

        /// Bit width may not be 0.
        case Type::Kind::Integer: {
            if (as<IntegerType>(type)->bit_width() == 0) {
                Error(type->location(), "Bit width of integer type cannot be 0");
                type->set_sema_errored();
            }
        } break;

        /// Calculate size, alignment, and member offsets.
        case Type::Kind::Struct: {
            /// TODO: Packed structs should probably be a separate type altogether and
            /// for those, we’ll have to perform all these calculations below in bits
            /// instead. Cereals!
            auto* s = as<StructType>(type);
            usz byte_size = 0;
            usz alignment = 1;

            /// Finalise all members.
            for (auto& member : s->members()) {
                /// Analyse the member type.
                Analyse(&member.type);
                member.type = DeclTypeDecay(member.type);
                if (member.type->sema_errored()) {
                    type->set_sema_errored();
                    continue;
                }

                /// Align the member to its alignment.
                auto msize = member.type->size(context) / 8;
                auto malign = member.type->align(context) / 8;
                member.byte_offset = utils::AlignTo(byte_size, malign);
                byte_size = member.byte_offset + msize;
                alignment = std::max(alignment, malign);
            }

            /// Align the struct to its alignment.
            /// Empty structs have a size of 0.
            s->alignment(alignment);
            s->byte_size(byte_size ? utils::AlignTo(byte_size, alignment) : 0);
        } break;

        /// Calculate enumerator values.
        case Type::Kind::Enum: {
            auto* e = as<EnumType>(type);
            LCC_ASSERT(
                e->underlying_type(),
                "Enum type has NULL underlying type"
            );

            if (not Analyse(&e->underlying_type())) {
                e->set_sema_errored();
                return false;
            }

            if (not e->underlying_type()->is_integer(true)) {
                Error(
                    e->location(),
                    "Disallowed underlying type of enum (sorry!).\n"
                    "Only integer or integer-like types are allowed, currently."
                );
                e->set_sema_errored();
                return false;
            }

            { // Error on duplicate enumerators.
                std::unordered_set<std::string> names;
                for (auto& val : e->enumerators()) {
                    if (not names.insert(val->name()).second) {
                        Error(val->location(), "Duplicate enumerator '{}'", val->name());
                        e->set_sema_errored();
                        return false;
                    }
                }
            }

            // Assign enumerator values to all enumerators.
            isz next_val = -1; //< For enums with integer underlying type.
            for (auto& val : e->enumerators()) {
                val->type(e);

                // For enums with integer underlying type, set the value if there is none.
                // Easy!
                if (not val->init()) {
                    if (e->underlying_type()->is_integer(true)) {
                        val->init() = new (mod) ConstantExpr(e, ++next_val, val->location());
                        val->set_sema_done();
                        continue;
                    }
                    Error(
                        val->location(),
                        "Unhandled underlying type given no init expression provided.\n"
                        "Compiler is too dumb to make a {}\n",
                        e->underlying_type()
                    );
                    val->set_sema_errored();
                    return false;
                }

                // User provided a value.
                // Harder.

                // Make sure the expression is well-formed, and has a type.
                if (not Analyse(&val->init())) {
                    Error(
                        val->init()->location(),
                        "Invalid init expression for {} within enumerator declaration",
                        val->name()
                    );
                    val->set_sema_errored();
                    return false;
                }

                // Convert the expression to the underlying type of the enum.
                if (not Convert(&val->init(), e->underlying_type())) {
                    // If the enum is associated with a declaration, print that name in the
                    // error message (name association is important for the developer!).
                    if (e->decl()) {
                        Error(
                            val->init()->location(),
                            "Init expression for {} within enumerator declaration {}",
                            val->name(),
                            e->decl()->name()
                        );
                        Note(
                            e->decl()->location(),
                            "Declared here"
                        );
                    } else {
                        Error(
                            val->init()->location(),
                            "Init expression for {} within enumerator definition",
                            val->name()
                        );
                        Note(
                            e->location(),
                            "Defined here"
                        );
                    }

                    val->set_sema_errored();
                    return false;
                }

                // Evaluate the expression at compile-time. If we can't, it's a fatal
                // error---enums are named constants.
                EvalResult res{0};
                if (not val->init()->evaluate(context, res, false)) {
                    Error(
                        val->init()->location(),
                        "Init expression for {} within enumerator is not a constant expression\n"
                        "This means the compiler is unable to calculate the value at compile-time.\n"
                        "Try using an integer constant like `69', if stuck.\n",
                        val->name()
                    );
                    val->set_sema_errored();
                    return false;
                }

                // Replace init expression with the constant expression that represents it
                // (with cached value).
                val->init() = new (mod) ConstantExpr(val->init(), res);
                val->set_sema_done();

                // For enums with integer underlying type, set the next value the compiler
                // will assign automatically if no init expression is provided.
                if (e->underlying_type()->is_integer(true))
                    next_val = decltype(next_val)(res.as_int().value()) + 1;

                // Declare the enumerator member in the enum's scope.
                auto d = e->scope()->declare(context, std::string(val->name()), val);
                LCC_ASSERT(d, "Failed to declare enumerator member");
            }
        } break;
    }

    /// Do *not* use `type` here, as it may have been replaced by something else.
    if (not(*type_ptr)->sema_done_or_errored()) (*type_ptr)->set_sema_done();
    return (*type_ptr)->ok();
}
