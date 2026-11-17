cmake_minimum_required(VERSION 3.22)
project(interop VERSION 0.1.0 LANGUAGES C CXX)

include(CTest)
option(INTEROP_BUILD_UNMANAGED_TO_MANAGED "Build the C++ to .NET bridge" ON)
option(INTEROP_BUILD_MANAGED_TO_UNMANAGED "Build the .NET to C++ bridge" ON)

if(INTEROP_BUILD_UNMANAGED_TO_MANAGED)
    add_subdirectory(unmanaged_to_managed)
endif()
if(INTEROP_BUILD_MANAGED_TO_UNMANAGED)
    add_subdirectory(managed_to_umanaged)
endif()
