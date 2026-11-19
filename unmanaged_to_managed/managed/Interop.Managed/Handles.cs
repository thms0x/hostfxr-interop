using System.Runtime.InteropServices;
using System.Threading;

namespace Interop.Managed;

// Keeps exported objects reachable while C++ owns their normal, non-pinned GCHandles.
internal static class Handles
{
    private static long liveHandles;

    // Root a managed object and return its opaque token to C++.
    public static ulong Export(object target)
    {
        GCHandle handle = GCHandle.Alloc(target);
        Interlocked.Increment(ref liveHandles);
        return unchecked((ulong)GCHandle.ToIntPtr(handle).ToInt64());
    }

    // Decode a live bridge token; GCHandle cannot validate stale or foreign tokens.
    private static GCHandle Find(ulong token)
    {
        if (token == 0) throw new ArgumentException("Null object handle.");
        return GCHandle.FromIntPtr(new IntPtr(unchecked((long)token)));
    }

    // Retrieve the managed target held by a live C++ lease.
    public static object Get(ulong token) => Find(token).Target ?? throw new ArgumentException("Invalid object handle.");

    // Free one GC root without disposing the underlying managed object.
    public static void Release(ulong token)
    {
        Find(token).Free();
        Interlocked.Decrement(ref liveHandles);
    }

    // Dispose a resource-bearing target without releasing its C++ handle.
    public static void Dispose(ulong token) => (Get(token) as IDisposable)?.Dispose();

    // Report outstanding exported handles for diagnostics and tests.
    public static long Count => Interlocked.Read(ref liveHandles);
}
