#ifndef LCC_CONTEXT_HH
#define LCC_CONTEXT_HH

#include <lcc/file.hh>
#include <lcc/forward.hh>
#include <lcc/location.hh>
#include <lcc/utils.hh>

#if defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(__NT__)
#    define LCC_PLATFORM_WINDOWS 1
#endif

namespace lcc {
class Target;
class Format;

class Context {
    /// The files owned by the context.
    std::vector<std::unique_ptr<File>> owned_files;

    /// Error flag. This is set-only.
    mutable bool error_flag = false;

    /// Called once the first time a context is created.
    static void InitialiseLCCData();

    /// Miscellaneous flags.
    bool _colour_diagnostics = false;
    bool _should_print_mir = false;
    bool _stopat_mir = false;

    const Target* _target{};
    const Format* _format{};

    std::vector<std::string> _include_directories{};

public:
    /// IR type caches.
    // TODO: Could these be smart pointers? If not, why?
    std::unordered_map<usz, Type*> integer_types;
    std::vector<Type*> array_types;
    std::vector<Type*> function_types;
    std::vector<Type*> struct_types;

    /// Create a new context.
    explicit Context(
        const Target* target,
        const Format* format,
        bool colour_diagnostics,
        bool should_print_mir,
        bool stopat_mir
    );

    /// Do not allow copying or moving the context.
    Context(const Context&) = delete;
    Context(Context&&) = delete;
    auto operator=(const Context&) -> Context& = delete;
    auto operator=(Context&&) -> Context& = delete;

    /// Delete all files and IR types.
    ~Context();

    /// Create a new file from a name and contents.
    template <typename Buffer>
    [[nodiscard]]
    auto create_file(fs::path name, Buffer&& contents) -> File& {
        return make_file(
            std::move(name),
            std::vector<char>{std::forward<Buffer>(contents)}
        );
    }

    /// Get a list of all files owned by the context.
    [[nodiscard]]
    auto files() const -> const decltype(owned_files)& {
        return owned_files;
    }

    /// Get a file from disk.
    ///
    /// This loads a file from disk or returns a reference to it if
    /// is has already been loaded.
    ///
    /// \param path The path to the file.
    /// \return A reference to the file.
    [[nodiscard]]
    auto get_or_load_file(fs::path path) -> File&;

    /// Check if the error flag is set.
    [[nodiscard]]
    auto has_error() const -> bool { return error_flag; }

    /// Set the error flag.
    ///
    /// \return The previous value of the error flag.
    auto set_error() const -> bool {
        auto old = error_flag;
        error_flag = true;
        return old;
    }

    /// Get the target.
    [[nodiscard]]
    auto target() const { return _target; }

    /// Get the target.
    [[nodiscard]]
    auto format() const { return _format; }

    /// Whether to use colours in diagnostics.
    [[nodiscard]]
    auto use_colour_diagnostics() const -> bool {
        return _colour_diagnostics;
    }

    [[nodiscard]]
    auto should_print_mir() const -> bool {
        return _should_print_mir;
    }

    [[nodiscard]]
    auto stopat_mir() const -> bool { return _stopat_mir; }

    auto include_directories() const -> const decltype(_include_directories)& {
        return _include_directories;
    }

    void add_include_directory(std::string dir) {
        _include_directories.push_back(std::move(dir));
    }

private:
    /// Register a file in the context.
    auto make_file(fs::path name, std::vector<char>&& contents) -> File&;
};
} // namespace lcc

#endif // LCC_CONTEXT_HH
