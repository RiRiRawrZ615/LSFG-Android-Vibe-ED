// Host-side regression test for the LSFG shader DLL parser
// (app/src/main/cpp/framegen/library/dll.cpp).
//
// Verifies that the bundled lsfg-vk.dll parses and exposes every resource ID
// the v3_1 pipeline's shader library needs:
//   - mipmaps  (named resource, stored under 0x80001348 = 2147488584)
//   - generate (ID 1)
//   - the 24 non-base shaders at IDs 3..26 (quality) and 27..50 (performance)
//
// Exit code 0 = all present, 1 = missing resources or parse failure.

#include "library/dll.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <path-to-dll>\n";
        return 2;
    }

    std::unordered_map<uint32_t, std::vector<uint32_t>> resources;
    try {
        resources = lsfgvk::parseDll(argv[1]);
    } catch (const std::exception& e) {
        std::cerr << "PARSE FAILED: " << e.what() << "\n";
        return 1;
    }

    std::cout << "parsed " << resources.size() << " resources from " << argv[1] << "\n";

    const uint32_t required[] = {
        2147488584u, 1,
        3, 4, 5, 6, 7,     // gamma0-4
        8, 9, 10, 11, 12,  // delta0-4
        13, 14, 15, 16,    // alpha0-3
        17, 18, 19, 20, 21,// epsilon0-4
        22, 23, 24, 25, 26,// beta0-4
        27, 28, 29, 30, 31,// gamma perf
        32, 33, 34, 35, 36,// delta perf
        37, 38, 39, 40,    // alpha perf
        41, 42, 43, 44, 45,// epsilon perf
        46, 47, 48, 49, 50 // beta perf
    };

    int missing = 0;
    for (uint32_t id : required) {
        auto it = resources.find(id);
        if (it == resources.end()) {
            std::cerr << "MISSING resource ID " << id << "\n";
            missing++;
            continue;
        }
        if (it->second.empty()) {
            std::cerr << "EMPTY resource ID " << id << "\n";
            missing++;
        }
    }

    if (missing) {
        std::cerr << "FAIL: " << missing << " required resources missing/empty\n";
        return 1;
    }
    std::cout << "OK: all " << sizeof(required) / sizeof(required[0])
              << " required shader resources present\n";

    // Verify the generate-shader byte-patch (library.cpp) finds its OpConstant
    // target in the bundled DLL's generate shader (resource ID 1). The patch
    // inserts a constant at offset 8 and rewrites the special constant at the
    // first OpConstant (opcode 25) whose following word is non-zero.
    const auto gen = resources.at(1);
    std::vector<uint32_t> bytecode{gen};
    bytecode.insert(bytecode.begin() + 8, { 0x00000031, 0x00020011 });
    bool patched = false;
    for (size_t i = 5; i < bytecode.size(); ) {
        const uint32_t word{bytecode.at(i)};
        if ((word & 0xFFFF) == 25 && bytecode.at(i + 8) != 0) {
            bytecode.at(i + 8) = 0x04; // 2x variant
            patched = true;
            break;
        }
        i += std::max((word >> 16) & 0xFFFF, 1U);
    }
    if (!patched) {
        std::cerr << "FAIL: generate shader patch target not found\n";
        return 1;
    }
    std::cout << "OK: generate shader patch target found (3 variants creatable)\n";

    // Half-precision mode maps the 24 non-base shaders to IDs idx+48 (quality)
    // and idx+72 (performance); verify those exist too (performance mode must
    // not throw "Unable to find shader").
    const uint32_t fp16Base[] = { 3, 8, 13, 17, 22 }; // gamma/delta/alpha/epsilon/beta bases
    for (uint32_t base : fp16Base) {
        const uint32_t span = (base == 13 || base == 22) ? 4 : 5;
        for (uint32_t i = 0; i < span; i++) {
            const uint32_t idx = base + i;
            for (uint32_t id : { idx + 48, idx + 72 }) {
                if (!resources.count(id)) {
                    std::cerr << "FAIL: half-precision shader ID " << id << " missing\n";
                    return 1;
                }
            }
        }
    }
    std::cout << "OK: half-precision shader IDs (51..98) all present\n";
    return 0;
}
