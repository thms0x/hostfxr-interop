# C++ / .NET interop

The primary target is **C++17 and .NET Framework 4.8 on Windows x64**.
Native C++ applications can construct managed objects, call methods, access
properties, and receive managed exceptions through a stable C ABI. Exported object
leases keep managed objects alive; constructed `IDisposable` objects are disposed
when their final lease is released.

## Build for .NET Framework 4.8

Use Visual Studio 2022 with the C++ desktop tools and Windows SDK, CMake 3.22+,
and a .NET SDK 8+ to compile the C# bridge. The resulting bridge targets `net48`;
applications require the .NET Framework 4.8 runtime, not a modern .NET runtime.
The build restores the Framework reference assemblies from NuGet.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
.\build\Release\interop_example.exe .\build\managed\interop_bridge\Interop.Managed.dll
```

`INTEROP_TARGET_FRAMEWORK` defaults to `net48`. The native library hosts CLR v4
using [`ICLRRuntimeHost`](https://learn.microsoft.com/en-us/dotnet/framework/unmanaged-api/hosting/iclrruntimehost-executeindefaultappdomain-method); no hostfxr pack or runtimeconfig file is needed for this
target. Managed callbacks use delegates rooted for the lifetime of the AppDomain.
Component assemblies load into the default AppDomain with `Assembly.LoadFrom`;
dependencies should be deployed alongside them. Framework assembly identity and
binding rules apply, so this mode does not isolate conflicting dependency versions.
The runtime remains loaded for the process lifetime.

## Optional modern .NET host

Modern .NET 8+ remains available on supported 64-bit platforms. Select the target
explicitly, using an installed targeting pack and matching runtime:

```sh
cmake -S . -B build-modern -DINTEROP_TARGET_FRAMEWORK=net8.0 -DCMAKE_BUILD_TYPE=Release
cmake --build build-modern --parallel
ctest --test-dir build-modern --output-on-failure
```

This mode uses hostfxr and the `.runtimeconfig.json` beside the bridge assembly.
Use a separate build directory when changing runtime targets. Linux and macOS
require this explicit option; the default Framework host requires Windows.

## C++17 usage

Link the CMake target `interop::interop` and include `<interop/interop.hpp>`.
Arguments accept initializer lists or `std::vector<interop::value>`.
Existing callers using the former C++20 `std::span` overloads should pass vectors.

```cpp
interop::runtime runtime("Interop.Managed.dll");
auto type = runtime.resolve_type("System.Text.StringBuilder");
auto builder = type.create({"Hello"});
builder.call("Append", {" from C++17"});
auto text = builder.call("ToString").as<std::string>();
```

Use unqualified core type names, such as `System.String`, across both runtimes.
Explicit core assembly names differ: `mscorlib` on Framework and
`System.Private.CoreLib` on modern .NET. The bridge and component assemblies must
target the selected runtime. ABI v1 requires 64-bit processes; x86 is unsupported.
