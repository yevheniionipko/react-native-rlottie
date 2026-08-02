// Chunk 1.7 — test harness runtime + entry point.
#include "test_harness.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <utility>

#ifndef RNRLOTTIE_TEST_DATA_DIR
#define RNRLOTTIE_TEST_DATA_DIR "tests"
#endif

namespace tst {
namespace {

std::vector<std::pair<const char*, Fn>>& registry() {
    static std::vector<std::pair<const char*, Fn>> r;
    return r;
}

int g_failures = 0;
const char* g_current = "";

}  // namespace

Reg::Reg(const char* name, Fn fn) { registry().push_back({name, fn}); }

void fail(const char* expr, const char* file, int line) {
    ++g_failures;
    std::printf("  FAIL [%s] %s (%s:%d)\n", g_current, expr, file, line);
}

std::string dataPath(const char* rel) {
    return std::string(RNRLOTTIE_TEST_DATA_DIR) + "/" + rel;
}

std::string readText(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<std::uint8_t> readBinary(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

}  // namespace tst

int main() {
    using namespace tst;
    int total = 0;
    for (auto& t : registry()) {
        g_current = t.first;
        const int before = g_failures;
        t.second();
        ++total;
        if (g_failures == before) std::printf("  ok   %s\n", t.first);
    }
    if (g_failures == 0) {
        std::printf("\nALL %d TESTS PASSED\n", total);
        return 0;
    }
    std::printf("\n%d CHECK FAILURE(S) across %d tests\n", g_failures, total);
    return 1;
}
