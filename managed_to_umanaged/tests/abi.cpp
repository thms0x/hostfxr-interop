#include <native_interop/interop.hpp>
#include <iostream>

using namespace native_interop;
namespace {
int destroyed = 0;
struct test_object : object { ~test_object() override { ++destroyed; } };
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
ni_value text(const char* data, std::int64_t length) { return {NI_UTF8, 0, 0, data, length}; }
}
int main() {
    try {
        ni_api api{};
        check(get_api(2, sizeof(api), &api) != 0, "Reject version");
        check(get_api(1, 0, &api) != 0 && get_api(1, sizeof(api), nullptr) != 0, "Reject layout/null output");
        check(get_api(1, sizeof(api), &api) == 0, "Get API");
        register_type("Test", [](const value::array&) { return std::make_shared<test_object>(); });
        ni_request request{};
        ni_result result{};
        check(api.invoke(nullptr, nullptr) != 0, "Reject null result");
        check(api.invoke(&request, &result) != 0, "Reject invalid request");
        api.free_result(&result); api.free_result(&result); api.free_result(nullptr);
        request.size = sizeof(request); request.version = 1; request.operation = NI_CONSTRUCT;
        request.type_name = text("Test", 4); request.arguments.kind = NI_ARRAY;
        check(api.invoke(&request, &result) == 0 && result.value.kind == NI_OBJECT, "Construct lease");
        const auto first = result.value.scalar;
        api.free_result(&result);
        check(destroyed == 1, "Free unadopted result destroys native object");
        check(api.invoke(&request, &result) == 0 && result.value.scalar > first, "Never reuse tokens");
        const auto second = result.value.scalar;
        result.value.scalar = 0;
        api.free_result(&result);
        check(destroyed == 1, "Adopted result remains alive");
        request.operation = NI_RELEASE; request.target = second;
        check(api.invoke(&request, &result) == 0, "Release adopted lease");
        api.free_result(&result);
        check(destroyed == 2, "Final release destroys object");
        check(api.invoke(&request, &result) != 0, "Reject stale lease");
        api.free_result(&result);
        request.operation = NI_CONSTRUCT; request.arguments.length = 1; request.arguments.data = nullptr;
        check(api.invoke(&request, &result) != 0, "Reject null array buffer");
        api.free_result(&result);
        request.arguments.length = -1;
        check(api.invoke(&request, &result) != 0, "Reject negative array length");
        api.free_result(&result);
        std::cout << "Native ABI checks passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
