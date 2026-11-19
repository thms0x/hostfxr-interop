#pragma once

#include <interop/abi.h>

#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace interop
{
    class runtime_core;
    class handle_state;
    class value;
    class type;

    // Reports a managed exception with its type, message, and stack trace.
    class managed_error : public std::runtime_error
    {
    public:
        // Build a C++ exception from diagnostics returned by the bridge.
        managed_error(std::string type, std::string message, std::string stack);
        // Return the fully qualified managed exception type.
        const std::string &managed_type() const noexcept { return type_; }
        // Return the full managed exception text captured at the ABI boundary.
        const std::string &managed_stack() const noexcept { return stack_; }

    private:
        std::string type_, stack_;
    };

    // Select an overload and close generic methods when the call needs them.
    // An absent signature infers an overload; an empty one selects zero parameters.
    struct call_options
    {
        std::optional<std::vector<std::string>> parameter_types;
        std::vector<std::string> generic_types;
    };

    // Shares one managed handle across C++ copies.
    // Release does not call IDisposable.Dispose; call dispose() when needed.
    class object
    {
    public:
        object() = default;
        object(const object &) = default;
        object(object &&) noexcept = default;
        object &operator=(const object &) = default;
        object &operator=(object &&) noexcept = default;
        // Release the managed GCHandle when the final C++ copy goes away.
        ~object();
        // Check whether this wrapper holds a managed object handle.
        explicit operator bool() const noexcept { return bool(state_); }
        // Drop this copy's ownership without disposing the managed object.
        void reset() noexcept { state_.reset(); }
        // Invoke an instance method with a short argument list.
        value call(std::string_view name, std::initializer_list<value> args = {},
                   const call_options &options = {}) const;
        // Invoke an instance method with a reusable argument vector.
        value call(std::string_view name, const std::vector<value> &args,
                   const call_options &options = {}) const;
        // Read a public instance property or field.
        value get(std::string_view name) const;
        // Write a public instance property or field.
        void set(std::string_view name, value v) const;
        // Calls IDisposable.Dispose if implemented; any exception is reported.
        void dispose() const;

    private:
        // Adopt the handle state created when decoding a result.
        explicit object(std::shared_ptr<handle_state> state) : state_(std::move(state)) {}
        std::shared_ptr<handle_state> state_;
        friend class runtime_core;
        friend class type;
    };

    // Stores one supported ABI value, including arrays and managed object handles.
    class value
    {
    public:
        using array = std::vector<value>;
        using storage = std::variant<std::monostate, bool, std::int32_t, std::int64_t,
                                     std::uint32_t, std::uint64_t, float, double, std::string, object, array,
                                     std::int8_t, std::uint8_t, std::int16_t, std::uint16_t, char16_t>;
        // Construct the matching variant alternative for a C++ input value.
        value() = default;
        value(std::nullptr_t) {}
        value(bool v) : data_(v) {}
        value(std::int32_t v) : data_(v) {}
        value(std::int64_t v) : data_(v) {}
        value(std::uint32_t v) : data_(v) {}
        value(std::uint64_t v) : data_(v) {}
        value(float v) : data_(v) {}
        value(double v) : data_(v) {}
        value(std::int8_t v) : data_(v) {}
        value(std::uint8_t v) : data_(v) {}
        value(std::int16_t v) : data_(v) {}
        value(std::uint16_t v) : data_(v) {}
        value(char16_t v) : data_(v) {}
        // Treat a null C string as a null managed value.
        value(const char *v) : data_(v ? storage(std::string(v)) : storage{}) {}
        value(std::string v) : data_(std::move(v)) {}
        value(std::string_view v) : data_(std::string(v)) {}
        // Treat an empty object wrapper as a null managed value.
        value(object v) : data_(v ? storage(std::move(v)) : storage{}) {}
        value(array v) : data_(std::move(v)) {}
        // Check whether the value represents a managed null.
        bool is_null() const noexcept { return std::holds_alternative<std::monostate>(data_); }
        // Read a value of the exact expected C++ type.
        template <class T>
        const T &as() const { return std::get<T>(data_); }
        // Expose the variant to the private ABI encoder.
        const storage &data() const noexcept { return data_; }

    private:
        storage data_;
    };

    // Holds a managed System.Type for construction and static member access.
    class type
    {
    public:
        // Invoke a constructor with a short argument list.
        object create(std::initializer_list<value> args = {}, const call_options &options = {}) const;
        // Invoke a constructor with a reusable argument vector.
        object create(const std::vector<value> &args, const call_options &options = {}) const;
        // Invoke a static method with a short argument list.
        value call(std::string_view name, std::initializer_list<value> args = {},
                   const call_options &options = {}) const;
        // Invoke a static method with a reusable argument vector.
        value call(std::string_view name, const std::vector<value> &args,
                   const call_options &options = {}) const;
        // Read a public static property or field.
        value get(std::string_view name) const;
        // Write a public static property or field.
        void set(std::string_view name, value v) const;

    private:
        // Wrap the managed System.Type handle returned by type lookup.
        explicit type(object target) : target_(std::move(target)) {}
        object target_;
        friend class runtime;
    };

    // Hosts the CLR and issues requests through its managed callback table.
    class runtime
    {
    public:
        // net48 uses the Windows CLR. Modern .NET uses the adjacent .runtimeconfig.json.
        // dotnet_root is only supported by the modern .NET host.
        explicit runtime(const std::filesystem::path &bridge_assembly,
                         const std::filesystem::path &dotnet_root = {});
        // Load a component assembly and resolve a public type within it.
        type load_type(const std::filesystem::path &assembly, std::string_view name) const;
        // Resolve a Framework or assembly-qualified name, e.g. System.GC.
        type resolve_type(std::string_view assembly_qualified_name) const;
        // Count all managed handles currently exported by this bridge.
        std::int64_t live_handles() const;
        // Advanced callers must follow the ownership rules in abi.h.
        interop_api native_api() const noexcept;

    private:
        std::shared_ptr<runtime_core> core_;
    };
} // namespace interop
