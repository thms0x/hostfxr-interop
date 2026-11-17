#pragma once
#include <native_interop/abi.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace native_interop
{
    class object;
    class value
    {
    public:
        using object_ptr = std::shared_ptr<object>;
        using array = std::vector<value>;
        // Variant order matches ni_kind; fixed numeric widths must match the caller.
        using storage = std::variant<std::monostate, bool, std::int32_t, std::int64_t,
                                     std::uint32_t, std::uint64_t, float, double, std::string, object_ptr, array,
                                     std::int8_t, std::uint8_t, std::int16_t, std::uint16_t, char16_t>;
        value() = default;
        value(std::nullptr_t) {}
        value(const char *text) : data_(text ? storage(std::string(text)) : storage{}) {}
        value(std::string_view text) : data_(std::string(text)) {}
        value(object_ptr ptr) : data_(ptr ? storage(std::move(ptr)) : storage{}) {}
        template <class T, std::enable_if_t<std::is_constructible_v<storage, T>, int> = 0>
        value(T input) : data_(std::move(input)) {}
        template <class T>
        const T &as() const { return std::get<T>(data_); }
        bool is_null() const { return std::holds_alternative<std::monostate>(data_); }
        const storage &data() const { return data_; }

    private:
        storage data_;
    };

    // Adapt a C++ class by implementing its explicitly exposed methods/properties.
    // Destructors must not throw. Synchronize mutable state if callers use threads.
    class object
    {
    public:
        virtual ~object() = default;
        virtual value call(std::string_view name, const value::array &)
        {
            throw std::invalid_argument("Unknown native method: " + std::string(name));
        }
        virtual value get(std::string_view name)
        {
            throw std::invalid_argument("Unknown native property: " + std::string(name));
        }
        virtual void set(std::string_view name, const value &)
        {
            throw std::invalid_argument("Unknown or read-only native property: " + std::string(name));
        }
    };

    using factory = std::function<std::shared_ptr<object>(const value::array &)>;
    // Registration is local to the plugin. Duplicate names are rejected. Optional
    // statics uses the same adapter interface for static calls and properties.
    void register_type(std::string name, factory construct, std::shared_ptr<object> statics = {});
    // Call from the plugin's exported ni_get_api after registering its types.
    int get_api(std::uint32_t version, std::uint32_t size, ni_api *output) noexcept;
} // namespace native_interop
