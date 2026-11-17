#include <native_interop/interop.hpp>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>

namespace native_interop {
namespace {
struct type_entry { factory construct; std::shared_ptr<object> statics; };
struct registry {
    std::mutex gate;
    std::unordered_map<std::string, type_entry> types;
    std::unordered_map<std::uint64_t, std::shared_ptr<object>> leases;
    std::uint64_t next = 0;
};
registry& state() { static registry instance; return instance; }

template<class To, class From> To bits(From from) {
    static_assert(sizeof(To) == sizeof(From));
    To to;
    std::memcpy(&to, &from, sizeof(to));
    return to;
}
void validate(const ni_value& v) {
    if (v.reserved != 0 || v.length < 0 || v.length > std::numeric_limits<std::int32_t>::max() ||
        (v.length && !v.data)) throw std::invalid_argument("Invalid ABI value buffer or reserved field");
}
std::string text(const ni_value& v) {
    validate(v);
    if (v.kind != NI_UTF8) throw std::invalid_argument("Expected UTF-8 string");
    return v.length ? std::string(static_cast<const char*>(v.data), static_cast<std::size_t>(v.length)) : std::string{};
}
std::shared_ptr<object> lookup(std::uint64_t token) {
    auto& s = state();
    std::lock_guard lock(s.gate);
    auto found = s.leases.find(token);
    if (found == s.leases.end()) throw std::invalid_argument("Unknown or released native object lease");
    return found->second;
}
std::uint64_t export_object(const std::shared_ptr<object>& ptr) {
    auto& s = state();
    std::lock_guard lock(s.gate);
    if (s.next == static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        throw std::overflow_error("Native lease space exhausted");
    auto token = ++s.next;
    s.leases.emplace(token, ptr);
    return token;
}
void release(std::uint64_t token) {
    std::shared_ptr<object> ptr;
    auto& s = state();
    {
        std::lock_guard lock(s.gate);
        auto found = s.leases.find(token);
        if (found == s.leases.end()) throw std::invalid_argument("Unknown or released native object lease");
        ptr = std::move(found->second);
        s.leases.erase(found);
    } // Run the destructor outside the registry lock.
}
type_entry find_type(const std::string& name) {
    auto& s = state();
    std::lock_guard lock(s.gate);
    auto found = s.types.find(name);
    if (found == s.types.end()) throw std::invalid_argument("Unknown native type: " + name);
    return found->second;
}
value read(const ni_value& v, int depth = 0) {
    if (depth > 32) throw std::invalid_argument("Value nesting exceeds 32 levels");
    validate(v);
    switch (v.kind) {
    case NI_NULL: return {};
    case NI_BOOL:
        if (v.scalar > 1) throw std::invalid_argument("Invalid bool");
        return value(v.scalar != 0);
    case NI_I8: return bits<std::int8_t>(static_cast<std::uint8_t>(v.scalar));
    case NI_U8: return static_cast<std::uint8_t>(v.scalar);
    case NI_I16: return bits<std::int16_t>(static_cast<std::uint16_t>(v.scalar));
    case NI_U16: return static_cast<std::uint16_t>(v.scalar);
    case NI_CHAR16: return static_cast<char16_t>(v.scalar);
    case NI_I32: return bits<std::int32_t>(static_cast<std::uint32_t>(v.scalar));
    case NI_U32: return static_cast<std::uint32_t>(v.scalar);
    case NI_I64: return bits<std::int64_t>(v.scalar);
    case NI_U64: return v.scalar;
    case NI_F32: return bits<float>(static_cast<std::uint32_t>(v.scalar));
    case NI_F64: return bits<double>(v.scalar);
    case NI_UTF8: return text(v);
    case NI_OBJECT: return lookup(v.scalar);
    case NI_ARRAY: {
        value::array items;
        const auto* data = static_cast<const ni_value*>(v.data);
        items.reserve(static_cast<std::size_t>(v.length));
        for (std::int64_t i = 0; i < v.length; ++i) items.push_back(read(data[i], depth + 1));
        return items;
    }
    default: throw std::invalid_argument("Unknown ABI value kind");
    }
}
void write(ni_value& v, const value& input, int depth = 0) {
    if (depth > 32) throw std::invalid_argument("Value nesting exceeds 32 levels");
    v = {};
    v.kind = static_cast<std::int32_t>(input.data().index());
    std::visit([&](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::monostate>) {}
        else if constexpr (std::is_integral_v<T>) v.scalar = static_cast<std::uint64_t>(item);
        else if constexpr (std::is_same_v<T, float>) v.scalar = bits<std::uint32_t>(item);
        else if constexpr (std::is_same_v<T, double>) v.scalar = bits<std::uint64_t>(item);
        else if constexpr (std::is_same_v<T, std::string>) {
            if (item.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
                throw std::length_error("String exceeds ABI limit");
            auto* bytes = new char[item.size()];
            v.data = bytes;
            v.length = static_cast<std::int64_t>(item.size());
            if (!item.empty()) std::memcpy(bytes, item.data(), item.size());
        } else if constexpr (std::is_same_v<T, value::object_ptr>) {
            if (item) v.scalar = export_object(item);
            else v.kind = NI_NULL;
        } else if constexpr (std::is_same_v<T, value::array>) {
            if (item.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
                throw std::length_error("Array exceeds ABI limit");
            auto* elements = new ni_value[item.size()]{};
            v.data = elements;
            v.length = static_cast<std::int64_t>(item.size());
            for (std::size_t i = 0; i < item.size(); ++i) write(elements[i], item[i], depth + 1);
        }
    }, input.data());
}
void free_value(ni_value& v) noexcept {
    if (v.kind == NI_ARRAY) {
        auto* elements = const_cast<ni_value*>(static_cast<const ni_value*>(v.data));
        for (std::int64_t i = 0; i < v.length; ++i) free_value(elements[i]);
        delete[] elements;
    } else if (v.kind == NI_UTF8) delete[] static_cast<const char*>(v.data);
    else if (v.kind == NI_OBJECT && v.scalar) {
        try { release(v.scalar); } catch (...) {}
    }
    v = {};
}
void NI_CALL free_result(ni_result* result) noexcept {
    if (!result) return;
    free_value(result->value);
    free_value(result->error_type);
    free_value(result->error_message);
}
value dispatch(const ni_request& r) {
    if (r.operation == NI_RELEASE) { release(r.target); return {}; }
    if (r.operation == NI_LIVE_HANDLES) {
        auto& s = state();
        std::lock_guard lock(s.gate);
        return static_cast<std::int64_t>(s.leases.size());
    }
    if (r.operation == NI_LOAD_TYPE) { find_type(text(r.type_name)); return {}; }
    if (r.arguments.kind != NI_ARRAY) throw std::invalid_argument("Expected argument array");
    auto args = read(r.arguments).as<value::array>();
    if (r.operation == NI_CONSTRUCT) {
        auto type = find_type(text(r.type_name));
        if (!type.construct) throw std::invalid_argument("Type has no public factory");
        auto result = type.construct(args);
        if (!result) throw std::runtime_error("Factory returned a null object");
        return result;
    }
    bool is_static = r.operation == NI_CALL_STATIC || r.operation == NI_GET_STATIC || r.operation == NI_SET_STATIC;
    auto target = is_static ? find_type(text(r.type_name)).statics : lookup(r.target);
    if (!target) throw std::invalid_argument("Type has no static members");
    auto name = text(r.name);
    switch (r.operation) {
    case NI_CALL_STATIC: case NI_CALL_METHOD: return target->call(name, args);
    case NI_GET_STATIC: case NI_GET:
        if (!args.empty()) throw std::invalid_argument("Get expects no arguments");
        return target->get(name);
    case NI_SET_STATIC: case NI_SET:
        if (args.size() != 1) throw std::invalid_argument("Set expects one argument");
        target->set(name, args[0]); return {};
    default: throw std::invalid_argument("Unknown ABI operation");
    }
}
int report(ni_result* result, const char* type, const char* message) noexcept {
    free_result(result);
    try {
        write(result->error_type, value(type));
        write(result->error_message, value(message));
        return 1;
    } catch (...) { return 2; }
}
int32_t NI_CALL invoke(const ni_request* request, ni_result* result) noexcept {
    if (!result) return 2;
    *result = {};
    try {
        if (!request || request->size != sizeof(ni_request) || request->version != NI_ABI_VERSION || request->reserved)
            throw std::invalid_argument("Invalid ABI request size, version, or reserved field");
        write(result->value, dispatch(*request));
        return 0;
    } catch (const std::invalid_argument& e) { return report(result, "std::invalid_argument", e.what()); }
      catch (const std::bad_variant_access& e) { return report(result, "std::bad_variant_access", e.what()); }
      catch (const std::exception& e) { return report(result, "std::exception", e.what()); }
      catch (...) { return report(result, "native_exception", "Unknown C++ exception"); }
}
} // namespace

void register_type(std::string name, factory construct, std::shared_ptr<object> statics) {
    if (name.empty()) throw std::invalid_argument("Type name cannot be empty");
    auto& s = state();
    std::lock_guard lock(s.gate);
    if (!s.types.emplace(std::move(name), type_entry{std::move(construct), std::move(statics)}).second)
        throw std::invalid_argument("Native type already registered");
}
int get_api(std::uint32_t version, std::uint32_t size, ni_api* output) noexcept {
    if (!output || version != NI_ABI_VERSION || size != sizeof(ni_api)) return 1;
    *output = {sizeof(ni_api), NI_ABI_VERSION, &invoke, &free_result};
    return 0;
}
} // namespace native_interop
