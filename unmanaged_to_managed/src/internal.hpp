#pragma once

#include <interop/abi.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace interop::detail
{
    // Start the selected CLR and obtain the managed bridge callbacks.
    interop_api bootstrap(const std::filesystem::path &assembly, const std::filesystem::path &dotnet_root);

    // Shared diagnostics and path conversion used by hosting and ABI calls.
    std::string status_message(std::string_view operation, int status);
    std::string utf8_path(const std::filesystem::path &path);
} // namespace interop::detail
