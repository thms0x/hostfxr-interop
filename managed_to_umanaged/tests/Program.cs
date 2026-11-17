using System;
using System.Runtime.CompilerServices;
using System.Threading.Tasks;
using Interop.Native;

static class Program
{
    static void Check(bool value, string message) { if (!value) throw new Exception(message); }
    static void Throws<T>(Action action) where T : Exception
    {
        try { action(); } catch (T) { return; }
        throw new Exception("Expected " + typeof(T).Name);
    }
    [MethodImpl(MethodImplOptions.NoInlining)]
    static void Abandon(NativeType type) { type.Create(); }
    static int Main(string[] args)
    {
        try
        {
            using var runtime = new NativeRuntime(args[0]);
            var type = runtime.LoadType("Demo.Counter");
            Check(runtime.LiveHandles == 0, "Initial handles");
            Throws<NativeException>(() => runtime.LoadType("Missing"));
            Throws<NativeException>(() => type.Create("wrong constructor type"));
            Throws<NativeException>(() => type.Create(1, 2));
            object[] scalars = { true, false, sbyte.MinValue, byte.MaxValue, short.MinValue, ushort.MaxValue,
                '\u03bb', int.MinValue, uint.MaxValue, long.MinValue, ulong.MaxValue, -1.25f, -2.5,
                float.NaN, double.PositiveInfinity, "", "Gr\u00fc\u00dfe \u03bb \ud83d\ude42\0tail" };
            foreach (object input in scalars)
            {
                object? output = type.Call("Echo", input);
                Check(output?.GetType() == input.GetType() && input.Equals(output), "Scalar roundtrip: " + input.GetType());
            }
            Check(BitConverter.ToInt32(BitConverter.GetBytes(type.Call<float>("Echo", -0.0f)), 0) == int.MinValue, "Negative zero");
            Check(type.Call("Echo", new object?[] { null }) is null, "Null roundtrip");
            var array = type.Call<object?[]>("Echo", (object)new object?[] { 1, new int[] { 2, 3 }, null });
            Check((int)array[0]! == 1 && (int)((object?[])array[1]!)[1]! == 3 && array[2] is null, "Nested arrays");
            Check(type.Call<object?[]>("Echo", (object)Array.Empty<int>()).Length == 0, "Empty array");
            Throws<ArgumentException>(() => type.Call("Echo", DateTime.Now));
            object?[] cycle = new object?[1]; cycle[0] = cycle;
            Throws<ArgumentException>(() => type.Call("Echo", (object)cycle));
            Throws<NativeException>(() => type.Call("Missing"));
            try { type.Call("Fail"); throw new Exception("Expected native exception"); }
            catch (NativeException error) { Check(error.Message.Contains("native failure") && error.NativeType == "std::exception", "Native diagnostics"); }

            int destroyed = type.Get<int>("Destroyed");
            var obj = type.Create(10);
            Check(obj.Call<int>("Add", 5) == 15, "Instance call");
            obj.Set("Value", 42);
            Check(obj.Get<int>("Value") == 42, "Property access");
            Throws<NativeException>(() => obj.Call("Add", 1L));
            Throws<NativeException>(() => obj.Get("Missing"));
            Throws<NativeException>(() => obj.Set("Missing", 0));
            Throws<NativeException>(() => obj.Call("Add", int.MaxValue));
            Check(obj.Get<int>("Value") == 42, "Overflow preserves state");
            var alias = obj.Call<NativeObject>("Self");
            Check(runtime.LiveHandles == 2, "Separate aliases have separate leases");
            Throws<InvalidCastException>(() => obj.Call<int>("Self"));
            Check(runtime.LiveHandles == 2, "Failed typed cast releases returned object");
            Throws<InvalidCastException>(() => type.Call<int>("Echo", (object)new object[] { obj }));
            Check(runtime.LiveHandles == 2, "Failed typed array cast releases nested objects");
            var returned = type.Call<object?[]>("Echo", (object)new object[] { obj, obj });
            Check(runtime.LiveHandles == 4, "Object array exports");
            foreach (object? item in returned) ((NativeObject)item!).Dispose();
            using (var other = new NativeRuntime(args[0]))
                Throws<ArgumentException>(() => other.LoadType("Demo.Counter").Call("Echo", obj));
            GC.Collect(); GC.WaitForPendingFinalizers();
            obj.Dispose(); obj.Dispose();
            Check(type.Get<int>("Destroyed") == destroyed, "Alias keeps native object alive");
            Throws<ObjectDisposedException>(() => obj.Call("Add", 1));
            Throws<ObjectDisposedException>(() => type.Call("Echo", obj));
            alias.Set("Value", 0);
            Parallel.For(0, 100, _ => alias.Call<int>("Add", 1));
            Check(alias.Get<int>("Value") == 100, "Concurrent calls");
            alias.Dispose();
            Check(type.Get<int>("Destroyed") == destroyed + 1, "Final lease destroys exactly once");
            Check(runtime.LiveHandles == 0, "All explicit handles released");
            Abandon(type);
            GC.Collect(); GC.WaitForPendingFinalizers(); GC.Collect();
            Check(runtime.LiveHandles == 0, "SafeHandle finalizer releases abandoned object");

            var survivor = type.Create(7);
            runtime.Dispose(); runtime.Dispose();
            Throws<ObjectDisposedException>(() => type.Create());
            Check(survivor.Call<int>("Add", 1) == 8, "Object survives runtime disposal");
            var survivingAlias = survivor.Call<NativeObject>("Self");
            survivor.Dispose();
            Check(survivingAlias.Get<int>("Value") == 8, "New alias retains module after runtime disposal");
            survivingAlias.Dispose();
            using var reloaded = new NativeRuntime(args[0]);
            Check(reloaded.LiveHandles == 0, "Library reload");
            Console.WriteLine("Managed-to-native integration checks passed");
            return 0;
        }
        catch (Exception error) { Console.Error.WriteLine(error); return 1; }
    }
}
