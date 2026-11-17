using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Interop.Managed;

// Each exported lease is a normal (non-pinned) GCHandle to an identity record.
// C++ owns the lease through object_lease and releases it with RAII. The identity
// record lets aliases share the same disposal policy even though they have
// different GCHandles.
internal static class Handles
{
    private sealed class Entry(object target)
    {
        public readonly object Target = target;
        public int References;
        public bool Owned, Disposed;
    }

    private static readonly object Gate = new();
    private static readonly ConditionalWeakTable<object, Entry> Identities = new();
    private static long liveHandles;

    public static ulong Export(object target, bool owned)
    {
        lock (Gate)
        {
            Entry entry = Identities.GetValue(target, static key => new Entry(key));
            if (entry.Disposed) throw new ObjectDisposedException(target.GetType().FullName);
            GCHandle handle = GCHandle.Alloc(entry);
            entry.References++;
            entry.Owned |= owned;
            liveHandles++;
            return unchecked((ulong)GCHandle.ToIntPtr(handle).ToInt64());
        }
    }

    // The ABI accepts handles only from this bridge and only while their C++
    // object_lease is alive. GCHandle cannot validate an arbitrary or stale token.
    private static (GCHandle Handle, Entry Entry) Find(ulong token)
    {
        if (token == 0) throw new ArgumentException("Null object handle.");
        GCHandle handle = GCHandle.FromIntPtr(new IntPtr(unchecked((long)token)));
        return (handle, handle.Target as Entry ?? throw new ArgumentException("Invalid object handle."));
    }

    public static object Get(ulong token)
    {
        lock (Gate)
        {
            Entry entry = Find(token).Entry;
            if (entry.Disposed) throw new ObjectDisposedException(entry.Target.GetType().FullName);
            return entry.Target;
        }
    }

    public static void Release(ulong token)
    {
        object? dispose = null;
        lock (Gate)
        {
            (GCHandle handle, Entry entry) = Find(token);
            handle.Free();
            liveHandles--;
            if (--entry.References == 0 && entry.Owned && !entry.Disposed)
            {
                entry.Disposed = true;
                dispose = entry.Target;
            }
        }
        // Never execute user code while holding the bridge lock.
        (dispose as IDisposable)?.Dispose();
    }

    public static void Dispose(ulong token)
    {
        object? dispose;
        lock (Gate)
        {
            Entry entry = Find(token).Entry;
            if (entry.Disposed) return;
            entry.Disposed = true;
            dispose = entry.Target;
        }
        (dispose as IDisposable)?.Dispose();
    }

    public static void TakeOwnership(ulong token)
    {
        lock (Gate)
        {
            Entry entry = Find(token).Entry;
            if (entry.Disposed) throw new ObjectDisposedException(entry.Target.GetType().FullName);
            entry.Owned = true;
        }
    }

    public static long Count { get { lock (Gate) return liveHandles; } }
}
