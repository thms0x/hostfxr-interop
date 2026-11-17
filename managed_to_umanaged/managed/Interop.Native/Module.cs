using System.ComponentModel;
using System.Runtime.InteropServices;

namespace Interop.Native;

internal static class Loader
{
#if NETFRAMEWORK
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true, ExactSpelling = true)]
    private static extern IntPtr LoadLibraryW(string path);
    [DllImport("kernel32.dll", CharSet = CharSet.Ansi, SetLastError = true, ExactSpelling = true)]
    private static extern IntPtr GetProcAddress(IntPtr library, string name);
    [DllImport("kernel32.dll", ExactSpelling = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool FreeLibrary(IntPtr library);
    public static IntPtr Load(string path)
    {
        IntPtr handle = LoadLibraryW(path);
        if (handle == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot load native library: " + path);
        return handle;
    }
    public static IntPtr Symbol(IntPtr library, string name)
    {
        IntPtr pointer = GetProcAddress(library, name);
        if (pointer == IntPtr.Zero) throw new EntryPointNotFoundException(name);
        return pointer;
    }
    public static void Free(IntPtr library) => FreeLibrary(library);
#else
    public static IntPtr Load(string path) => NativeLibrary.Load(path);
    public static IntPtr Symbol(IntPtr library, string name) => NativeLibrary.GetExport(library, name);
    public static void Free(IntPtr library) => NativeLibrary.Free(library);
#endif
}

// One loader reference per runtime; object handles retain it after runtime disposal.
internal sealed unsafe class Module
{
    private readonly object gate = new();
    private IntPtr library;
    private int references;
    internal readonly Invoke Invoke;
    internal readonly FreeResult FreeResult;

    public Module(string path)
    {
        if (IntPtr.Size != 8 || sizeof(AbiValue) != 32 || sizeof(Request) != 120 || sizeof(Result) != 96 || sizeof(Api) != 24)
            throw new PlatformNotSupportedException("Native interop ABI requires a matching 64-bit process.");
        library = Loader.Load(Path.GetFullPath(path));
        try
        {
            var getApi = Marshal.GetDelegateForFunctionPointer<GetApi>(Loader.Symbol(library, "ni_get_api"));
            Api api = default;
            int status = getApi(1, (uint)sizeof(Api), &api);
            if (status != 0 || api.Size != sizeof(Api) || api.Version != 1 || api.Invoke == IntPtr.Zero || api.FreeResult == IntPtr.Zero)
                throw new InvalidOperationException("Native bridge ABI version or layout mismatch.");
            Invoke = Marshal.GetDelegateForFunctionPointer<Invoke>(api.Invoke);
            FreeResult = Marshal.GetDelegateForFunctionPointer<FreeResult>(api.FreeResult);
        }
        catch { Loader.Free(library); library = IntPtr.Zero; throw; }
    }

    internal void Retain()
    {
        lock (gate)
        {
            if (library == IntPtr.Zero) throw new ObjectDisposedException(nameof(NativeRuntime));
            references = checked(references + 1);
        }
    }
    internal void Release()
    {
        IntPtr close = IntPtr.Zero;
        lock (gate)
        {
            if (--references == 0) { close = library; library = IntPtr.Zero; }
        }
        if (close != IntPtr.Zero) Loader.Free(close);
    }
    internal void Abandon()
    {
        // Only used if constructing the initial managed owner failed.
        if (library != IntPtr.Zero) { Loader.Free(library); library = IntPtr.Zero; }
    }
    internal void ReleaseObject(ulong token)
    {
        Request request = new() { Size = (uint)sizeof(Request), Version = 1, Operation = Operation.Release, Target = token };
        Result result = default;
        try { Invoke(&request, &result); }
        finally { FreeResult(&result); }
    }

    internal object? Perform(BridgeHandle owner, Operation operation, string? typeName, string? name, object?[] arguments)
    {
        using var ownerPin = new HandlePin(owner);
        using var scope = new RequestScope(this);
        Request request = new()
        {
            Size = (uint)sizeof(Request), Version = 1, Operation = operation, Target = owner.Token,
            TypeName = scope.Encode(typeName), Name = scope.Encode(name), Arguments = scope.Encode(arguments)
        };
        Result result = default;
        var adopted = new List<NativeObject>();
        try
        {
            int status = Invoke(&request, &result);
            if (status != 0)
                throw new NativeException(result.ErrorType.Kind == Kind.Utf8 ? Values.Text(result.ErrorType) : "native_error",
                    result.ErrorMessage.Kind == Kind.Utf8 ? Values.Text(result.ErrorMessage) : $"Native call failed with status {status}.");
            return Values.Decode(ref result.Value, this, adopted);
        }
        catch
        {
            foreach (NativeObject item in adopted) item.Dispose();
            throw;
        }
        finally { FreeResult(&result); }
    }
}

internal sealed class BridgeHandle : SafeHandle
{
    internal readonly Module Module;
    internal readonly ulong Token;
    internal BridgeHandle(Module module, ulong token = 0) : base(IntPtr.Zero, true)
    {
        Module = module;
        Token = token;
        module.Retain();
        SetHandle(new IntPtr(1)); // Opaque native token is stored separately, never dereferenced.
    }
    public override bool IsInvalid => handle == IntPtr.Zero;
    protected override bool ReleaseHandle()
    {
        try { if (Token != 0) Module.ReleaseObject(Token); }
        catch { /* SafeHandle finalization must not throw. */ }
        finally { Module.Release(); }
        return true;
    }
}

internal sealed class HandlePin : IDisposable
{
    private readonly SafeHandle handle;
    private bool retained;
    internal HandlePin(SafeHandle handle)
    {
        this.handle = handle;
        handle.DangerousAddRef(ref retained);
    }
    public void Dispose()
    {
        if (retained) { retained = false; handle.DangerousRelease(); }
    }
}
