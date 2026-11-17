#include <interop/interop.hpp>
#include <cmath>
#include <iostream>
#include <limits>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class T>
void roundtrip(const interop::type& probe, const char* type_name, T input) {
    interop::call_options options;
    options.generic_types = {type_name};
    check(probe.call("Echo", {interop::value(input)}, options).as<T>() == input, type_name);
}
}

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    try {
        interop::runtime runtime(argv[1]);
        interop::runtime second_runtime(argv[1]);
        check(runtime.native_api().invoke == second_runtime.native_api().invoke, "stable bridge entry points");
        const auto baseline = runtime.live_handles();
        {
            auto probe = runtime.load_type(argv[2], "Interop.Fixture.Probe");
            roundtrip(probe, "System.Boolean", true);
            roundtrip(probe, "System.SByte", std::int8_t{-128});
            roundtrip(probe, "System.Byte", std::uint8_t{255});
            roundtrip(probe, "System.Int16", std::int16_t{-32768});
            roundtrip(probe, "System.UInt16", std::uint16_t{65535});
            roundtrip(probe, "System.Int32", std::numeric_limits<std::int32_t>::min());
            roundtrip(probe, "System.UInt32", std::numeric_limits<std::uint32_t>::max());
            roundtrip(probe, "System.Int64", std::numeric_limits<std::int64_t>::min());
            roundtrip(probe, "System.UInt64", std::numeric_limits<std::uint64_t>::max());
            roundtrip(probe, "System.Char", char16_t{0x03bb});
            roundtrip(probe, "System.Single", -1.25f);
            roundtrip(probe, "System.Double", -2.5);
            roundtrip(probe, "System.Single", std::numeric_limits<float>::infinity());
            roundtrip(probe, "System.String", std::string{});
            const std::string text = std::string(u8"Gr\u00fc\u00dfe \u03bb \U0001f642") + '\0' + "tail";
            roundtrip(probe, "System.String", text);
            interop::call_options floating;
            floating.generic_types = {"System.Single"};
            check(std::signbit(probe.call("Echo", {-0.0f}, floating).as<float>()), "negative zero bits");
            check(std::isnan(probe.call("Echo", {std::numeric_limits<float>::quiet_NaN()}, floating).as<float>()), "NaN bits");
            const auto array = probe.call("EchoArray", {interop::value::array{1, -2, 3}}).as<interop::value::array>();
            check(array.size() == 3 && array[1].as<std::int32_t>() == -2, "array marshalling");
            check(probe.call("EchoArray", {interop::value::array{}}).as<interop::value::array>().empty(), "empty array");
            check(probe.call("Select", {"text"}).as<std::string>() == "string", "overload specificity");
            interop::call_options exact;
            exact.parameter_types = std::vector<std::string>{"System.Object"};
            check(probe.call("Select", {"text"}, exact).as<std::string>() == "object", "exact signature");
            bool caught = false;
            try { probe.call("Fail"); }
            catch (const interop::managed_error& e) {
                caught = e.managed_type() == "System.InvalidOperationException" &&
                    std::string(e.what()).find("managed failure") != std::string::npos;
            }
            check(caught, "managed exception propagation");
            auto object = probe.create(std::vector<interop::value>{});
            object.set("Text", text);
            check(object.get("Text").as<std::string>() == text, "property marshalling");
            auto alias = object.call("Self", std::vector<interop::value>{}).as<interop::object>();
            probe.call("Collect", std::vector<interop::value>{});
            object.reset();
            check(probe.get("Disposals").as<std::int32_t>() == 0, "release does not dispose");
            check(alias.get("Text").as<std::string>() == text, "callback and lease survive GC");
            alias.dispose();
            check(probe.get("Disposals").as<std::int32_t>() == 1, "explicit disposal");
            alias.reset();
            check(probe.get("Disposals").as<std::int32_t>() == 1, "release does not dispose again");
            auto undisposed = probe.create();
            undisposed.reset();
            check(probe.get("Disposals").as<std::int32_t>() == 1, "final release only frees GCHandle");
        }
        check(runtime.live_handles() == baseline, "all handles released");
        auto api = runtime.native_api();
        interop_request old_version{};
        old_version.size = sizeof(old_version);
        old_version.version = INTEROP_ABI_VERSION - 1;
        old_version.operation = INTEROP_LIVE_HANDLES;
        interop_result old_result{};
        check(api.invoke(&old_version, &old_result) != 0, "previous ABI version rejected");
        api.free_result(&old_result);
        interop_request invalid{};
        interop_result result{};
        check(api.invoke(&invalid, &result) != 0, "invalid ABI request rejected");
        api.free_result(&result);
        api.free_result(&result);
        std::cout << "Interop integration checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
