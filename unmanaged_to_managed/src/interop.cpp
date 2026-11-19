#include <interop/abi.h>
#include <interop/interop.hpp>
#include "internal.hpp"

#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>

namespace interop
{
    // Turn the managed error fields into one C++ exception.
    managed_error::managed_error(std::string type, std::string message, std::string stack)
        : std::runtime_error(type + ": " + message), type_(std::move(type)), stack_(std::move(stack))
    {
    }

    // Value conversion helpers stay private to this implementation file.
    namespace
    {
        // Preserve scalar bits in C++17 without aliasing or numeric conversion.
        template <class To, class From>
        To bit_copy(const From &source) noexcept
        {
            static_assert(sizeof(To) == sizeof(From));
            static_assert(std::is_trivially_copyable_v<To> && std::is_trivially_copyable_v<From>);
            To result;
            std::memcpy(&result, &source, sizeof(result));
            return result;
        }

        // Own nested ABI arrays while a native request is in progress.
        struct encoded_value
        {
            interop_value raw{};
            std::vector<std::unique_ptr<encoded_value>> children;
            std::vector<interop_value> elements;
        };

        // Copy a UTF-8 result or diagnostic from managed-owned memory.
        std::string text(const interop_value &raw)
        {
            if (raw.kind == INTEROP_NULL)
                return {};
            if (raw.kind != INTEROP_UTF8 || raw.length < 0 || (raw.length && !raw.data))
                throw std::runtime_error("Invalid UTF-8 result from managed bridge");
            return raw.length ? std::string(static_cast<const char *>(raw.data), static_cast<std::size_t>(raw.length))
                              : std::string{};
        }
    } // namespace

    // Own one exported handle; copies of interop::object share this lease.
    class object_lease
    {
    public:
        std::shared_ptr<runtime_core> core;
        std::uint64_t handle;
        // Keep the runtime callbacks alive for the handle's full lifetime.
        object_lease(std::shared_ptr<runtime_core> owner, std::uint64_t lease) : core(std::move(owner)), handle(lease)
        {
        }
        ~object_lease();
    };

    // Own the ABI callbacks and translate public C++ calls into requests.
    class runtime_core : public std::enable_shared_from_this<runtime_core>
    {
    public:
        interop_api api;
        // Retain the managed function table obtained during bootstrap.
        explicit runtime_core(interop_api functions) : api(functions)
        {
        }

        // Encode a C++ value and keep nested buffers alive until invoke returns.
        std::unique_ptr<encoded_value> encode(const value &input, int depth = 0)
        {
            if (depth > 32)
                throw std::invalid_argument("Value nesting exceeds 32 levels");
            auto encoded = std::make_unique<encoded_value>();
            auto &raw = encoded->raw;
            // The public variant order deliberately follows the ABI kind values.
            raw.kind = static_cast<std::int32_t>(input.data().index());
            std::visit(
                [&](const auto &item)
                {
                    using T = std::decay_t<decltype(item)>;
                    if constexpr (std::is_same_v<T, std::monostate>)
                    {
                    }
                    else if constexpr (std::is_integral_v<T>)
                        raw.scalar = static_cast<std::uint64_t>(item);
                    else if constexpr (std::is_same_v<T, float>)
                        raw.scalar = bit_copy<std::uint32_t>(item);
                    else if constexpr (std::is_same_v<T, double>)
                        raw.scalar = bit_copy<std::uint64_t>(item);
                    else if constexpr (std::is_same_v<T, std::string>)
                    {
                        raw.data = item.data();
                        raw.length = static_cast<std::int64_t>(item.size());
                    }
                    else if constexpr (std::is_same_v<T, object>)
                    {
                        if (!item.lease_)
                        {
                            raw.kind = INTEROP_NULL;
                            return;
                        }
                        if (item.lease_->core->api.invoke != api.invoke)
                            throw std::invalid_argument(
                                "Cannot pass objects between different managed bridge instances");
                        raw.scalar = item.lease_->handle;
                    }
                    else if constexpr (std::is_same_v<T, value::array>)
                    {
                        encoded->children.reserve(item.size());
                        encoded->elements.reserve(item.size());
                        for (const auto &element : item)
                        {
                            encoded->children.push_back(encode(element, depth + 1));
                            encoded->elements.push_back(encoded->children.back()->raw);
                        }
                        raw.data = encoded->elements.data();
                        raw.length = static_cast<std::int64_t>(encoded->elements.size());
                    }
                },
                input.data());
            return encoded;
        }

        // Copy a result into C++ storage and adopt any returned object handles.
        value decode(interop_value &raw, int depth = 0)
        {
            if (depth > 32)
                throw std::runtime_error("Managed result nesting exceeds 32 levels");
            switch (raw.kind)
            {
            case INTEROP_NULL:
                return {};
            case INTEROP_BOOL:
                return value(raw.scalar != 0);
            case INTEROP_I32:
                return value(bit_copy<std::int32_t>(static_cast<std::uint32_t>(raw.scalar)));
            case INTEROP_I64:
                return value(bit_copy<std::int64_t>(raw.scalar));
            case INTEROP_U32:
                return value(static_cast<std::uint32_t>(raw.scalar));
            case INTEROP_U64:
                return value(raw.scalar);
            case INTEROP_F32:
                return value(bit_copy<float>(static_cast<std::uint32_t>(raw.scalar)));
            case INTEROP_F64:
                return value(bit_copy<double>(raw.scalar));
            case INTEROP_I8:
                return value(bit_copy<std::int8_t>(static_cast<std::uint8_t>(raw.scalar)));
            case INTEROP_U8:
                return value(static_cast<std::uint8_t>(raw.scalar));
            case INTEROP_I16:
                return value(bit_copy<std::int16_t>(static_cast<std::uint16_t>(raw.scalar)));
            case INTEROP_U16:
                return value(static_cast<std::uint16_t>(raw.scalar));
            case INTEROP_CHAR16:
                return value(static_cast<char16_t>(raw.scalar));
            case INTEROP_UTF8:
                return value(text(raw));
            case INTEROP_OBJECT:
            {
                if (!raw.scalar)
                    throw std::runtime_error("Managed bridge returned a zero object lease");
                auto lease = std::make_shared<object_lease>(shared_from_this(), raw.scalar);
                raw.scalar = 0; // The result guard must not free an adopted lease.
                return value(object(std::move(lease)));
            }
            case INTEROP_ARRAY:
            {
                if (raw.length < 0 || raw.length > std::numeric_limits<std::int32_t>::max() ||
                    (raw.length && !raw.data))
                    throw std::runtime_error("Invalid managed array result");
                value::array elements;
                elements.reserve(static_cast<std::size_t>(raw.length));
                auto *items = static_cast<interop_value *>(const_cast<void *>(raw.data));
                for (std::int64_t i = 0; i < raw.length; ++i)
                    elements.push_back(decode(items[i], depth + 1));
                return value(std::move(elements));
            }
            default:
                throw std::runtime_error("Unknown managed value kind");
            }
        }

        // Send one request, translate errors, and free its response on every path.
        value perform(int operation, const std::shared_ptr<object_lease> &target = {}, std::string_view name = {},
                      value::array args = {}, const call_options &options = {}, std::string_view assembly = {})
        {
            if (target && target->core->api.invoke != api.invoke)
                throw std::invalid_argument("Object belongs to a different managed bridge");
            value arguments(std::move(args));
            value member = name.empty() ? value{} : value(name);
            value assembly_name = assembly.empty() ? value{} : value(assembly);
            value signature;
            if (options.parameter_types)
            {
                value::array names;
                for (const auto &item : *options.parameter_types)
                    names.emplace_back(item);
                signature = value(std::move(names));
            }
            value::array generics;
            for (const auto &item : options.generic_types)
                generics.emplace_back(item);
            value generic_names(std::move(generics));
            auto encoded_args = encode(arguments);
            auto encoded_member = encode(member);
            auto encoded_assembly = encode(assembly_name);
            auto encoded_signature = encode(signature);
            auto encoded_generics = encode(generic_names);
            interop_request request{sizeof(interop_request),
                                    INTEROP_ABI_VERSION,
                                    operation,
                                    0,
                                    target ? target->handle : 0,
                                    encoded_member->raw,
                                    encoded_assembly->raw,
                                    encoded_args->raw,
                                    encoded_signature->raw,
                                    encoded_generics->raw};
            interop_result result{};
            // Free result buffers even if decoding or error translation throws.
            struct result_guard
            {
                interop_result *result;
                interop_free_result_fn free;
                ~result_guard()
                {
                    free(result);
                }
            } guard{&result, api.free_result};
            int status = api.invoke(&request, &result);
            if (status != 0)
            {
                if (result.error_type.kind == INTEROP_UTF8)
                    throw managed_error(text(result.error_type), text(result.error_message), text(result.error_stack));
                throw std::runtime_error(detail::status_message("Managed call without diagnostics", status));
            }
            return decode(result.value);
        }

        // Release a managed handle from a noexcept C++ destructor.
        void release(std::uint64_t token) noexcept
        {
            interop_request request{};
            request.size = sizeof(request);
            request.version = INTEROP_ABI_VERSION;
            request.operation = INTEROP_RELEASE;
            request.target = token;
            interop_result result{};
            api.invoke(&request, &result);
            api.free_result(&result);
        }

        // Reject an empty wrapper before issuing an instance request.
        static const std::shared_ptr<object_lease> &require(const object &target)
        {
            if (!target.lease_)
                throw std::invalid_argument("Cannot use an empty managed object");
            return target.lease_;
        }
    };

    // The managed GCHandle is freed only when the final C++ owner goes away.
    object_lease::~object_lease()
    {
        core->release(handle);
    }

    object::~object()
    {
        // Copies share lease_; its final owner runs object_lease::~object_lease.
        lease_.reset();
    }

    value object::call(std::string_view name, std::initializer_list<value> args, const call_options &options) const
    {
        return call(name, std::vector<value>(args), options);
    }
    value object::call(std::string_view name, const std::vector<value> &args, const call_options &options) const
    {
        const auto lease = runtime_core::require(*this);
        return lease->core->perform(INTEROP_CALL, lease, name, {args.begin(), args.end()}, options);
    }
    value object::get(std::string_view name) const
    {
        const auto lease = runtime_core::require(*this);
        return lease->core->perform(INTEROP_GET, lease, name);
    }
    void object::set(std::string_view name, value v) const
    {
        const auto lease = runtime_core::require(*this);
        lease->core->perform(INTEROP_SET, lease, name, {std::move(v)});
    }
    void object::dispose() const
    {
        const auto lease = runtime_core::require(*this);
        lease->core->perform(INTEROP_DISPOSE, lease);
    }
    object type::create(std::initializer_list<value> args, const call_options &options) const
    {
        return create(std::vector<value>(args), options);
    }
    object type::create(const std::vector<value> &args, const call_options &options) const
    {
        const auto lease = runtime_core::require(target_);
        return lease->core->perform(INTEROP_CONSTRUCT, lease, {}, {args.begin(), args.end()}, options).as<object>();
    }
    value type::call(std::string_view name, std::initializer_list<value> args, const call_options &options) const
    {
        return call(name, std::vector<value>(args), options);
    }
    value type::call(std::string_view name, const std::vector<value> &args, const call_options &options) const
    {
        const auto lease = runtime_core::require(target_);
        return lease->core->perform(INTEROP_CALL_STATIC, lease, name, {args.begin(), args.end()}, options);
    }
    value type::get(std::string_view name) const
    {
        const auto lease = runtime_core::require(target_);
        return lease->core->perform(INTEROP_GET_STATIC, lease, name);
    }
    void type::set(std::string_view name, value v) const
    {
        const auto lease = runtime_core::require(target_);
        lease->core->perform(INTEROP_SET_STATIC, lease, name, {std::move(v)});
    }
    runtime::runtime(const std::filesystem::path &bridge, const std::filesystem::path &dotnet_root)
        : core_(std::make_shared<runtime_core>(detail::bootstrap(bridge, dotnet_root)))
    {
    }
    type runtime::load_type(const std::filesystem::path &assembly, std::string_view name) const
    {
        const auto path = detail::utf8_path(std::filesystem::canonical(assembly));
        return type(core_->perform(INTEROP_LOAD_TYPE, {}, name, {}, {}, path).as<object>());
    }
    type runtime::resolve_type(std::string_view name) const
    {
        return type(core_->perform(INTEROP_LOAD_TYPE, {}, name).as<object>());
    }
    std::int64_t runtime::live_handles() const
    {
        return core_->perform(INTEROP_LIVE_HANDLES).as<std::int64_t>();
    }
    interop_api runtime::native_api() const noexcept
    {
        return core_->api;
    }
} // namespace interop
