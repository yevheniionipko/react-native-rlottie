// Chunk 5.3 — deterministic cache key composition (native half). See
// cpp/CacheKey.h for the design and the collision-avoidance argument.
#include "CacheKey.h"
#include "Sha256.h"
#include "test_harness.h"

using namespace rnrlottie;

TEST(cachekey_sha256_known_vectors) {
    // FIPS 180-4 / NIST test vectors — pins the implementation against a
    // known-correct digest, not just internal self-consistency.
    CHECK(sha256Hex("") ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");  // NOLINT
    CHECK(sha256Hex("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");  // NOLINT
}

TEST(cachekey_same_bytes_same_key) {
    const std::string json = R"({"v":"5.5.2","fr":30,"w":64,"h":64})";
    const auto k1 = composeCacheKey(json, "caller", "commit-a", "");
    const auto k2 = composeCacheKey(json, "caller", "commit-a", "");
    CHECK(k1 == k2);
}

TEST(cachekey_different_bytes_different_key) {
    const auto k1 = composeCacheKey(R"({"w":64})", "caller", "commit-a", "");
    const auto k2 = composeCacheKey(R"({"w":65})", "caller", "commit-a", "");
    CHECK(k1 != k2);
}

TEST(cachekey_caller_key_cannot_alias_different_payloads) {
    // Two different payloads, same caller-supplied label (and even the SAME
    // string masquerading as the OTHER payload's TS-computed hash) — must not
    // collide, because the native sha256 prefix is computed from the actual
    // bytes, not trusted from the caller.
    const std::string sameLabel = "same-caller-label";
    const auto k1 = composeCacheKey("{\"payload\":1}", sameLabel, "commit-a", "");
    const auto k2 = composeCacheKey("{\"payload\":2}", sameLabel, "commit-a", "");
    CHECK(k1 != k2);

    // Even a caller trying to impersonate payload 1's key by putting its hash
    // in their own "caller key" field cannot make payload 2 resolve to k1: the
    // native hash is still computed over payload 2's actual bytes and forms
    // the first field, which is where k1 and k2 diverge.
    const auto forged = composeCacheKey("{\"payload\":2}", k1, "commit-a", "");
    CHECK(forged != k1);
}

TEST(cachekey_changes_with_rlottie_commit) {
    const std::string json = R"({"w":64,"h":64})";
    const auto k1 = composeCacheKey(json, "caller", "commit-a", "");
    const auto k2 = composeCacheKey(json, "caller", "commit-b", "");
    CHECK(k1 != k2);
}

TEST(cachekey_changes_with_resource_path) {
    // resourcePath is part of parseConfig: it changes how external raster
    // assets resolve for byte-identical JSON, so it must affect the key too.
    const std::string json = R"({"w":64,"h":64})";
    const auto k1 = composeCacheKey(json, "caller", "commit-a", "/app/assets/a");
    const auto k2 = composeCacheKey(json, "caller", "commit-a", "/app/assets/b");
    CHECK(k1 != k2);
}

TEST(cachekey_starts_with_content_hash) {
    const std::string json = R"({"w":64,"h":64})";
    const auto key = composeCacheKey(json, "whatever:with:colons", "commit-a", "path:with:colons");
    CHECK(key.rfind(sha256Hex(json) + ":", 0) == 0);
}
