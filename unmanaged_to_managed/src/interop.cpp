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
            std::vector<encoded_value> children;
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

    // Hold one exported handle and keep its runtime callbacks alive.
    class handle_state
    {
    public:
        std::shared_ptr<runtime_core> core;
        std::uint64_t handle;
        // Keep the runtime callbacks alive for the handle's full lifetime.
        handle_state(std::shared_ptr<runtime_core> owner, std::uint64_t value) : core(std::move(owner)), handle(value)
        {
        }
        ~handle_state();
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

        // Encode one value with an explicit case for each ABI kind.
        encoded_value encode(const value &input, int depth = 0)
        {
            if (depth > 32)
                throw std::invalid_argument("Value nesting exceeds 32 levels");
            encoded_value encoded;
            auto &raw = encoded.raw;
            const auto &data = input.data();
            // The public variant order follows the ABI kind values.
            raw.kind = static_cast<std::int32_t>(data.index());
            switch (raw.kind)
            {
            case INTEROP_NULL:
                break;
            case INTEROP_BOOL:
                raw.scalar = std::get<bool>(data);
                break;
            case INTEROP_I32:
                raw.scalar = static_cast<std::uint64_t>(std::get<std::int32_t>(data));
                break;
            case INTEROP_I64:
                raw.scalar = static_cast<std::uint64_t>(std::get<std::int64_t>(data));
                break;
            case INTEROP_U32:
                raw.scalar = std::get<std::uint32_t>(data);
                break;
            case INTEROP_U64:
                raw.scalar = std::get<std::uint64_t>(data);
                break;
            case INTEROP_F32:
                raw.scalar = bit_copy<std::uint32_t>(std::get<float>(data));
                break;
            case INTEROP_F64:
                raw.scalar = bit_copy<std::uint64_t>(std::get<double>(data));
                break;
            case INTEROP_UTF8:
            {
                const auto &text = std::get<std::string>(data);
                raw.data = text.data();
                raw.length = static_cast<std::int64_t>(text.size());
                break;
            }
            case INTEROP_OBJECT:
            {
                const auto &object = std::get<interop::object>(data);
                if (!object.state_)
                {
                    raw.kind = INTEROP_NULL;
                    break;
                }
                if (object.state_->core->api.invoke != api.invoke)
                    throw std::invalid_argument("Cannot pass objects between different managed bridge instances");
                raw.scalar = object.state_->handle;
                break;
            }
            case INTEROP_ARRAY:
                return encode_array(std::get<value::array>(data), depth);
            case INTEROP_I8:
                raw.scalar = static_cast<std::uint64_t>(std::get<std::int8_t>(data));
                break;
            case INTEROP_U8:
                raw.scalar = std::get<std::uint8_t>(data);
                break;
            case INTEROP_I16:
                raw.scalar = static_cast<std::uint64_t>(std::get<std::int16_t>(data));
                break;
            case INTEROP_U16:
                raw.scalar = std::get<std::uint16_t>(data);
                break;
            case INTEROP_CHAR16:
                raw.scalar = std::get<char16_t>(data);
                break;
            default:
                throw std::invalid_argument("Unsupported native value kind");
            }
            return encoded;
        }

        // Keep nested ABI slots and their borrowed values alive during invoke.
        encoded_value encode_array(const value::array &items, int depth = 0)
        {
            if (depth > 32)
                throw std::invalid_argument("Value nesting exceeds 32 levels");
            encoded_value encoded;
            encoded.raw.kind = INTEROP_ARRAY;
            encoded.children.reserve(items.size());
            encoded.elements.reserve(items.size());
            for (const auto &item : items)
            {
                encoded.children.push_back(encode(item, depth + 1));
                encoded.elements.push_back(encoded.children.back().raw);
            }
            encoded.raw.data = encoded.elements.data();
            encoded.raw.length = static_cast<std::int64_t>(encoded.elements.size());
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
                auto state = std::make_shared<handle_state>(shared_from_this(), raw.scalar);
                raw.scalar = 0; // The result guard must not free an adopted lease.
                return value(object(std::move(state)));
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
        value perform(int operation, const std::shared_ptr<handle_state> &target = {}, std::string_view name = {},
                      const value::array &args = {}, const call_options &options = {}, std::string_view assembly = {})
        {
            if (target && target->core->api.invoke != api.invoke)
                throw std::invalid_argument("Object belongs to a different managed bridge");
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

            auto encoded_member = encode(member);
            auto encoded_assembly = encode(assembly_name);
            auto encoded_args = encode_array(args);
            auto encoded_signature = encode(signature);
            auto encoded_generics = encode(generic_names);
            interop_request request{};
            request.size = sizeof(request);
            request.version = INTEROP_ABI_VERSION;
            request.operation = operation;
            request.target = target ? target->handle : 0;
            request.name = encoded_member.raw;
            request.assembly = encoded_assembly.raw;
            request.arguments = encoded_args.raw;
            request.parameter_types = encoded_signature.raw;
            request.generic_types = encoded_generics.raw;
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
        static const std::shared_ptr<handle_state> &require(const object &target)
        {
            if (!target.state_)
                throw std::invalid_argument("Cannot use an empty managed object");
            return target.state_;
        }
    };

    // The managed GCHandle is freed only when the final C++ owner goes away.
    handle_state::~handle_state()
    {
        core->release(handle);
    }

    object::~object() = default;

    value object::call(std::string_view name, std::initializer_list<value> args, const call_options &options) const
    {
        return call(name, std::vector<value>(args), options);
    }
    value object::call(std::string_view name, const std::vector<value> &args, const call_options &options) const
    {
        const auto state = runtime_core::require(*this);
        return state->core->perform(INTEROP_CALL, state, name, args, options);
    }
    value object::get(std::string_view name) const
    {
        const auto state = runtime_core::require(*this);
        return state->core->perform(INTEROP_GET, state, name);
    }
    void object::set(std::string_view name, value v) const
    {
        const auto state = runtime_core::require(*this);
        state->core->perform(INTEROP_SET, state, name, {std::move(v)});
    }
    void object::dispose() const
    {
        const auto state = runtime_core::require(*this);
        state->core->perform(INTEROP_DISPOSE, state);
    }
    object type::create(std::initializer_list<value> args, const call_options &options) const
    {
        return create(std::vector<value>(args), options);
    }
    object type::create(const std::vector<value> &args, const call_options &options) const
    {
        const auto state = runtime_core::require(target_);
        return state->core->perform(INTEROP_CONSTRUCT, state, {}, args, options).as<object>();
    }
    value type::call(std::string_view name, std::initializer_list<value> args, const call_options &options) const
    {
        return call(name, std::vector<value>(args), options);
    }
    value type::call(std::string_view name, const std::vector<value> &args, const call_options &options) const
    {
        const auto state = runtime_core::require(target_);
        return state->core->perform(INTEROP_CALL_STATIC, state, name, args, options);
    }
    value type::get(std::string_view name) const
    {
        const auto state = runtime_core::require(target_);
        return state->core->perform(INTEROP_GET_STATIC, state, name);
    }
    void type::set(std::string_view name, value v) const
    {
        const auto state = runtime_core::require(target_);
        state->core->perform(INTEROP_SET_STATIC, state, name, {std::move(v)});
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
