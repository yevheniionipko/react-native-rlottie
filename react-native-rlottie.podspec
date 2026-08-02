require "json"

package = JSON.parse(File.read(File.join(__dir__, "package.json")))

# rlottie internal include roots (each rlottie subdir self-includes its own
# dir) + our committed config.h (replaces rlottie's CMake-generated one) + the
# shared C++ core root. See Chunk 0.3 in detailed-implementation-plan.md.
rlottie_header_search_paths = [
  '"$(PODS_TARGET_SRCROOT)/cpp"',
  '"$(PODS_TARGET_SRCROOT)/cpp/third_party/rlottie_config"',
  '"$(PODS_TARGET_SRCROOT)/cpp/third_party/rlottie/inc"',
  '"$(PODS_TARGET_SRCROOT)/cpp/third_party/rlottie/src/vector"',
  '"$(PODS_TARGET_SRCROOT)/cpp/third_party/rlottie/src/vector/freetype"',
  '"$(PODS_TARGET_SRCROOT)/cpp/third_party/rlottie/src/vector/pixman"',
  '"$(PODS_TARGET_SRCROOT)/cpp/third_party/rlottie/src/vector/stb"',
  '"$(PODS_TARGET_SRCROOT)/cpp/third_party/rlottie/src/lottie"',
].join(" ")

Pod::Spec.new do |s|
  s.name         = "react-native-rlottie"
  s.version      = package["version"]
  s.summary      = package["description"]
  s.homepage     = "https://github.com/your-org/react-native-rlottie" # TODO: set on publish
  s.license      = package["license"]
  s.authors      = "react-native-rlottie contributors"
  s.platforms    = { :ios => "13.0" }
  s.source       = { :git => "https://github.com/your-org/react-native-rlottie.git", :tag => "#{s.version}" }

  s.requires_arc = true
  s.dependency "React-Core"

  # --- Vendored rlottie (Chunk 0.2) ------------------------------------------
  # Compiled directly by CocoaPods (no CMake). Pinned to rlottie's upstream
  # contract: C++14, -fno-exceptions, -fno-rtti. CRITICAL: -U__ARM_NEON__ forces
  # the generic render path on Apple-clang arm64, which (unlike Android NDK
  # arm64) defines __ARM_NEON__ and would otherwise pull in vdrawhelper_neon.cpp
  # -> the 32-bit pixman-arm-neon-asm.S symbols that rlottie never builds.
  # Verified by a host arm64 compile+link of all 34 TUs (Chunk 0.3).
  s.subspec "rlottie" do |ss|
    ss.source_files = "cpp/third_party/rlottie/src/**/*.{cpp,h}",
                      "cpp/third_party/rlottie/inc/*.h",
                      "cpp/third_party/rlottie_config/config.h"
    # Exclude emscripten-only sources, the optional C API, and the 32-bit ASM.
    ss.exclude_files = "cpp/third_party/rlottie/src/wasm/**/*",
                       "cpp/third_party/rlottie/src/binding/c/**/*",
                       "cpp/third_party/rlottie/src/**/*.S"
    ss.preserve_paths = "cpp/third_party/rlottie/**/*"
    ss.compiler_flags = "-std=gnu++14 -fno-exceptions -fno-rtti -U__ARM_NEON__ -DNDEBUG -w"
    ss.pod_target_xcconfig = {
      "HEADER_SEARCH_PATHS" => rlottie_header_search_paths,
      "CLANG_WARN_DOCUMENTATION_COMMENTS" => "NO",
    }
  end

  # --- Shared C++ core + iOS adapter (Objective-C++) -------------------------
  # C++17 (plan §1). Phase 1 fills in cpp/*, Phase 2 fills in ios/*. For now this
  # carries the Chunk 0.3 link probe (cpp/spike/).
  s.subspec "core" do |ss|
    ss.dependency "react-native-rlottie/rlottie"
    ss.source_files = "cpp/*.{h,hpp,cpp}",
                      "cpp/spike/*.{h,cpp}",
                      "ios/**/*.{h,m,mm}"
    ss.pod_target_xcconfig = {
      "CLANG_CXX_LANGUAGE_STANDARD" => "c++17",
      "CLANG_CXX_LIBRARY" => "libc++",
      "HEADER_SEARCH_PATHS" => rlottie_header_search_paths,
    }
  end

  s.default_subspecs = "core"
end
