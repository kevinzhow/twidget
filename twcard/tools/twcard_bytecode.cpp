// twcard_bytecode: build-time tool. Compiles the runtime bundle to QuickJS bytecode in the form twcard::Resources
// expects (SPEC S0 "Version": `twcard-bytecode <the bundle's first line>\n` + JS_WriteObject's output), so a blob
// made from another bundle version is never used.
//   twcard_bytecode twcard-runtime.js twcard-runtime.qbc
#include <quickjs.h>

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
    JSRuntime* rt = JS_NewRuntime();
    JSContext* ctx = JS_NewContext(rt);
    JSValue fn = JS_Eval(ctx, bundle.data(), bundle.size(), "twcard-runtime.js", JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    int rc = 0;
    if (JS_IsException(fn)) {
        JSValue exc = JS_GetException(ctx);
        const char* m = JS_ToCString(ctx, exc);
        std::fprintf(stderr, "twcard_bytecode: %s\n", m ? m : "compile error");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, exc);
        rc = 1;
    } else {
        size_t size = 0;
        uint8_t* buf = JS_WriteObject(ctx, &size, fn, JS_WRITE_OBJ_BYTECODE);
        JS_FreeValue(ctx, fn);
        std::ofstream out(argv[2], std::ios::binary);
        const std::string header = "twcard-bytecode " + first + "\n";
        out.write(header.data(), std::streamsize(header.size()));
        out.write(reinterpret_cast<const char*>(buf), std::streamsize(size));
        js_free(ctx, buf);
        if (!out) rc = 1;
    }
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return rc;
}
