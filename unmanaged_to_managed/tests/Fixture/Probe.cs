using System;

namespace Interop.Fixture;

public sealed class Probe : IDisposable
{
    public static int Disposals { get; private set; }
    public string Text { get; set; } = string.Empty;
    public Probe Self() => this;
    public void Dispose() => Disposals++;
    public static T Echo<T>(T value) => value;
    public static int[] EchoArray(int[] values) => values;
    public static string Select(object value) => "object";
    public static string Select(string value) => "string";
    public static void Fail() => throw new InvalidOperationException("managed failure");
    public static void Collect()
    {
        GC.Collect();
        GC.WaitForPendingFinalizers();
        GC.Collect();
    }
}
