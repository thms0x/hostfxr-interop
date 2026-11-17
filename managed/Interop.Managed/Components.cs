using System.Collections.Concurrent;
using System.Reflection;
#if !NETFRAMEWORK
using System.Runtime.Loader;
#endif

namespace Interop.Managed;

internal static class Components
{
#if !NETFRAMEWORK
    private sealed class ComponentContext(string path) : AssemblyLoadContext(isCollectible: false)
    {
        private readonly AssemblyDependencyResolver resolver = new(path);
        protected override Assembly? Load(AssemblyName name)
        {
            string? dependency = resolver.ResolveAssemblyToPath(name);
            return dependency is null ? null : LoadFromAssemblyPath(dependency);
        }
        protected override IntPtr LoadUnmanagedDll(string name)
        {
            string? dependency = resolver.ResolveUnmanagedDllToPath(name);
            return dependency is null ? IntPtr.Zero : LoadUnmanagedDllFromPath(dependency);
        }
    }

#endif

    private static readonly ConcurrentDictionary<string, Lazy<Assembly>> Assemblies = new(
        Path.DirectorySeparatorChar == '\\' ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal);

    public static Type LoadType(string? path, string name)
    {
        if (string.IsNullOrEmpty(path)) return Resolve(name, null);
        string fullPath = Path.GetFullPath(path);
        Assembly assembly = Assemblies.GetOrAdd(fullPath, static p => new Lazy<Assembly>(
#if NETFRAMEWORK
            () => Assembly.LoadFrom(p))).Value;
#else
            () => new ComponentContext(p).LoadFromAssemblyPath(p))).Value;
#endif
        Type type = Resolve(name, assembly);
        if (!type.IsVisible) throw new TypeAccessException($"Type {type} is not public.");
        return type;
    }

    public static Type Resolve(string name, Assembly? component)
    {
        Type? type = component?.GetType(name, throwOnError: false) ?? Type.GetType(name, throwOnError: false);
        if (type is null && component is not null)
        {
#if !NETFRAMEWORK
            var context = AssemblyLoadContext.GetLoadContext(component)!;
#endif
            type = Type.GetType(name,
#if NETFRAMEWORK
                assemblyName => Assembly.Load(assemblyName),
#else
                assemblyName => context.LoadFromAssemblyName(assemblyName),
#endif
                (assembly, typeName, ignoreCase) => (assembly ?? component).GetType(typeName, false, ignoreCase), false);
        }
        type = type ?? throw new TypeLoadException($"Could not resolve managed type '{name}'. Use its full or assembly-qualified name.");
        if (!type.IsVisible) throw new TypeAccessException($"Type {type} is not public.");
        return type;
    }
}
