#include <native_interop/interop.hpp>
#include <atomic>
#include <limits>
#include <mutex>

using namespace native_interop;
namespace {
std::atomic<std::int32_t> destroyed{0};
void count(const value::array& args, std::size_t expected) {
    if (args.size() != expected) throw std::invalid_argument("Wrong argument count");
}
class counter final : public object, public std::enable_shared_from_this<counter> {
    std::mutex gate_;
    std::int32_t value_;
public:
    explicit counter(std::int32_t initial) : value_(initial) {}
    ~counter() override { ++destroyed; }
    value call(std::string_view name, const value::array& args) override {
        if (name == "Add") {
            count(args, 1);
            auto delta = args[0].as<std::int32_t>();
            std::lock_guard lock(gate_);
            auto sum = static_cast<std::int64_t>(value_) + delta;
            if (sum > std::numeric_limits<std::int32_t>::max() || sum < std::numeric_limits<std::int32_t>::min())
                throw std::overflow_error("Counter overflow");
            value_ = static_cast<std::int32_t>(sum);
            return value_;
        }
        if (name == "Self") { count(args, 0); return std::static_pointer_cast<object>(shared_from_this()); }
        return object::call(name, args);
    }
    value get(std::string_view name) override {
        if (name == "Value") { std::lock_guard lock(gate_); return value_; }
        return object::get(name);
    }
    void set(std::string_view name, const value& input) override {
        if (name == "Value") { std::lock_guard lock(gate_); value_ = input.as<std::int32_t>(); return; }
        object::set(name, input);
    }
};
class static_members final : public object {
public:
    value call(std::string_view name, const value::array& args) override {
        if (name == "Echo") { count(args, 1); return args[0]; }
        if (name == "Fail") throw std::runtime_error("native failure");
        return object::call(name, args);
    }
    value get(std::string_view name) override {
        if (name == "Destroyed") return destroyed.load();
        return object::get(name);
    }
};
}

extern "C" NI_EXPORT int32_t NI_CALL ni_get_api(uint32_t version, uint32_t size, ni_api* output) {
    try {
        static std::once_flag once;
        std::call_once(once, [] {
            register_type("Demo.Counter", [](const value::array& args) {
                if (args.size() > 1) throw std::invalid_argument("Counter expects zero or one Int32 argument");
                return std::make_shared<counter>(args.empty() ? 0 : args[0].as<std::int32_t>());
            }, std::make_shared<static_members>());
        });
        return get_api(version, size, output);
    } catch (...) { return 1; } // Registration errors must also stay inside the C ABI.
}
