namespace Interop.Native;

public sealed class NativeException : Exception
{
    public string NativeType { get; }
    internal NativeException(string type, string message) : base(type + ": " + message) { NativeType = type; }
}

/// <summary>Loads one native plugin exporting ni_get_api. Dispose closes this owner;
/// existing NativeObjects keep the library loaded until their own disposal.</summary>
public sealed class NativeRuntime : IDisposable
{
    private readonly BridgeHandle owner;
    public NativeRuntime(string libraryPath)
    {
        var module = new Module(libraryPath);
        try { owner = new BridgeHandle(module); }
        catch { module.Abandon(); throw; }
    }
    public NativeType LoadType(string name)
    {
        if (string.IsNullOrEmpty(name)) throw new ArgumentException("A registered type name is required.", nameof(name));
        Perform(Operation.LoadType, name, null, []);
        return new NativeType(this, name);
    }
    public long LiveHandles => (long)Perform(Operation.LiveHandles, null, null, [])!;
    internal object? Perform(Operation operation, string? type, string? name, object?[] args) =>
        owner.Module.Perform(owner, operation, type, name, args);
    public void Dispose() => owner.Dispose();
}

/// <summary>A registered native type; operations require its NativeRuntime to be open.</summary>
public sealed class NativeType
{
    private readonly NativeRuntime runtime;
    public string Name { get; }
    internal NativeType(NativeRuntime runtime, string name) { this.runtime = runtime; Name = name; }
    public NativeObject Create(params object?[] arguments) =>
        Values.Cast<NativeObject>(runtime.Perform(Operation.Construct, Name, null, arguments));
    public object? Call(string name, params object?[] arguments) => runtime.Perform(Operation.CallStatic, Name, name, arguments);
    public T Call<T>(string name, params object?[] arguments) => Values.Cast<T>(Call(name, arguments));
    public object? Get(string name) => runtime.Perform(Operation.GetStatic, Name, name, []);
    public T Get<T>(string name) => Values.Cast<T>(Get(name));
    public void Set(string name, object? value) => runtime.Perform(Operation.SetStatic, Name, name, [value]);
}

/// <summary>Owns one native object lease. Dispose is idempotent; SafeHandle provides
/// finalizer cleanup. Managed references to this wrapper share its disposal state.</summary>
public sealed class NativeObject : IDisposable
{
    internal BridgeHandle Handle { get; }
    internal NativeObject(Module module, ulong token) { Handle = new BridgeHandle(module, token); }
    public object? Call(string name, params object?[] arguments) => Handle.Module.Perform(Handle, Operation.Call, null, name, arguments);
    public T Call<T>(string name, params object?[] arguments) => Values.Cast<T>(Call(name, arguments));
    public object? Get(string name) => Handle.Module.Perform(Handle, Operation.Get, null, name, []);
    public T Get<T>(string name) => Values.Cast<T>(Get(name));
    public void Set(string name, object? value) => Handle.Module.Perform(Handle, Operation.Set, null, name, [value]);
    public void Dispose() => Handle.Dispose();
}
