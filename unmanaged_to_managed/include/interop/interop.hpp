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
    class object_lease;
    class value;
    class type;

    class managed_error : public std::runtime_error
    {
    public:
        managed_error(std::string type, std::string message, std::string stack);
        const std::string &managed_type() const noexcept { return type_; }
        const std::string &managed_stack() const noexcept { return stack_; }

    private:
        std::string type_, stack_;
    };

    // An absent signature infers an overload. An empty signature selects zero parameters.
    struct call_options
    {
        std::optional<std::vector<std::string>> parameter_types;
        std::vector<std::string> generic_types;
    };

    // Copies share one lease; reset() releases this copy. Every lease roots its target.
    // Release does not call IDisposable.Dispose; call dispose() explicitly when needed.
    class object
    {
    public:
        object() = default;
        object(const object &) = default;
        object(object &&) noexcept = default;
        object &operator=(const object &) = default;
        object &operator=(object &&) noexcept = default;
        // The final C++ owner releases the managed GCHandle through the bridge.
        ~object();
        explicit operator bool() const noexcept { return bool(lease_); }
        void reset() noexcept { lease_.reset(); }
        value call(std::string_view name, std::initializer_list<value> args = {},
                   const call_options &options = {}) const;
        value call(std::string_view name, const std::vector<value> &args,
                   const call_options &options = {}) const;
        value get(std::string_view name) const;
        void set(std::string_view name, value v) const;
        // Calls IDisposable.Dispose if implemented; any exception is reported.
        void dispose() const;

    private:
        explicit object(std::shared_ptr<object_lease> lease) : lease_(std::move(lease)) {}
        std::shared_ptr<object_lease> lease_;
        friend class runtime_core;
        friend class type;
    };

    class value
    {
    public:
        using array = std::vector<value>;
        using storage = std::variant<std::monostate, bool, std::int32_t, std::int64_t,
                                     std::uint32_t, std::uint64_t, float, double, std::string, object, array,
                                     std::int8_t, std::uint8_t, std::int16_t, std::uint16_t, char16_t>;
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
        value(const char *v) : data_(v ? storage(std::string(v)) : storage{}) {}
        value(std::string v) : data_(std::move(v)) {}
        value(std::string_view v) : data_(std::string(v)) {}
        value(object v) : data_(v ? storage(std::move(v)) : storage{}) {}
        value(array v) : data_(std::move(v)) {}
        bool is_null() const noexcept { return std::holds_alternative<std::monostate>(data_); }
        template <class T>
        const T &as() const { return std::get<T>(data_); }
        const storage &data() const noexcept { return data_; }

    private:
        storage data_;
    };

    class type
    {
    public:
        object create(std::initializer_list<value> args = {}, const call_options &options = {}) const;
        object create(const std::vector<value> &args, const call_options &options = {}) const;
        value call(std::string_view name, std::initializer_list<value> args = {},
                   const call_options &options = {}) const;
        value call(std::string_view name, const std::vector<value> &args,
                   const call_options &options = {}) const;
        value get(std::string_view name) const;
        void set(std::string_view name, value v) const;

    private:
        explicit type(object target) : target_(std::move(target)) {}
        object target_;
        friend class runtime;
    };

    class runtime
    {
    public:
        // net48 uses the Windows CLR. Modern .NET uses the adjacent .runtimeconfig.json.
        // dotnet_root is only supported by the modern .NET host.
        explicit runtime(const std::filesystem::path &bridge_assembly,
                         const std::filesystem::path &dotnet_root = {});
        type load_type(const std::filesystem::path &assembly, std::string_view name) const;
        // Framework or assembly-qualified type name, e.g. System.GC.
        type resolve_type(std::string_view assembly_qualified_name) const;
        std::int64_t live_handles() const;
        // Advanced callers must follow the ownership rules in abi.h.
        interop_api native_api() const noexcept;

    private:
        std::shared_ptr<runtime_core> core_;
    };
} // namespace interop
