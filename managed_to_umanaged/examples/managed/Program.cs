using System;
using Interop.Native;

if (args.Length != 1)
{
    Console.Error.WriteLine("Usage: Interop.Native.Example /path/to/native_sample.dll (or .so / .dylib)");
    return 2;
}
try
{
    using var runtime = new NativeRuntime(args[0]);
    var nativeType = runtime.LoadType("Demo.Counter");
    using var nativeObject = nativeType.Create(10);
    Console.WriteLine("After Add: " + nativeObject.Call<int>("Add", 5));
    nativeObject.Set("Value", 42);
    Console.WriteLine("Value: " + nativeObject.Get<int>("Value"));
    Console.WriteLine(nativeType.Call<string>("Echo", "Hello from C# through C++17!"));
    return 0;
}
catch (Exception error)
{
    Console.Error.WriteLine(error);
    return 1;
}
