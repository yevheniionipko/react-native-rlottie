// Chunk 0.4 — pixel-format spike harness (shared, platform-neutral).
//
// rlottie's Surface pixel format is decided by the rlottie renderer itself, not
// by the host platform, so rendering the probe fixture with the same rlottie we
// vendor determines the authoritative channel order, premultiplication, stride,
// and row orientation. Platform presenters (CGImage / Android Bitmap) only
// REINTERPRET these bytes — see docs/pixel-format-report.md.
//
// Usage: pixel_probe <path-to-pixel-probe.json> [out.raw]
// Prints, for each sample pixel, the uint32 word and its 4 memory bytes in
// address order (byte[0] = lowest address = little-endian LSB).
#include <rlottie.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {
constexpr std::size_t kW = 64;
constexpr std::size_t kH = 64;

void dumpPixel(const char* label, std::uint32_t px) {
    const auto b0 = static_cast<unsigned>(px & 0xFF);
    const auto b1 = static_cast<unsigned>((px >> 8) & 0xFF);
    const auto b2 = static_cast<unsigned>((px >> 16) & 0xFF);
    const auto b3 = static_cast<unsigned>((px >> 24) & 0xFF);
    std::printf("%-16s word=0x%08X  mem[0..3]=%02X %02X %02X %02X\n", label, px,
                b0, b1, b2, b3);
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <pixel-probe.json> [out.raw]\n", argv[0]);
        return 2;
    }
    std::ifstream in(argv[1]);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string json = ss.str();

    auto anim = rlottie::Animation::loadFromData(json, /*key=*/"pixel-probe",
                                                 /*resourcePath=*/"",
                                                 /*cachePolicy=*/false);
    if (!anim) {
        std::fprintf(stderr, "rlottie failed to parse fixture\n");
        return 1;
    }

    std::vector<std::uint32_t> buffer(kW * kH, 0xDEADBEEF);  // sentinel fill
    rlottie::Surface surface(buffer.data(), kW, kH, kW * sizeof(std::uint32_t));
    anim->renderSync(0, surface);

    std::printf("rlottie totalFrame=%zu duration=%.3f  surface=%zux%zu stride=%zu bytes\n",
                anim->totalFrame(), anim->duration(), kW, kH, surface.bytesPerLine());

    auto at = [&](std::size_t x, std::size_t y) { return buffer[y * kW + x]; };
    std::puts("--- sample pixels (fixture regions) ---");
    dumpPixel("red   (10,10)", at(10, 10));   // opaque #ff0000
    dumpPixel("green (54,10)", at(54, 10));    // opaque #00ff00
    dumpPixel("blue  (10,54)", at(10, 54));    // opaque #0000ff
    dumpPixel("red50 (54,54)", at(54, 54));    // #ff0000 @ 50% layer opacity
    dumpPixel("clear (32,32)", at(32, 32));    // transparent center cross

    if (argc >= 3) {
        std::ofstream raw(argv[2], std::ios::binary);
        raw.write(reinterpret_cast<const char*>(buffer.data()),
                  static_cast<std::streamsize>(buffer.size() * sizeof(std::uint32_t)));
        std::printf("wrote raw ARGB buffer: %s (%zu bytes)\n", argv[2],
                    buffer.size() * sizeof(std::uint32_t));
    }
    return 0;
}
