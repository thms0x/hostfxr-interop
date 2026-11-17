using System.Runtime.InteropServices;

namespace Interop.Native;

internal enum Kind { Null, Bool, I32, I64, U32, U64, F32, F64, Utf8, Object, Array, I8, U8, I16, U16, Char16 }
internal enum Operation { LoadType = 1, Construct, CallStatic, Call, GetStatic, SetStatic, Get, Set, Release, LiveHandles }
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
    public AbiValue TypeName, Name, Arguments;
}
[StructLayout(LayoutKind.Sequential)]
internal struct Result { public AbiValue Value, ErrorType, ErrorMessage; }
[StructLayout(LayoutKind.Sequential)]
internal struct Api { public uint Size, Version; public IntPtr Invoke, FreeResult; }
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
internal unsafe delegate int GetApi(uint version, uint size, Api* output);
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
internal unsafe delegate int Invoke(Request* request, Result* result);
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
internal unsafe delegate void FreeResult(Result* result);
