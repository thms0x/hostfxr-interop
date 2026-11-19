#include "internal.hpp"

#ifndef INTEROP_NETFRAMEWORK
#include <coreclr_delegates.h>
#include <hostfxr.h>
#include <nethost.h>
#endif

#include <cstdint>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

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

namespace interop::detail
{
    // Format a native hosting status for a C++ exception.
    std::string status_message(std::string_view operation, int status)
    {
        std::ostringstream out;
        out << operation << " failed (0x" << std::hex << static_cast<std::uint32_t>(status) << ')';
        return out.str();
    }

    // Preserve UTF-8 when passing a path through the value ABI.
    std::string utf8_path(const std::filesystem::path &path)
    {
        auto text = path.u8string();
        return std::string(reinterpret_cast<const char *>(text.data()), text.size());
    }

    namespace
    {
#ifndef INTEROP_NETFRAMEWORK
        using host_string = std::basic_string<char_t>;
        // Use the character width required by the platform's .NET host API.
        host_string host_path(const std::filesystem::path &path)
        {
#ifdef _WIN32
            return path.wstring();
#else
            return path.string();
#endif
        }

        // Functions needed to initialize and query a modern .NET host.
        struct host_exports
        {
            hostfxr_initialize_for_runtime_config_fn initialize;
            hostfxr_get_runtime_delegate_fn get_delegate;
            hostfxr_close_fn close;
        };

        // Locate and cache hostfxr for the process lifetime.
        // The loaded runtime must outlive every wrapper and callback.
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

        // Start modern .NET and obtain the bridge's versioned callback table.
        interop_api bootstrap_hostfxr(const std::filesystem::path &assembly, const std::filesystem::path &dotnet_root)
        {
            const auto full_path = std::filesystem::canonical(assembly);
            auto config = full_path;
            config.replace_extension(".runtimeconfig.json");
            if (!std::filesystem::is_regular_file(config))
                throw std::runtime_error("Missing bridge runtime configuration: " + utf8_path(config));
            const auto host = load_host(dotnet_root);
            hostfxr_handle context = nullptr;
            int status = host.initialize(host_path(config).c_str(), nullptr, &context);
            // Close the temporary hostfxr context after retrieving the entry point.
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
        // Release temporary CLR hosting interfaces on every exit path.
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

        // Raise a C++ exception for a failed CLR hosting call.
        void check_hresult(HRESULT status, std::string_view operation)
        {
            if (FAILED(status))
                throw std::runtime_error(status_message(operation, status));
        }

        // Start CLR v4 and ask the managed entry point for its callback table.
        interop_api bootstrap_netframework(const std::filesystem::path &assembly,
                                           const std::filesystem::path &dotnet_root)
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
    } // namespace

    // Select the host at build time; the public runtime constructor uses one entry point.
    interop_api bootstrap(const std::filesystem::path &assembly, const std::filesystem::path &dotnet_root)
    {
#ifdef INTEROP_NETFRAMEWORK
        return bootstrap_netframework(assembly, dotnet_root);
#else
        return bootstrap_hostfxr(assembly, dotnet_root);
#endif
    }
} // namespace interop::detail
