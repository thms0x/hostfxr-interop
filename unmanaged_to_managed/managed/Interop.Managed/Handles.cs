using System.Runtime.InteropServices;
using System.Threading;

namespace Interop.Managed;

// Each exported lease is a normal (non-pinned) GCHandle to the managed object.
// C++ owns the lease through object_lease and releases it with RAII.
internal static class Handles
{
    private static long liveHandles;

    public static ulong Export(object target)
    {
        GCHandle handle = GCHandle.Alloc(target);
        Interlocked.Increment(ref liveHandles);
        return unchecked((ulong)GCHandle.ToIntPtr(handle).ToInt64());
    }

    // The ABI accepts handles only from this bridge and only while their C++
    // object_lease is alive. GCHandle cannot validate an arbitrary or stale token.
    private static GCHandle Find(ulong token)
    {
        if (token == 0) throw new ArgumentException("Null object handle.");
        return GCHandle.FromIntPtr(new IntPtr(unchecked((long)token)));
    }

    public static object Get(ulong token) => Find(token).Target ?? throw new ArgumentException("Invalid object handle.");

    public static void Release(ulong token)
    {
        Find(token).Free();
        Interlocked.Decrement(ref liveHandles);
    }

    public static void Dispose(ulong token) => (Get(token) as IDisposable)?.Dispose();

    public static long Count => Interlocked.Read(ref liveHandles);
}
