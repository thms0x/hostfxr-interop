using System.Runtime.InteropServices;
using System.Text;

namespace Interop.Managed;

internal sealed class ArrayArgument(object?[] items)
{
    public object?[] Items { get; } = items;
}

internal static unsafe class Marshalling
{
    private static readonly UTF8Encoding Utf8 = new(false, true);
    private const int MaxDepth = 32;

    private static float FloatFromBits(uint bits) => *(float*)&bits;
    private static uint FloatBits(float value) => *(uint*)&value;

    private static void* AllocateArray(int length)
    {
        var values = (AbiValue*)Marshal.AllocHGlobal(checked(length * sizeof(AbiValue)));
        for (int i = 0; i < length; i++) values[i] = default;
        return values;
    }

    public static string Text(in AbiValue value)
    {
        if (value.Kind != Kind.Utf8) throw new ArgumentException("Expected a UTF-8 string.");
        ValidateBuffer(value);
        return value.Length == 0 ? string.Empty : Utf8.GetString((byte*)value.Data, (int)value.Length);
    }

    private static void ValidateBuffer(in AbiValue value)
    {
        if (value.Length < 0 || value.Length > int.MaxValue || (value.Length != 0 && value.Data == null))
            throw new ArgumentException("Invalid buffer length or null buffer.");
    }

    public static object? Read(in AbiValue v, int depth = 0)
    {
        if (depth > MaxDepth) throw new ArgumentException("Value nesting exceeds 32 levels.");
        if (v.Reserved != 0) throw new ArgumentException("Reserved value field must be zero.");
        return v.Kind switch
        {
            Kind.Null => null,
            Kind.Bool when v.Scalar <= 1 => v.Scalar != 0,
            Kind.I32 => unchecked((int)v.Scalar),
            Kind.I64 => unchecked((long)v.Scalar),
            Kind.U32 => unchecked((uint)v.Scalar),
            Kind.U64 => v.Scalar,
            Kind.F32 => FloatFromBits(unchecked((uint)v.Scalar)),
            Kind.F64 => BitConverter.Int64BitsToDouble(unchecked((long)v.Scalar)),
            Kind.I8 => unchecked((sbyte)v.Scalar),
            Kind.U8 => unchecked((byte)v.Scalar),
            Kind.I16 => unchecked((short)v.Scalar),
            Kind.U16 => unchecked((ushort)v.Scalar),
            Kind.Char16 => unchecked((char)v.Scalar),
            Kind.Utf8 => Text(v),
            Kind.Object => Handles.Get(v.Scalar),
            Kind.Array => new ArrayArgument(ReadArray(v, depth)),
            _ => throw new ArgumentException($"Unsupported value kind or payload: {v.Kind}.")
        };
    }

    public static object?[] ReadArray(in AbiValue value, int depth = 0)
    {
        if (value.Kind != Kind.Array) throw new ArgumentException("Expected an array.");
        ValidateBuffer(value);
        var items = new object?[(int)value.Length];
        var values = (AbiValue*)value.Data;
        for (int i = 0; i < items.Length; i++) items[i] = Read(values[i], depth + 1);
        return items;
    }

    public static string[] Strings(in AbiValue value) => value.Kind == Kind.Null ? [] :
        ReadArray(value).Select(v => v as string ?? throw new ArgumentException("Expected type names.")).ToArray();

    public static void Write(ref AbiValue destination, object? value, bool owned = false, int depth = 0)
    {
        if (depth > MaxDepth) throw new ArgumentException("Value nesting exceeds 32 levels (possibly a cyclic array).");
        destination = default;
        // Constructors always return an object lease, including boxed structs,
        // strings, and arrays. Subsequent method results use value marshalling.
        if (owned && value is not null)
        {
            destination.Scalar = Handles.Export(value, true);
            destination.Kind = Kind.Object;
            return;
        }
        switch (value)
        {
            case null: return;
            case bool v: destination.Kind = Kind.Bool; destination.Scalar = v ? 1UL : 0; return;
            case int v: destination.Kind = Kind.I32; destination.Scalar = unchecked((ulong)v); return;
            case long v: destination.Kind = Kind.I64; destination.Scalar = unchecked((ulong)v); return;
            case uint v: destination.Kind = Kind.U32; destination.Scalar = v; return;
            case ulong v: destination.Kind = Kind.U64; destination.Scalar = v; return;
            case float v: destination.Kind = Kind.F32; destination.Scalar = FloatBits(v); return;
            case double v: destination.Kind = Kind.F64; destination.Scalar = unchecked((ulong)BitConverter.DoubleToInt64Bits(v)); return;
            case sbyte v: destination.Kind = Kind.I8; destination.Scalar = unchecked((ulong)v); return;
            case byte v: destination.Kind = Kind.U8; destination.Scalar = v; return;
            case short v: destination.Kind = Kind.I16; destination.Scalar = unchecked((ulong)v); return;
            case ushort v: destination.Kind = Kind.U16; destination.Scalar = v; return;
            case char v: destination.Kind = Kind.Char16; destination.Scalar = v; return;
            case string v:
                byte[] bytes = Utf8.GetBytes(v);
                destination.Data = (void*)Marshal.AllocHGlobal(bytes.Length);
                destination.Kind = Kind.Utf8;
                destination.Length = bytes.Length;
                Marshal.Copy(bytes, 0, (IntPtr)destination.Data, bytes.Length);
                return;
            case Array v when v.Rank == 1 && v.GetLowerBound(0) == 0:
                destination.Data = AllocateArray(v.Length);
                destination.Kind = Kind.Array;
                destination.Length = v.Length;
                var values = (AbiValue*)destination.Data;
                for (int i = 0; i < v.Length; i++) Write(ref values[i], v.GetValue(i), depth: depth + 1);
                return;
            default:
                destination.Scalar = Handles.Export(value, owned);
                destination.Kind = Kind.Object;
                return;
        }
    }

    // Cleanup continues even if a user IDisposable throws.
    public static void Free(ref AbiValue value)
    {
        if (value.Kind == Kind.Array)
        {
            var values = (AbiValue*)value.Data;
            for (long i = 0; i < value.Length; i++) Free(ref values[i]);
            Marshal.FreeHGlobal((IntPtr)value.Data);
        }
        else if (value.Kind == Kind.Utf8) Marshal.FreeHGlobal((IntPtr)value.Data);
        else if (value.Kind == Kind.Object && value.Scalar != 0)
        {
            try { Handles.Release(value.Scalar); }
            catch { /* Cleanup at a noexcept ABI boundary. Use explicit Dispose to observe errors. */ }
        }
        value = default;
    }
}
