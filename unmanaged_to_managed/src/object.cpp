#include "detail/bridge_state.hpp"

namespace interop
{
    namespace detail
    {
        // The final C++ owner tells the managed bridge to free its GCHandle.
        object_state::~object_state()
        {
            runtime->release(token);
        }
    } // namespace detail

    managed_error::managed_error(std::string type, std::string message, std::string stack)
        : std::runtime_error(type + ": " + message), type_(std::move(type)), stack_(std::move(stack))
    {
    }

    object::~object()
    {
        // Copies share state_; the last reset runs object_state::~object_state.
        state_.reset();
    }

    value object::call(std::string_view name, std::initializer_list<value> args, const call_options &options) const
    {
        return call(name, std::vector<value>(args), options);
    }
    value object::call(std::string_view name, const std::vector<value> &args, const call_options &options) const
    {
        const auto state = detail::runtime_state::require(*this);
        return state->runtime->perform(INTEROP_CALL, state, name, {args.begin(), args.end()}, options);
    }
    value object::get(std::string_view name) const
    {
        const auto state = detail::runtime_state::require(*this);
        return state->runtime->perform(INTEROP_GET, state, name);
    }
    void object::set(std::string_view name, value v) const
    {
        const auto state = detail::runtime_state::require(*this);
        state->runtime->perform(INTEROP_SET, state, name, {std::move(v)});
    }
    void object::dispose() const
    {
        const auto state = detail::runtime_state::require(*this);
        state->runtime->perform(INTEROP_DISPOSE, state);
    }
    void object::take_ownership() const
    {
        const auto state = detail::runtime_state::require(*this);
        state->runtime->perform(INTEROP_TAKE_OWNERSHIP, state);
    }
    object type::create(std::initializer_list<value> args, const call_options &options) const
    {
        return create(std::vector<value>(args), options);
    }
    object type::create(const std::vector<value> &args, const call_options &options) const
    {
        const auto state = detail::runtime_state::require(target_);
        return state->runtime->perform(INTEROP_CONSTRUCT, state, {}, {args.begin(), args.end()}, options).as<object>();
    }
    value type::call(std::string_view name, std::initializer_list<value> args, const call_options &options) const
    {
        return call(name, std::vector<value>(args), options);
    }
    value type::call(std::string_view name, const std::vector<value> &args, const call_options &options) const
    {
        const auto state = detail::runtime_state::require(target_);
        return state->runtime->perform(INTEROP_CALL_STATIC, state, name, {args.begin(), args.end()}, options);
    }
    value type::get(std::string_view name) const
    {
        const auto state = detail::runtime_state::require(target_);
        return state->runtime->perform(INTEROP_GET_STATIC, state, name);
    }
    void type::set(std::string_view name, value v) const
    {
        const auto state = detail::runtime_state::require(target_);
        state->runtime->perform(INTEROP_SET_STATIC, state, name, {std::move(v)});
    }
    runtime::runtime(const std::filesystem::path &bridge, const std::filesystem::path &dotnet_root)
        : state_(std::make_shared<detail::runtime_state>(detail::bootstrap(bridge, dotnet_root)))
    {
    }
    type runtime::load_type(const std::filesystem::path &assembly, std::string_view name) const
    {
        return type(
            state_
                ->perform(INTEROP_LOAD_TYPE, {}, name, {}, {}, detail::utf8_path(std::filesystem::canonical(assembly)))
                .as<object>());
    }
    type runtime::resolve_type(std::string_view name) const
    {
        return type(state_->perform(INTEROP_LOAD_TYPE, {}, name).as<object>());
    }
    std::int64_t runtime::live_handles() const
    {
        return state_->perform(INTEROP_LIVE_HANDLES).as<std::int64_t>();
    }
    interop_api runtime::native_api() const noexcept
    {
        return state_->api;
    }
} // namespace interop
