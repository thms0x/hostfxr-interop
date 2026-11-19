#include <interop/abi.h>
#include <interop/interop.hpp>

#ifndef INTEROP_NETFRAMEWORK
#include <coreclr_delegates.h>
#include <hostfxr.h>
#include <nethost.h>
#endif

#include <cstring>
#include <limits>
#include <mutex>
#include <sstream>
#include <type_traits>
#include <unordered_map>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#ifdef INTEROP_NETFRAMEWORK
#include <metahost.h>
#include <mscoree.h>
#endif
#define HOST_TEXT(text) L##text
#else
#include <dlfcn.h>
#define HOST_TEXT(text) text
#endif

namespace interop
{
    // Public errors are translated from the C ABI result.
    managed_error::managed_error(std::string type, std::string message, std::string stack)
        : std::runtime_error(type + ": " + message), type_(std::move(type)), stack_(std::move(stack))
    {
    }

    // Hosting and value helpers stay private to this implementation file.
    namespace
    {
        std::string status_message(std::string_view operation, int status)
        {
            std::ostringstream out;
            out << operation << " failed (0x" << std::hex << static_cast<std::uint32_t>(status) << ')';
            return out.str();
        }

        std::string utf8_path(const std::filesystem::path &path)
        {
            auto text = path.u8string();
            return std::string(reinterpret_cast<const char *>(text.data()), text.size());
        }

#ifndef INTEROP_NETFRAMEWORK
        using host_string = std::basic_string<char_t>;
        host_string host_path(const std::filesystem::path &path)
        {
#ifdef _WIN32
            return path.wstring();
#else
            return path.string();
#endif
        }

        struct host_exports
        {
            hostfxr_initialize_for_runtime_config_fn initialize;
            hostfxr_get_runtime_delegate_fn get_delegate;
            hostfxr_close_fn close;
        };

        // .NET hosting libraries and the runtime are process-lifetime resources. Never
        // dlclose/FreeLibrary them, including when a runtime wrapper is destroyed.
        host_exports load_host(const std::filesystem::path &dotnet_root)
        {
            static std::mutex gate;
            static std::unordered_map<host_string, host_exports> modules;
            host_string root = dotnet_root.empty() ? host_string{} : host_path(std::filesystem::absolute(dotnet_root));
            get_hostfxr_parameters parameters{sizeof(get_hostfxr_parameters), nullptr,
                                              root.empty() ? nullptr : root.c_str()};
            std::size_t size = 0;
            int status = get_hostfxr_path(nullptr, &size, &parameters);
            if (size == 0)
                throw std::runtime_error(status_message("Locate hostfxr; install a matching .NET runtime", status));
            std::vector<char_t> path(size);
            status = get_hostfxr_path(path.data(), &size, &parameters);
            if (status != 0)
                throw std::runtime_error(status_message("Locate hostfxr", status));
            std::lock_guard lock(gate);
            if (auto found = modules.find(path.data()); found != modules.end())
                return found->second;
#ifdef _WIN32
            HMODULE module = LoadLibraryW(path.data());
            if (!module)
                throw std::runtime_error("Cannot load hostfxr: " + std::to_string(GetLastError()));
            auto symbol = [module](const char *name)
            { return GetProcAddress(module, name); };
#else
            void *module = dlopen(path.data(), RTLD_NOW | RTLD_LOCAL);
            if (!module)
                throw std::runtime_error(std::string("Cannot load hostfxr: ") + dlerror());
            auto symbol = [module](const char *name)
            { return dlsym(module, name); };
#endif
            host_exports exports{
                reinterpret_cast<hostfxr_initialize_for_runtime_config_fn>(
                    symbol("hostfxr_initialize_for_runtime_config")),
                reinterpret_cast<hostfxr_get_runtime_delegate_fn>(symbol("hostfxr_get_runtime_delegate")),
                reinterpret_cast<hostfxr_close_fn>(symbol("hostfxr_close"))};
            if (!exports.initialize || !exports.get_delegate || !exports.close)
                throw std::runtime_error("hostfxr is missing a required hosting export");
            modules.emplace(path.data(), exports);
            return exports;
        }

        interop_api bootstrap(const std::filesystem::path &assembly, const std::filesystem::path &dotnet_root)
        {
            const auto full_path = std::filesystem::canonical(assembly);
            auto config = full_path;
            config.replace_extension(".runtimeconfig.json");
            if (!std::filesystem::is_regular_file(config))
                throw std::runtime_error("Missing bridge runtime configuration: " + utf8_path(config));
            const auto host = load_host(dotnet_root);
            hostfxr_handle context = nullptr;
            int status = host.initialize(host_path(config).c_str(), nullptr, &context);
            struct context_guard
            {
                hostfxr_handle value;
                hostfxr_close_fn close;
                ~context_guard()
                {
                    if (value)
                        close(value);
                }
            } guard{context, host.close};
            // Positive statuses include Success_HostAlreadyInitialized and are valid.
            if (status < 0 || !context)
                throw std::runtime_error(status_message("Initialize .NET", status));
            void *loader_pointer = nullptr;
            status = host.get_delegate(context, hdt_load_assembly_and_get_function_pointer, &loader_pointer);
            if (status != 0 || !loader_pointer)
                throw std::runtime_error(status_message("Get assembly loader", status));
            const auto loader = reinterpret_cast<load_assembly_and_get_function_pointer_fn>(loader_pointer);
            void *entry = nullptr;
            status = loader(host_path(full_path).c_str(), HOST_TEXT("Interop.Managed.EntryPoint, Interop.Managed"),
                            HOST_TEXT("GetApi"), UNMANAGEDCALLERSONLY_METHOD, nullptr, &entry);
            if (status != 0 || !entry)
                throw std::runtime_error(status_message("Load managed bridge", status));
            interop_api api{};
            status = reinterpret_cast<interop_get_api_fn>(entry)(INTEROP_ABI_VERSION, sizeof(api), &api);
            if (status != 0 || api.size != sizeof(api) || api.version != INTEROP_ABI_VERSION || !api.invoke ||
                !api.free_result)
                throw std::runtime_error("Managed bridge ABI version or layout mismatch");
            return api;
        }

#else
        template <class T>
        struct com_ptr
        {
            T *value = nullptr;
            ~com_ptr()
            {
                if (value)
                    value->Release();
            }
            com_ptr() = default;
            com_ptr(const com_ptr &) = delete;
            com_ptr &operator=(const com_ptr &) = delete;
        };

        void check_hresult(HRESULT status, std::string_view operation)
        {
            if (FAILED(status))
                throw std::runtime_error(status_message(operation, status));
        }

        interop_api bootstrap(const std::filesystem::path &assembly, const std::filesystem::path &dotnet_root)
        {
            if (!dotnet_root.empty())
                throw std::invalid_argument("dotnet_root is only supported by the modern .NET host");
            const auto full_path = std::filesystem::canonical(assembly);
            // Retain the host for process lifetime; stopping the CLR invalidates live leases.
            static ICLRRuntimeHost *host = []
            {
                com_ptr<ICLRMetaHost> meta;
                check_hresult(
                    CLRCreateInstance(CLSID_CLRMetaHost, IID_ICLRMetaHost, reinterpret_cast<void **>(&meta.value)),
                    "Create CLR meta host");
                com_ptr<ICLRRuntimeInfo> info;
                check_hresult(
                    meta.value->GetRuntime(L"v4.0.30319", IID_ICLRRuntimeInfo, reinterpret_cast<void **>(&info.value)),
                    "Locate .NET Framework CLR v4");
                BOOL loadable = FALSE;
                check_hresult(info.value->IsLoadable(&loadable), "Check CLR availability");
                if (!loadable)
                    throw std::runtime_error(".NET Framework CLR v4 cannot be loaded in this process");
                com_ptr<ICLRRuntimeHost> runtime;
                check_hresult(info.value->GetInterface(CLSID_CLRRuntimeHost, IID_ICLRRuntimeHost,
                                                       reinterpret_cast<void **>(&runtime.value)),
                              "Get CLR runtime host");
                check_hresult(runtime.value->Start(), "Start .NET Framework CLR");
                auto *result = runtime.value;
                runtime.value = nullptr;
                return result;
            }();
            interop_api api{};
            api.size = sizeof(api);
            api.version = INTEROP_ABI_VERSION;
            const auto address = std::to_wstring(reinterpret_cast<std::intptr_t>(&api));
            DWORD status = 1;
            check_hresult(host->ExecuteInDefaultAppDomain(full_path.wstring().c_str(), L"Interop.Managed.EntryPoint",
                                                          L"Initialize", address.c_str(), &status),
                          "Load .NET Framework bridge");
            if (status != 0 || api.size != sizeof(api) || api.version != INTEROP_ABI_VERSION || !api.invoke ||
                !api.free_result)
                throw std::runtime_error("Managed bridge ABI version or layout mismatch");
            return api;
        }
#endif

        template <class To, class From>
        To bit_copy(const From &source) noexcept
        {
            static_assert(sizeof(To) == sizeof(From));
            static_assert(std::is_trivially_copyable_v<To> && std::is_trivially_copyable_v<From>);
            To result;
            std::memcpy(&result, &source, sizeof(result));
            return result;
        }

        struct encoded_value
        {
            interop_value raw{};
            std::vector<std::unique_ptr<encoded_value>> children;
            std::vector<interop_value> elements;
        };

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

    // One exported managed handle. Copies of interop::object share this lease.
    class object_lease
    {
    public:
        std::shared_ptr<runtime_core> core;
        std::uint64_t handle;
        object_lease(std::shared_ptr<runtime_core> owner, std::uint64_t lease) : core(std::move(owner)), handle(lease)
        {
        }
        ~object_lease();
    };

    // Owns the ABI callbacks and translates public C++ operations into requests.
    class runtime_core : public std::enable_shared_from_this<runtime_core>
    {
    public:
        interop_api api;
        explicit runtime_core(interop_api functions) : api(functions)
        {
        }

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
                raw.scalar = 0; // Ownership transferred; free_result must not release it.
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
            return decode(result.value);
        }

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
        : core_(std::make_shared<runtime_core>(bootstrap(bridge, dotnet_root)))
    {
    }
    type runtime::load_type(const std::filesystem::path &assembly, std::string_view name) const
    {
        return type(core_->perform(INTEROP_LOAD_TYPE, {}, name, {}, {}, utf8_path(std::filesystem::canonical(assembly)))
                        .as<object>());
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
