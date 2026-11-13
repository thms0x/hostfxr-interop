using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Interop.Managed;

internal enum Kind : int { Null, Bool, I32, I64, U32, U64, F32, F64, Utf8, Object, Array, I8, U8, I16, U16, Char16 }
internal enum Operation : int
{
    LoadType = 1, Construct, CallStatic, Call, GetStatic, SetStatic, Get, Set,
    Release, Dispose, LiveHandles, TakeOwnership
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct AbiValue
{
    public Kind Kind;
    public int Reserved;
    public ulong Scalar;
    public void* Data;
    public long Length;
}

[StructLayout(LayoutKind.Sequential)]
internal struct Request
{
    public uint Size, Version;
    public Operation Operation;
    public int Reserved;
    public ulong Target;
    public AbiValue Name, Assembly, Arguments, ParameterTypes, GenericTypes;
}

[StructLayout(LayoutKind.Sequential)]
internal struct Result
{
    public AbiValue Value, ErrorType, ErrorMessage, ErrorStack;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct Api
{
    public uint Size, Version;
    public delegate* unmanaged[Cdecl]<Request*, Result*, int> Invoke;
    public delegate* unmanaged[Cdecl]<Result*, void> FreeResult;
}

public static unsafe class EntryPoint
{
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    public static int GetApi(uint version, uint size, void* output)
    {
        if (version != 1 || size != sizeof(Api) || output == null || IntPtr.Size != 8 ||
            sizeof(AbiValue) != 32 || sizeof(Request) != 184 || sizeof(Result) != 128)
            return 1;
        *(Api*)output = new Api { Size = (uint)sizeof(Api), Version = 1,
            Invoke = &Bridge.Invoke, FreeResult = &Bridge.FreeResult };
        return 0;
    }
}
