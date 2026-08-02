// Chunk 4.1 — a dependency-free SHA-256, used only for cache-key derivation.
//
// Why hand-rolled: this package ships with ZERO runtime dependencies, and React
// Native has no built-in crypto (`crypto.subtle` is absent on both Hermes and
// JSC, and its digest API is async anyway, which would make source
// normalization async and force a render-time await).
//
// Why SHA-256 and not a cheap 32/64-bit hash: the key's job is to guarantee two
// DIFFERENT payloads can never silently share a cache entry (plan §15). A short
// hash makes that a birthday-bound question over user content; a cryptographic
// digest makes it a non-issue. This is not used for any security decision.
//
// COST: pure-JS SHA-256 runs on the order of tens of MB/s, so a large animation
// (the 16 MiB `InputLimits::maxJsonBytes` ceiling) can block the JS thread for a
// noticeable fraction of a second. Mitigated in `source.ts` by hashing each
// source object at most once (identity-memoized). If that ever proves too slow
// for real payloads, the right fix is to hash natively — the C++ side already
// has the bytes in hand — which is Chunk 5.3's call to make.

const K = new Uint32Array([
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
  0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
  0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
  0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
  0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
  0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
  0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
  0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
  0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
]);

/**
 * UTF-8 encodes `input`.
 *
 * Hand-rolled rather than using `TextEncoder`: it is absent on older JSC/Hermes
 * builds within this package's supported RN range (>= 0.68), and a missing
 * global here would be a runtime crash on exactly the oldest devices.
 *
 * Unpaired surrogates (which `JSON.stringify` can legitimately produce from a
 * malformed string) are encoded as U+FFFD rather than throwing — a cache key
 * must never be the thing that crashes a render.
 */
function utf8Bytes(input: string): Uint8Array {
  const out: number[] = [];
  for (let i = 0; i < input.length; i++) {
    let code = input.charCodeAt(i);
    if (code >= 0xd800 && code <= 0xdbff) {
      const next = i + 1 < input.length ? input.charCodeAt(i + 1) : 0;
      if (next >= 0xdc00 && next <= 0xdfff) {
        code = 0x10000 + ((code - 0xd800) << 10) + (next - 0xdc00);
        i++;
      } else {
        code = 0xfffd; // lone high surrogate
      }
    } else if (code >= 0xdc00 && code <= 0xdfff) {
      code = 0xfffd; // lone low surrogate
    }

    if (code < 0x80) {
      out.push(code);
    } else if (code < 0x800) {
      out.push(0xc0 | (code >> 6), 0x80 | (code & 0x3f));
    } else if (code < 0x10000) {
      out.push(
        0xe0 | (code >> 12),
        0x80 | ((code >> 6) & 0x3f),
        0x80 | (code & 0x3f),
      );
    } else {
      out.push(
        0xf0 | (code >> 18),
        0x80 | ((code >> 12) & 0x3f),
        0x80 | ((code >> 6) & 0x3f),
        0x80 | (code & 0x3f),
      );
    }
  }
  return Uint8Array.from(out);
}

function rotr(x: number, n: number): number {
  return ((x >>> n) | (x << (32 - n))) >>> 0;
}

/** Lowercase hex SHA-256 of the UTF-8 encoding of `input`. */
export function sha256Hex(input: string): string {
  const bytes = utf8Bytes(input);
  const byteLen = bytes.length;

  // Pad: 0x80, then zeros, then the 64-bit big-endian bit length.
  const paddedLen = (((byteLen + 8) >> 6) + 1) << 6;
  const m = new Uint8Array(paddedLen);
  m.set(bytes);
  m[byteLen] = 0x80;

  // Bit length as a 64-bit big-endian value. Split so lengths above 2^29 bytes
  // (where `byteLen * 8` exceeds 32 bits) still encode correctly.
  const bitLen = byteLen * 8;
  const hi = Math.floor(bitLen / 0x100000000);
  const lo = bitLen >>> 0;
  m[paddedLen - 8] = (hi >>> 24) & 0xff;
  m[paddedLen - 7] = (hi >>> 16) & 0xff;
  m[paddedLen - 6] = (hi >>> 8) & 0xff;
  m[paddedLen - 5] = hi & 0xff;
  m[paddedLen - 4] = (lo >>> 24) & 0xff;
  m[paddedLen - 3] = (lo >>> 16) & 0xff;
  m[paddedLen - 2] = (lo >>> 8) & 0xff;
  m[paddedLen - 1] = lo & 0xff;

  let h0 = 0x6a09e667;
  let h1 = 0xbb67ae85;
  let h2 = 0x3c6ef372;
  let h3 = 0xa54ff53a;
  let h4 = 0x510e527f;
  let h5 = 0x9b05688c;
  let h6 = 0x1f83d9ab;
  let h7 = 0x5be0cd19;

  const w = new Uint32Array(64);

  for (let offset = 0; offset < paddedLen; offset += 64) {
    for (let i = 0; i < 16; i++) {
      const j = offset + i * 4;
      w[i] =
        ((m[j] << 24) | (m[j + 1] << 16) | (m[j + 2] << 8) | m[j + 3]) >>> 0;
    }
    for (let i = 16; i < 64; i++) {
      const a = w[i - 15];
      const b = w[i - 2];
      const s0 = (rotr(a, 7) ^ rotr(a, 18) ^ (a >>> 3)) >>> 0;
      const s1 = (rotr(b, 17) ^ rotr(b, 19) ^ (b >>> 10)) >>> 0;
      w[i] = (w[i - 16] + s0 + w[i - 7] + s1) >>> 0;
    }

    let a = h0;
    let b = h1;
    let c = h2;
    let d = h3;
    let e = h4;
    let f = h5;
    let g = h6;
    let h = h7;

    for (let i = 0; i < 64; i++) {
      const S1 = (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) >>> 0;
      const ch = ((e & f) ^ (~e & g)) >>> 0;
      const temp1 = (h + S1 + ch + K[i] + w[i]) >>> 0;
      const S0 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) >>> 0;
      const maj = ((a & b) ^ (a & c) ^ (b & c)) >>> 0;
      const temp2 = (S0 + maj) >>> 0;

      h = g;
      g = f;
      f = e;
      e = (d + temp1) >>> 0;
      d = c;
      c = b;
      b = a;
      a = (temp1 + temp2) >>> 0;
    }

    h0 = (h0 + a) >>> 0;
    h1 = (h1 + b) >>> 0;
    h2 = (h2 + c) >>> 0;
    h3 = (h3 + d) >>> 0;
    h4 = (h4 + e) >>> 0;
    h5 = (h5 + f) >>> 0;
    h6 = (h6 + g) >>> 0;
    h7 = (h7 + h) >>> 0;
  }

  return (
    hex32(h0) +
    hex32(h1) +
    hex32(h2) +
    hex32(h3) +
    hex32(h4) +
    hex32(h5) +
    hex32(h6) +
    hex32(h7)
  );
}

function hex32(value: number): string {
  return (value >>> 0).toString(16).padStart(8, '0');
}
