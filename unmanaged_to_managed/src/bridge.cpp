#include "detail/bridge_state.hpp"

#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>

namespace interop
{
    namespace detail
    {
        namespace
        {
            template <class To, class From> To bit_copy(const From &source) noexcept
            {
                static_assert(sizeof(To) == sizeof(From));
                static_assert(std::is_trivially_copyable_v<To> && std::is_trivially_copyable_v<From>);
                To result;
                std::memcpy(&result, &source, sizeof(result));
                return result;
            }

            std::string text(const interop_value &raw)
            {
                if (raw.kind == INTEROP_NULL)
                    return {};
                if (raw.kind != INTEROP_UTF8 || raw.length < 0 || (raw.length && !raw.data))
                    throw std::runtime_error("Invalid UTF-8 result from managed bridge");
                return raw.length
                           ? std::string(static_cast<const char *>(raw.data), static_cast<std::size_t>(raw.length))
                           : std::string{};
            }
        } // namespace

        std::unique_ptr<encoded_value> runtime_state::encode(const value &input, int depth)
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
                        if (!item.state_)
                        {
                            raw.kind = INTEROP_NULL;
                            return;
                        }
                        if (item.state_->runtime->api.invoke != api.invoke)
                            throw std::invalid_argument("Cannot pass objects between different "
                                                        "managed bridge instances");
                        raw.scalar = item.state_->token;
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

        value runtime_state::decode(interop_value &raw, int depth)
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
                auto state = std::make_shared<object_state>(shared_from_this(), raw.scalar);
                raw.scalar = 0; // Ownership transferred; free_result must not release it.
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

        value runtime_state::perform(int operation, const std::shared_ptr<object_state> &target, std::string_view name,
                                     value::array args, const call_options &options, std::string_view assembly)
        {
            if (target && target->runtime->api.invoke != api.invoke)
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
            auto encoded_args = runtime_state::encode(arguments);
            auto encoded_member = encode(member);
            auto encoded_assembly = encode(assembly_name);
            auto encoded_signature = encode(signature);
            auto encoded_generics = encode(generic_names);
            interop_request request{sizeof(interop_request),
                                    INTEROP_ABI_VERSION,
                                    operation,
                                    0,
                                    target ? target->token : 0,
                                    encoded_member->raw,
                                    encoded_assembly->raw,
                                    encoded_args->raw,
                                    encoded_signature->raw,
                                    encoded_generics->raw};
            interop_result result{};
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
                throw std::runtime_error(status_message("Managed call without diagnostics", status));
            }
            return runtime_state::decode(result.value);
        }

        void runtime_state::release(std::uint64_t token) noexcept
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

        const std::shared_ptr<object_state> &runtime_state::require(const object &target)
        {
            if (!target.state_)
                throw std::invalid_argument("Cannot use an empty managed object");
            return target.state_;
        }
    } // namespace detail

} // namespace interop
