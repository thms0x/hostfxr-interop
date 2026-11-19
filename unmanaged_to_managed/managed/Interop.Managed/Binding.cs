using System.Reflection;

namespace Interop.Managed;

// Selects public .NET members by reflection and invokes the best match.
internal static class Binding
{
    // Choose public static or instance members, including inherited members.
    public static BindingFlags Flags(bool isStatic) => BindingFlags.Public | BindingFlags.FlattenHierarchy |
        (isStatic ? BindingFlags.Static : BindingFlags.Instance);

    // Match and invoke a constructor or method using decoded arguments.
    public static object? Invoke(Type type, object? target, string name, object?[] arguments,
        string[]? signature, string[] genericTypes, bool construct)
    {
        if (type.ContainsGenericParameters) throw new NotSupportedException("Close generic types before invoking them.");
        Type[]? parameterTypes = signature?.Select(n => Components.Resolve(n, type.Assembly)).ToArray();
        Type[] generics = genericTypes.Select(n => Components.Resolve(n, type.Assembly)).ToArray();
        IEnumerable<MethodBase> methods = construct ? type.GetConstructors(Flags(false)) :
            type.GetMethods(Flags(target is null)).Where(m => m.Name == name);
        var candidates = new List<(MethodBase Method, object?[] Arguments, int Score)>();
        foreach (MethodBase candidate in methods)
        {
            MethodBase method = candidate;
            if (method is MethodInfo info)
            {
                if (generics.Length != 0)
                {
                    if (!info.IsGenericMethodDefinition || info.GetGenericArguments().Length != generics.Length) continue;
                    try { method = info.MakeGenericMethod(generics); }
                    catch (ArgumentException) { continue; }
                }
                else if (info.ContainsGenericParameters) continue;
                if (((MethodInfo)method).ReturnType.IsByRef || ((MethodInfo)method).ReturnType.IsPointer) continue;
            }
            else if (generics.Length != 0) continue;
            ParameterInfo[] parameters = method.GetParameters();
            if (parameters.Length != arguments.Length || parameters.Any(p => p.ParameterType.IsByRef || p.ParameterType.IsPointer)) continue;
            if (parameterTypes is not null && !parameters.Select(p => p.ParameterType).SequenceEqual(parameterTypes)) continue;
            var converted = new object?[arguments.Length];
            int score = 0;
            bool matches = true;
            for (int i = 0; i < arguments.Length; i++)
            {
                if (!ConvertArgument(arguments[i], parameters[i].ParameterType, out converted[i], out int cost))
                { matches = false; break; }
                score += cost;
            }
            if (matches) candidates.Add((method, converted, score));
        }
        if (candidates.Count == 0) throw new MissingMethodException(
            $"No public matching {(construct ? "constructor" : name)} on {type}. Numeric widths must match; supply parameter_types to select an overload.");
        // Prefer the cheapest conversions, then the most specific parameter types.
        int best = candidates.Min(c => c.Score);
        var finalists = candidates.Where(c => c.Score == best).ToArray();
        var winners = finalists.Where(c => finalists.All(other => c.Method == other.Method || MoreSpecific(c.Method, other.Method))).ToArray();
        if (winners.Length != 1) throw new AmbiguousMatchException($"Ambiguous overload {type}.{name}; supply exact parameter_types.");
        var winner = winners[0];
        return winner.Method is ConstructorInfo constructor ? constructor.Invoke(winner.Arguments) :
            ((MethodInfo)winner.Method).Invoke(target, winner.Arguments);
    }

    // Check whether a's parameter types are strictly narrower than b's.
    private static bool MoreSpecific(MethodBase a, MethodBase b)
    {
        Type[] left = a.GetParameters().Select(p => p.ParameterType).ToArray();
        Type[] right = b.GetParameters().Select(p => p.ParameterType).ToArray();
        return left.Zip(right, (l, r) => r.IsAssignableFrom(l)).All(match => match) &&
            left.Zip(right, (l, r) => l != r).Any(different => different);
    }

    // Check assignability and materialize arrays for a candidate parameter type.
    private static bool ConvertArgument(object? value, Type target, out object? converted, out int cost)
    {
        converted = value;
        cost = 0;
        Type? nullable = Nullable.GetUnderlyingType(target);
        if (value is null) return !target.IsValueType || nullable is not null;
        if (nullable is not null) target = nullable;
        // Arrays arrive untyped; build the array required by this overload.
        if (value is ArrayArgument array)
        {
            if (target == typeof(object)) { converted = Materialize(array); cost = 10; return true; }
            if (!target.IsArray || target.GetArrayRank() != 1) return false;
            Type element = target.GetElementType()!;
            Array result = Array.CreateInstance(element, array.Items.Length);
            for (int i = 0; i < array.Items.Length; i++)
            {
                if (!ConvertArgument(array.Items[i], element, out object? item, out int itemCost)) return false;
                result.SetValue(item, i);
                cost += itemCost;
            }
            converted = result;
            return true;
        }
        if (!target.IsInstanceOfType(value)) return false;
        cost = value.GetType() == target ? 0 : 1;
        return true;
    }

    // Turn nested ABI array placeholders into object arrays.
    private static object?[] Materialize(ArrayArgument array) => array.Items.Select(v =>
        v is ArrayArgument nested ? Materialize(nested) : v).ToArray();

    // Read or write a public property or field on a type or instance.
    public static object? Member(Type type, object? target, string name, bool set, object?[] arguments)
    {
        if (arguments.Length != (set ? 1 : 0)) throw new ArgumentException("Invalid member argument count.");
        BindingFlags flags = Flags(target is null);
        PropertyInfo? property = type.GetProperty(name, flags);
        FieldInfo? field = property is null ? type.GetField(name, flags) : null;
        if (property is not null)
        {
            MethodInfo? accessor = set ? property.SetMethod : property.GetMethod;
            if (accessor is null || !accessor.IsPublic || property.GetIndexParameters().Length != 0)
                throw new MemberAccessException($"No public {(set ? "setter" : "getter")} for {type}.{name} (indexers require method calls).");
            if (!set) return property.GetValue(target);
            if (!ConvertArgument(arguments[0], property.PropertyType, out object? converted, out _))
                throw new ArgumentException($"Value does not match {property.PropertyType}.");
            property.SetValue(target, converted);
            return null;
        }
        if (field is null) throw new MissingMemberException(type.FullName, name);
        if (!set) return field.GetValue(target);
        if (field.IsInitOnly || field.IsLiteral) throw new FieldAccessException($"Field {name} is read-only.");
        if (!ConvertArgument(arguments[0], field.FieldType, out object? fieldValue, out _))
            throw new ArgumentException($"Value does not match {field.FieldType}.");
        field.SetValue(target, fieldValue);
        return null;
    }
}
