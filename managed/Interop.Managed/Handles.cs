using System.Runtime.CompilerServices;

namespace Interop.Managed;

// Each export gets a fresh token. Identity tracking joins aliases so an owned
// IDisposable is disposed exactly once after its final lease, including returns.
internal static class Handles
{
    private sealed class Entry(object target)
    {
        public readonly object Target = target;
        public int References;
        public bool Owned, Disposed;
    }

    private static readonly object Gate = new();
    private static readonly Dictionary<ulong, Entry> Leases = [];
    private static readonly ConditionalWeakTable<object, Entry> Identities = new();
    private static ulong next;

    public static ulong Export(object target, bool owned)
    {
        lock (Gate)
        {
            Entry entry = Identities.GetValue(target, static key => new Entry(key));
            ObjectDisposedException.ThrowIf(entry.Disposed, target);
            if (next == ulong.MaxValue) throw new OverflowException("Object lease space exhausted.");
            ulong token = ++next;
            Leases.Add(token, entry);
            entry.References++;
            entry.Owned |= owned;
            return token;
        }
    }

    public static object Get(ulong token)
    {
        lock (Gate)
        {
            Entry entry = Find(token);
            ObjectDisposedException.ThrowIf(entry.Disposed, entry.Target);
            return entry.Target;
        }
    }

    private static Entry Find(ulong token) => Leases.TryGetValue(token, out Entry? entry)
        ? entry : throw new ArgumentException($"Unknown or released object lease {token}.");

    public static void Release(ulong token)
    {
        object? dispose = null;
        lock (Gate)
        {
            Entry entry = Find(token);
            Leases.Remove(token);
            if (--entry.References == 0 && entry.Owned && !entry.Disposed)
            {
                entry.Disposed = true;
                dispose = entry.Target;
            }
        }
        // Never execute user code while holding the registry lock.
        (dispose as IDisposable)?.Dispose();
    }

    public static void Dispose(ulong token)
    {
        object? dispose;
        lock (Gate)
        {
            Entry entry = Find(token);
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
            Entry entry = Find(token);
            ObjectDisposedException.ThrowIf(entry.Disposed, entry.Target);
            entry.Owned = true;
        }
    }

    public static long Count { get { lock (Gate) return Leases.Count; } }
}
