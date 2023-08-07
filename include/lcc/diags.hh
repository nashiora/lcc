#ifndef LCC_DIAGS_HH
#define LCC_DIAGS_HH

#include <lcc/forward.hh>
#include <lcc/location.hh>
#include <lcc/utils.hh>

namespace lcc {
/// A diagnostic. The diagnostic is issued when the destructor is called.
struct Diag {
    /// Diagnostic severity.
    enum Kind {
        None,    ///< Not an error. Do not emit this diagnostic.
        Note,    ///< Informational note.
        Warning, ///< Warning, but no hard error.
        Error,   ///< Hard error. Program is ill-formed.
        FError,  ///< Fatal (system) error. NOT a compiler bug.
        ICError, ///< Compiler bug.
    };

private:
    const Context* ctx;
    Kind kind;
    Location where;
    std::string msg;

    /// Handle fatal error codes.
    void HandleFatalErrors();

    /// Print a diagnostic with no (valid) location info.
    void PrintDiagWithoutLocation();

    /// Seek to a source location.
    [[nodiscard]] auto Seek() const -> LocInfo;

    /// Seek to a source location, but only return the line and column.
    [[nodiscard]] auto SeekLineColumn() const -> LocInfoShort;

    /// Check if the source location is seekable.
    [[nodiscard]] bool Seekable() const;

public:
    static constexpr u8 ICE_EXIT_CODE = 17;
    static constexpr u8 FATAL_EXIT_CODE = 18;

    Diag(Diag&& other)
        : ctx(other.ctx),
          kind(other.kind),
          where(other.where),
          msg(std::move(other.msg)) {
        other.kind = None;
    }

    Diag& operator=(Diag&& other) {
        if (this == &other) return *this;
        ctx = other.ctx;
        kind = other.kind;
        where = other.where;
        msg = std::move(other.msg);
        other.kind = None;
        return *this;
    }

    /// The destructor prints the diagnostic, if it hasn’t been moved from.
    ~Diag();

    /// Issue a diagnostic.
    Diag(const Context* ctx, Kind kind, Location where, std::string msg)
        : ctx(ctx), kind(kind), where(where), msg(std::move(msg)) {}

    /// Issue a diagnostic with no location.
    Diag(Kind _kind, std::string&& msg)
        : ctx(nullptr), kind(_kind), where(), msg(std::move(msg)) {}

    /// Issue a diagnostic with a format string and arguments.
    template <typename... Args>
    Diag(
        const Context* ctx,
        Kind kind,
        Location where,
        fmt::format_string<Args...> fmt,
        Args&&... args
    ) : Diag{ctx, kind, where, fmt::format(fmt, std::forward<Args>(args)...)} {}

    /// Issue a diagnostic with a format string and arguments, but no location.
    template <typename... Args>
    Diag(Kind kind, fmt::format_string<Args...> fmt, Args&&... args)
        : Diag{kind, fmt::format(fmt, std::forward<Args>(args)...)} {}

    /// Raise an internal compiler error and exit.
    template <typename... Args>
    [[noreturn]] static void ICE(fmt::format_string<Args...> fmt, Args&&... args) {
        Diag{ICError, fmt::format(fmt, std::forward<Args>(args)...)};
        std::terminate(); /// Should never be reached.
    }

    /// Raise an internal compiler error at a location and exit.
    template <typename... Args>
    [[noreturn]] static void ICE(
        const Context* ctx,
        Location where,
        fmt::format_string<Args...> fmt,
        Args&&... args
    ) {
        Diag{ctx, ICError, where, fmt::format(fmt, std::forward<Args>(args)...)};
        std::terminate(); /// Should never be reached.
    }

    /// Raise a fatal error and exit.
    ///
    /// This is NOT an ICE; instead it is an error that is probably caused by
    /// the underlying system, such as attempting to output to a directory that
    /// isn’t accessible to the user.
    template <typename... Args>
    [[noreturn]] static void Fatal(fmt::format_string<Args...> fmt, Args&&... args) {
        Diag{FError, fmt::format(fmt, std::forward<Args>(args)...)};
        std::terminate(); /// Should never be reached.
    }
};

} // namespace lcc

#endif // LCC_DIAGS_HH