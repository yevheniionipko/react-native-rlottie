require "json"

require File.join(File.dirname(`node --print "require.resolve('react-native/package.json')"`), "scripts/react_native_pods")

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
  s.homepage     = "https://github.com/yevheniionipko/react-native-rlottie"
  s.license      = package["license"]
  s.authors      = "react-native-rlottie contributors"
  s.platforms    = { :ios => "13.0" }
  s.source       = { :git => "https://github.com/yevheniionipko/react-native-rlottie.git", :tag => "#{s.version}" }

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
    # Exclude emscripten-only sources, the optional C API, the 32-bit ASM, and
    # rapidjson's MSVC-only <stdint.h>/<inttypes.h> shims (vendored for
    # pre-C99 MSVC only — each literally `#error`s under any other compiler).
    # Chunk 8.1's example app is what caught this: `source_files` above is a
    # recursive `**/*.h` glob, so it swept these headers in too, and Xcode's
    # per-target header map (on by default) then made `#include <stdint.h>`
    # resolve to msinttypes/stdint.h ahead of the real system header for the
    # WHOLE target — poisoning every TU that includes <stdint.h>, not just
    # rapidjson's own, and producing exactly that `#error` plus a cascade of
    # redefinition errors. A plain command-line compile (Chunk 0.3's host
    # probe) never exercises Xcode's header map, so it never surfaced this.
    ss.exclude_files = "cpp/third_party/rlottie/src/wasm/**/*",
                       "cpp/third_party/rlottie/src/binding/c/**/*",
                       "cpp/third_party/rlottie/src/**/*.S",
                       "cpp/third_party/rlottie/src/lottie/rapidjson/msinttypes/**/*"
    ss.preserve_paths = "cpp/third_party/rlottie/**/*"
    ss.compiler_flags = "-std=gnu++14 -fno-exceptions -fno-rtti -U__ARM_NEON__ -DNDEBUG -w"
    ss.pod_target_xcconfig = {
      "HEADER_SEARCH_PATHS" => rlottie_header_search_paths,
      "CLANG_WARN_DOCUMENTATION_COMMENTS" => "NO",
    }
  end

  # --- Shared C++ core + iOS adapter (Objective-C++) -------------------------
  # Phase 1 fills in cpp/*, Phase 2 fills in ios/*. cpp/spike/ carries the
  # Chunk 0.3 link probe. `ios/**/*.{h,m,mm}` already recurses into
  # ios/fabric/ (Chunk 9.8's Fabric component view, guarded internally by
  # #ifdef RCT_NEW_ARCH_ENABLED — see docs/new-architecture-design.md §1.4), so
  # no separate glob is needed for it. No CLANG_CXX_LANGUAGE_STANDARD pin here
  # (see the trailing install_modules_dependencies call).
  s.subspec "core" do |ss|
    ss.dependency "react-native-rlottie/rlottie"
    ss.source_files = "cpp/*.{h,hpp,cpp}",
                      "cpp/spike/*.{h,cpp}",
                      "ios/**/*.{h,m,mm}"
    ss.pod_target_xcconfig = {
      "CLANG_CXX_LIBRARY" => "libc++",
      "HEADER_SEARCH_PATHS" => rlottie_header_search_paths,
    }
  end

  s.default_subspecs = "core"

  # MUST be the last statement in the spec block: it reads then overwrites
  # spec.compiler_flags and spec.pod_target_xcconfig (see
  # node_modules/react-native/scripts/cocoapods/new_architecture.rb:76-104 and
  # docs/new-architecture-design.md §1.4). Anything set on `s` after this call
  # is lost.
  install_modules_dependencies(s)
end
