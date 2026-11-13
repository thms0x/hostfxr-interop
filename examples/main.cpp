#include <interop/interop.hpp>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: interop_example /path/to/Interop.Managed.dll\n";
        return 2;
    }
    try {
        interop::runtime runtime(argv[1]);
        auto builder_type = runtime.resolve_type("System.Text.StringBuilder, System.Private.CoreLib");
        auto builder = builder_type.create({"Hello"});
        builder.call("Append", {" from C++ through hostfxr!"});
        std::cout << builder.call("ToString").as<std::string>() << '\n';

        auto stream_type = runtime.resolve_type("System.IO.MemoryStream, System.Private.CoreLib");
        auto stream = stream_type.create();
        stream.call("WriteByte", {std::uint8_t{42}});
        std::cout << "Stream length: " << stream.get("Length").as<std::int64_t>() << '\n';
        // IDisposable.Dispose runs when the last C++ lease goes out of scope.
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
