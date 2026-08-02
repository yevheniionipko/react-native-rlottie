// Chunk 5.5 — a standalone driver for LLVMFuzzerTestOneInput (see
// fuzz_loadfromdata.cpp) used when a real libFuzzer runtime
// (libclang_rt.fuzzer_*, part of a full LLVM/compiler-rt install) is not
// available. Notably, the Xcode Command Line Tools' bundled clang++ in some
// dev environments does NOT ship that runtime archive, so a
// `-fsanitize=fuzzer` link fails at the final link step even though
// compilation succeeds — tests/run-tests.sh's `fuzz` variant detects that link
// failure and falls back to building this driver instead, still under
// ASan+UBSan, so a crash/leak/UB is still caught even without libFuzzer's
// coverage-guided engine.
//
// This is deliberately NOT a reimplementation of libFuzzer: no coverage
// feedback, no corpus minimization. It (1) replays every seed file given on
// the command line through LLVMFuzzerTestOneInput — this alone re-runs the
// whole malformed corpus plus a valid fixture through the exact fuzz entry
// point — then (2) spends the remaining time budget on bounded random mutation
// of those seeds. That is enough to make "wire a fuzz target so it is
// buildable and runnable for a bounded number of iterations" true in any
// clang environment, not just ones with a full libFuzzer install.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

namespace {

std::vector<std::uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

// Cheap byte-level mutations: flip/insert/delete/duplicate a handful of bytes.
// Not remotely as effective as coverage-guided mutation, but it exercises the
// parser with structurally-close-but-corrupted variants of real seeds, which
// is exactly the kind of input the corpus files were written to represent.
std::vector<std::uint8_t> mutate(std::vector<std::uint8_t> buf, std::mt19937& rng) {
    if (buf.empty()) {
        buf.push_back(static_cast<std::uint8_t>(rng() & 0xff));
        return buf;
    }
    const int kMutations = 1 + static_cast<int>(rng() % 4);
    for (int i = 0; i < kMutations; ++i) {
        const auto op = rng() % 4;
        const auto pos = rng() % buf.size();
        switch (op) {
            case 0:  // flip a byte
                buf[pos] = static_cast<std::uint8_t>(rng() & 0xff);
                break;
            case 1:  // insert a random byte
                buf.insert(buf.begin() + static_cast<long>(pos),
                          static_cast<std::uint8_t>(rng() & 0xff));
                break;
            case 2:  // delete a byte
                if (buf.size() > 1) buf.erase(buf.begin() + static_cast<long>(pos));
                break;
            case 3: {  // duplicate a chunk (stresses size-limit handling)
                const auto len = std::min<std::size_t>(64, buf.size() - pos);
                buf.insert(buf.end(), buf.begin() + static_cast<long>(pos),
                          buf.begin() + static_cast<long>(pos + len));
                break;
            }
        }
    }
    return buf;
}

}  // namespace

int main(int argc, char** argv) {
    double seconds = 20.0;
    std::vector<std::string> seedPaths;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind("-max_total_time=", 0) == 0) {
            seconds = std::atof(arg.c_str() + std::strlen("-max_total_time="));
        } else if (arg.rfind('-', 0) != 0) {
            seedPaths.push_back(arg);
        }
    }

    std::vector<std::vector<std::uint8_t>> seeds;
    for (const auto& p : seedPaths) {
        seeds.push_back(readFile(p));
    }
    if (seeds.empty()) {
        seeds.push_back({'{', '}'});
    }

    std::printf("standalone fuzz driver: %zu seed(s), budget %.1fs\n", seeds.size(), seconds);

    // Pass 1: every seed verbatim — this alone replays the whole malformed
    // corpus through the exact fuzz entry point.
    for (const auto& s : seeds) {
        LLVMFuzzerTestOneInput(s.data(), s.size());
    }

    // Pass 2: bounded random mutation for the remaining time budget.
    std::mt19937 rng(12345);  // fixed seed: reproducible smoke runs
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    std::uint64_t iterations = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto& base = seeds[rng() % seeds.size()];
        const auto mutated = mutate(base, rng);
        LLVMFuzzerTestOneInput(mutated.data(), mutated.size());
        ++iterations;
    }

    std::printf("standalone fuzz driver: completed %llu iterations (%zu seed replays + mutations)\n",
               static_cast<unsigned long long>(iterations), seeds.size());
    return 0;
}
