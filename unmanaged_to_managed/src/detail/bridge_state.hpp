#pragma once

#include <interop/interop.hpp>
#include <utility>

namespace interop::detail
{
    // Internal boundary shared by the host, ABI transport, and public wrappers.
    std::string status_message(std::string_view operation, int status);
    std::string utf8_path(const std::filesystem::path &path);
    interop_api bootstrap(const std::filesystem::path &assembly, const std::filesystem::path &dotnet_root);

    struct encoded_value
    {
        interop_value raw{};
        std::vector<std::unique_ptr<encoded_value>> children;
        std::vector<interop_value> elements;
    };

    struct object_state
    {
        std::shared_ptr<runtime_state> runtime;
        std::uint64_t token;
        object_state(std::shared_ptr<runtime_state> owner, std::uint64_t lease)
            : runtime(std::move(owner)), token(lease)
        {
        }
        ~object_state();
    };

    struct runtime_state : std::enable_shared_from_this<runtime_state>
    {
        interop_api api;
        explicit runtime_state(interop_api functions) : api(functions)
        {
        }

        std::unique_ptr<encoded_value> encode(const value &input, int depth = 0);
        value decode(interop_value &raw, int depth = 0);
        value perform(int operation, const std::shared_ptr<object_state> &target = {}, std::string_view name = {},
                      value::array args = {}, const call_options &options = {}, std::string_view assembly = {});
        void release(std::uint64_t token) noexcept;
        static const std::shared_ptr<object_state> &require(const object &target);
    };
} // namespace interop::detail
