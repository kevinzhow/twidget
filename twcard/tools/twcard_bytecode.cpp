// twcard_bytecode: build-time tool. Compiles the runtime bundle to QuickJS bytecode in the form twcard::Resources
// expects (SPEC S0 "Version": `twcard-bytecode <the bundle's first line>\n` + JS_WriteObject's output), so a blob
// made from another bundle version is never used.
//   twcard_bytecode twcard-runtime.js twcard-runtime.qbc
#include "bundle_bytecode.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: twcard_bytecode <bundle.js> <out.qbc>\n");
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "twcard_bytecode: cannot read %s\n", argv[1]);
        return 1;
    }
    std::ostringstream s;
    s << in.rdbuf();
    const std::string bundle = s.str();
    const std::string first = bundle.substr(0, bundle.find('\n'));
    if (first.rfind("// twcard ", 0) != 0) {
        std::fprintf(stderr, "twcard_bytecode: %s has no `// twcard <semver> protocol <n>` first line\n", argv[1]);
        return 1;
    }
    const auto compiled = twcard::compileTrustedBundle(bundle);
    if (compiled.bytes.empty()) {
        std::fprintf(stderr, "twcard_bytecode: %s\n", compiled.error.c_str());
        return 1;
    }
    std::ofstream out(argv[2], std::ios::binary);
    out << "twcard-bytecode " << first << '\n';
    out.write(reinterpret_cast<const char*>(compiled.bytes.data()), std::streamsize(compiled.bytes.size()));
    out.close();
    return out ? 0 : 1;
}
