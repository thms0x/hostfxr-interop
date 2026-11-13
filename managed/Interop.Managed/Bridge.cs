using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Interop.Managed;

internal static unsafe class Bridge
{
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    public static int Invoke(Request* request, Result* result)
    {
        if (result == null) return 2;
        *result = default;
        try
        {
            if (request == null || request->Size != sizeof(Request) || request->Version != 1 || request->Reserved != 0)
                throw new ArgumentException("Invalid ABI request size, version, or reserved field.");
            object? value = Dispatch(*request);
            Marshalling.Write(ref result->Value, value, request->Operation == Operation.Construct);
            return 0;
        }
        catch (Exception exception)
        {
            // Remove partially written arrays and any exported object leases.
            Marshalling.Free(ref result->Value);
            while (exception is TargetInvocationException { InnerException: not null } wrapped)
                exception = wrapped.InnerException!;
            try
            {
                Marshalling.Write(ref result->ErrorType, exception.GetType().FullName);
                Marshalling.Write(ref result->ErrorMessage, exception.Message);
                Marshalling.Write(ref result->ErrorStack, exception.ToString());
                return 1;
            }
            catch { return 2; } // Allocation failure; caller still frees partial diagnostics.
        }
    }

    private static object? Dispatch(in Request request)
    {
        switch (request.Operation)
        {
            case Operation.Release: Handles.Release(request.Target); return null;
            case Operation.Dispose: Handles.Dispose(request.Target); return null;
            case Operation.LiveHandles: return Handles.Count;
            case Operation.TakeOwnership: Handles.TakeOwnership(request.Target); return null;
            case Operation.LoadType:
                return Components.LoadType(request.Assembly.Kind == Kind.Null ? null : Marshalling.Text(request.Assembly),
                    Marshalling.Text(request.Name));
        }
        object target = Handles.Get(request.Target);
        bool isStatic = request.Operation is Operation.Construct or Operation.CallStatic or Operation.GetStatic or Operation.SetStatic;
        Type type = isStatic ? target as Type ?? throw new ArgumentException("Target is not a type.") : target.GetType();
        object? instance = isStatic ? null : target;
        object?[] arguments = Marshalling.ReadArray(request.Arguments);
        string name = request.Name.Kind == Kind.Null ? ".ctor" : Marshalling.Text(request.Name);
        return request.Operation switch
        {
            Operation.Construct or Operation.CallStatic or Operation.Call => Binding.Invoke(type, instance, name, arguments,
                request.ParameterTypes.Kind == Kind.Null ? null : Marshalling.Strings(request.ParameterTypes),
                Marshalling.Strings(request.GenericTypes), request.Operation == Operation.Construct),
            Operation.GetStatic or Operation.Get => Binding.Member(type, instance, name, false, arguments),
            Operation.SetStatic or Operation.Set => Binding.Member(type, instance, name, true, arguments),
            _ => throw new ArgumentException($"Unknown operation {request.Operation}.")
        };
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    public static void FreeResult(Result* result)
    {
        if (result == null) return;
        Marshalling.Free(ref result->Value);
        Marshalling.Free(ref result->ErrorType);
        Marshalling.Free(ref result->ErrorMessage);
        Marshalling.Free(ref result->ErrorStack);
    }
}
