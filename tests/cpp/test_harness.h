// Chunk 1.7 — a tiny, dependency-free test framework for the shared C++ core.
//
// Deliberately not GoogleTest/Catch2: the whole project vendors its deps and
// builds offline, so a ~80-line committed harness avoids another third-party
// tree. Register a test with TEST(name){ ... } and assert with CHECK(cond).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tst {

using Fn = void (*)();

struct Reg {
    Reg(const char* name, Fn fn);
};

void fail(const char* expr, const char* file, int line);

// Resolve a path under the test data dir (<repo>/tests), set at compile time.
std::string dataPath(const char* rel);

std::string readText(const std::string& path);
std::vector<std::uint8_t> readBinary(const std::string& path);

}  // namespace tst

#define CHECK(cond)                                        \
    do {                                                   \
        if (!(cond)) ::tst::fail(#cond, __FILE__, __LINE__); \
    } while (0)

#define TEST(name)                                          \
    static void name();                                     \
    static ::tst::Reg tst_reg_##name(#name, &name);         \
    static void name()
