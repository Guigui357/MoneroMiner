#include "wasm_jit.hpp"

#ifdef __EMSCRIPTEN__

#include <cstring>

namespace randomx {
namespace {

static void append_u32(std::vector<uint8_t>& out, uint32_t value) {
    do {
        uint8_t byte = static_cast<uint8_t>(value & 0x7fu);
        value >>= 7;
        if (value != 0) byte |= 0x80u;
        out.push_back(byte);
    } while (value != 0);
}

static void append_section(std::vector<uint8_t>& out, uint8_t id,
                           const std::vector<uint8_t>& payload) {
    out.push_back(id);
    append_u32(out, static_cast<uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
}

} // namespace

bool WasmJit::compile(const uint8_t* program, std::size_t size) {
    module_.clear();

    // Stage 1 is intentionally a small, valid WASM JIT probe. It emits a
    // callable exported function with the ABI that the eventual RandomX VM
    // JIT will use. Keeping module construction here makes the next stage
    // independent of Emscripten's generated C++ code.
    (void)program;
    (void)size;

    const uint8_t header[] = {
        0x00, 0x61, 0x73, 0x6d,
        0x01, 0x00, 0x00, 0x00
    };
    module_.insert(module_.end(), std::begin(header), std::end(header));

    // Type section: () -> ()
    std::vector<uint8_t> type;
    type.push_back(0x01);
    type.push_back(0x60);
    type.push_back(0x00);
    type.push_back(0x00);
    append_section(module_, 0x01, type);

    // Function section: function 0 uses type 0.
    std::vector<uint8_t> function;
    function.push_back(0x01);
    function.push_back(0x00);
    append_section(module_, 0x03, function);

    // Export section: export function as "rx_jit".
    std::vector<uint8_t> export_section;
    export_section.push_back(0x01);
    export_section.push_back(0x06);
    const char name[] = "rx_jit";
    export_section.insert(export_section.end(), name, name + 6);
    export_section.push_back(0x00);
    export_section.push_back(0x00);
    append_section(module_, 0x07, export_section);

    // Code section: one empty function body.
    std::vector<uint8_t> code;
    code.push_back(0x01);
    code.push_back(0x02);
    code.push_back(0x00);
    code.push_back(0x0b);
    append_section(module_, 0x0a, code);

    return true;
}

} // namespace randomx

#endif // __EMSCRIPTEN__
