using System.Runtime.InteropServices;
using System.Text;

namespace Interop.Native;

internal sealed unsafe class RequestScope(Module module) : IDisposable
{
    private readonly List<IntPtr> buffers = [];
    private readonly List<HandlePin> pins = [];
    private void* Allocate(int bytes)
    {
        IntPtr memory = Marshal.AllocHGlobal(bytes);
        try { buffers.Add(memory); }
        catch { Marshal.FreeHGlobal(memory); throw; }
        return (void*)memory;
    }
    internal AbiValue Encode(object? input, int depth = 0)
    {
        if (depth > 32) throw new ArgumentException("Value nesting exceeds 32 levels.");
        AbiValue v = default;
        switch (input)
        {
            case null: break;
            case bool x: v.Kind = Kind.Bool; v.Scalar = x ? 1UL : 0; break;
            case sbyte x: v.Kind = Kind.I8; v.Scalar = unchecked((ulong)x); break;
            case byte x: v.Kind = Kind.U8; v.Scalar = x; break;
            case short x: v.Kind = Kind.I16; v.Scalar = unchecked((ulong)x); break;
            case ushort x: v.Kind = Kind.U16; v.Scalar = x; break;
            case char x: v.Kind = Kind.Char16; v.Scalar = x; break;
            case int x: v.Kind = Kind.I32; v.Scalar = unchecked((ulong)x); break;
            case uint x: v.Kind = Kind.U32; v.Scalar = x; break;
            case long x: v.Kind = Kind.I64; v.Scalar = unchecked((ulong)x); break;
            case ulong x: v.Kind = Kind.U64; v.Scalar = x; break;
            case float x: v.Kind = Kind.F32; v.Scalar = *(uint*)&x; break;
            case double x: v.Kind = Kind.F64; v.Scalar = *(ulong*)&x; break;
            case string x:
                byte[] bytes = Values.Utf8.GetBytes(x);
                v.Kind = Kind.Utf8;
                v.Length = bytes.Length;
                v.Data = Allocate(bytes.Length);
                Marshal.Copy(bytes, 0, (IntPtr)v.Data, bytes.Length);
                break;
            case NativeObject x:
                if (!ReferenceEquals(x.Handle.Module, module))
                    throw new ArgumentException("Cannot pass objects between NativeRuntime instances.");
                var pin = new HandlePin(x.Handle);
                try { pins.Add(pin); }
                catch { pin.Dispose(); throw; }
                v.Kind = Kind.Object;
                v.Scalar = x.Handle.Token;
                break;
            case Array x when x.Rank == 1 && x.GetLowerBound(0) == 0:
                v.Kind = Kind.Array;
                v.Length = x.Length;
                v.Data = Allocate(checked(x.Length * sizeof(AbiValue)));
                for (int i = 0; i < x.Length; i++) ((AbiValue*)v.Data)[i] = Encode(x.GetValue(i), depth + 1);
                break;
            default: throw new ArgumentException($"Unsupported interop argument type: {input.GetType()}.");
        }
        return v;
    }
    public void Dispose()
    {
        foreach (IntPtr memory in buffers) Marshal.FreeHGlobal(memory);
        buffers.Clear();
        foreach (HandlePin pin in pins) pin.Dispose();
        pins.Clear();
    }
}

internal static unsafe class Values
{
    internal static readonly UTF8Encoding Utf8 = new(false, true);
    private static void Validate(in AbiValue v)
    {
        if (v.Reserved != 0 || v.Length < 0 || v.Length > int.MaxValue || (v.Length != 0 && v.Data == null))
            throw new InvalidOperationException("Invalid native value buffer or reserved field.");
    }
    internal static string Text(in AbiValue v)
    {
        Validate(v);
        if (v.Kind != Kind.Utf8) throw new InvalidOperationException("Expected a native UTF-8 string.");
        return v.Length == 0 ? string.Empty : Utf8.GetString((byte*)v.Data, (int)v.Length);
    }
    internal static object? Decode(ref AbiValue v, Module module, List<NativeObject> adopted, int depth = 0)
    {
        if (depth > 32) throw new InvalidOperationException("Native value nesting exceeds 32 levels.");
        Validate(v);
        switch (v.Kind)
        {
            case Kind.Null: return null;
            case Kind.Bool when v.Scalar <= 1: return v.Scalar != 0;
            case Kind.I8: return unchecked((sbyte)v.Scalar);
            case Kind.U8: return unchecked((byte)v.Scalar);
            case Kind.I16: return unchecked((short)v.Scalar);
            case Kind.U16: return unchecked((ushort)v.Scalar);
            case Kind.Char16: return unchecked((char)v.Scalar);
            case Kind.I32: return unchecked((int)v.Scalar);
            case Kind.U32: return unchecked((uint)v.Scalar);
            case Kind.I64: return unchecked((long)v.Scalar);
            case Kind.U64: return v.Scalar;
            case Kind.F32: { uint bits = (uint)v.Scalar; return *(float*)&bits; }
            case Kind.F64: { ulong bits = v.Scalar; return *(double*)&bits; }
            case Kind.Utf8: return Text(v);
            case Kind.Object:
                if (v.Scalar == 0) throw new InvalidOperationException("Native bridge returned a zero lease.");
                var obj = new NativeObject(module, v.Scalar);
                v.Scalar = 0; // Transfer ownership before any further managed allocations.
                try { adopted.Add(obj); }
                catch { obj.Dispose(); throw; }
                return obj;
            case Kind.Array:
                var items = new object?[(int)v.Length];
                for (int i = 0; i < items.Length; i++)
                    items[i] = Decode(ref ((AbiValue*)v.Data)[i], module, adopted, depth + 1);
                return items;
            default: throw new InvalidOperationException("Unknown native value kind or invalid payload.");
        }
    }
    internal static T Cast<T>(object? value)
    {
        if (value is T typed) return typed;
        if (value is null && default(T) is null) return default!;
        DisposeValue(value);
        throw new InvalidCastException($"Native result {value?.GetType().FullName ?? "null"} cannot be cast to {typeof(T)}.");
    }
    private static void DisposeValue(object? value)
    {
        if (value is NativeObject obj) obj.Dispose();
        else if (value is object?[] array) foreach (object? item in array) DisposeValue(item);
    }
}
