// Chunk 7.4 (Part C) — LEG 3: the ONLY leg that touches a REAL
// android.graphics.Bitmap. Run standalone under `app_process` (no gradle, no
// APK — see scripts/run-golden-android.sh), on the booted emulator/device.
//
// This is the measurement that upgrades docs/pixel-format-report.md's Android
// byte-order claim (Bitmap.Config.ARGB_8888 is R,G,B,A in native memory, so
// rlottie's B,G,R,A needs an R<->B swap — Chunks 0.4/3.1) from documentation
// to an actual on-device observation: load the already R<->B-swapped bytes
// (written by golden_runner's Leg 2, via the exact rlottieSurfaceToAndroid()
// the JNI blit uses) into a real Bitmap with copyPixelsFromBuffer, then ask
// the REAL platform graphics stack (getPixel) what color landed there. If the
// channel order were wrong, every non-gray pixel's R and B would come back
// swapped versus what we expect here, and this leg would fail loudly.
//
// Two tolerance regimes, and this is deliberate (mirrors GoldenCompare.h's
// two-leg reasoning — do not "fix" a mismatch here by loosening it further
// without re-reading this comment):
//
//   - FULLY OPAQUE pixels (alpha == 255): premultiplied == straight-alpha, so
//     there is nothing to round. Bitmap.getPixel() must return EXACTLY the
//     R,G,B we wrote. Any delta at all here is a real ordering/precision bug,
//     not measurement noise — checked with zero tolerance.
//   - PARTIALLY TRANSPARENT pixels (0 < alpha < 255): the bytes we load are
//     PREMULTIPLIED (that's rlottie's — and Android ARGB_8888's — native
//     format), but Bitmap.getPixel() is documented to always return a
//     STRAIGHT (un-premultiplied) color, so Skia divides color-by-alpha
//     internally before handing it back. That division is real per-pixel
//     arithmetic (with its own rounding), independent of and in addition to
//     whatever rounding this file's own reference un-premultiply below does
//     to derive the "expected" value — two independently-rounded divisions of
//     the same premultiplied byte can legitimately land one 8-bit step apart.
//     A bounded per-channel tolerance (kAlphaBlendedTolerance) absorbs that
//     without hiding a real channel-order bug, which would produce a much
//     larger, systematic delta (order of 100+, not <=2) because R and B would
//     be entirely transposed rather than off by a rounding step.
//   - Fully transparent pixels (alpha == 0) carry no color information by
//     definition (any RGB is valid under alpha 0), so they are skipped rather
//     than compared.
//
// Reads a manifest written by golden_runner (native Leg 2) instead of
// hardcoding a fixture list — CLAUDE.md's InputLimits note already warns
// about exactly this kind of hand-copied-subset drift hazard, and duplicating
// tests/cpp/GoldenFixtures.h's table into a second, Java-side list here would
// be the same mistake in a different language.
import android.graphics.Bitmap;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;

public class BitmapGoldenMain {
    // See the header comment above: this is rounding slack for Skia's
    // un-premultiply division, not slack for a channel-order bug.
    //
    // MEASURED, not assumed: across the whole corpus on emulator-5554 (arm64,
    // API 37) the worst delta actually observed among blended pixels was 1 —
    // one step, on shapes frame 10 only; every other frame was exact. So the
    // rounding divergence is real (a 0 tolerance would fail) but tiny, and 2
    // leaves exactly one step of headroom for a different Skia version. Each
    // frame's line prints `worstBlendedDelta=<observed>/<allowed>` so this
    // number stays evidenced rather than inherited on trust.
    private static final int ALPHA_BLENDED_TOLERANCE = 2;

    private static final class ManifestEntry {
        String name;
        int frame;
        int width;
        int height;
        String fileName;
    }

    private static byte[] readAllBytes(File f) throws IOException {
        try (InputStream in = new FileInputStream(f)) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) >= 0) {
                out.write(buf, 0, n);
            }
            return out.toByteArray();
        }
    }

    private static List<ManifestEntry> readManifest(File manifestFile) throws IOException {
        List<ManifestEntry> entries = new ArrayList<>();
        String text = new String(readAllBytes(manifestFile), "UTF-8");
        for (String line : text.split("\n")) {
            line = line.trim();
            if (line.isEmpty()) continue;
            String[] parts = line.split(" ");
            if (parts.length != 5) {
                throw new IOException("malformed manifest line: " + line);
            }
            ManifestEntry e = new ManifestEntry();
            e.name = parts[0];
            e.frame = Integer.parseInt(parts[1]);
            e.width = Integer.parseInt(parts[2]);
            e.height = Integer.parseInt(parts[3]);
            e.fileName = parts[4];
            entries.add(e);
        }
        return entries;
    }

    // Reference un-premultiply of OUR OWN input bytes, purely to derive what
    // we expect getPixel() to report. Uses the standard round-to-nearest
    // formula; Skia's internal table-based divide may round a different
    // half-step, which is exactly what ALPHA_BLENDED_TOLERANCE accounts for.
    private static int unpremultiplyChannel(int c, int a) {
        if (a == 0) return 0;
        int v = (c * 255 + a / 2) / a;
        if (v > 255) v = 255;
        return v;
    }

    private static boolean checkEntry(File dir, ManifestEntry e, StringBuilder log) {
        File dataFile = new File(dir, e.fileName);
        byte[] rgba;
        try {
            rgba = readAllBytes(dataFile);
        } catch (IOException ex) {
            log.append("  FAIL: cannot read ").append(dataFile).append(": ")
               .append(ex.getMessage()).append('\n');
            return false;
        }
        final int expectedBytes = e.width * e.height * 4;
        if (rgba.length != expectedBytes) {
            log.append("  FAIL: ").append(e.name).append(" frame ").append(e.frame)
               .append(": expected ").append(expectedBytes).append(" bytes, got ")
               .append(rgba.length).append('\n');
            return false;
        }

        Bitmap bitmap = Bitmap.createBitmap(e.width, e.height, Bitmap.Config.ARGB_8888);
        bitmap.copyPixelsFromBuffer(ByteBuffer.wrap(rgba));

        boolean ok = true;
        int checkedOpaque = 0, checkedBlended = 0, skippedTransparent = 0;
        // The worst per-channel delta actually OBSERVED among blended pixels.
        // Reported unconditionally so ALPHA_BLENDED_TOLERANCE stays an evidenced
        // allowance rather than an assumed one: if this stays 0 across the whole
        // corpus, the tolerance is documenting a theoretical rounding step that
        // this Skia version does not in fact take, and a reader can see that
        // instead of having to trust the constant.
        int worstBlendedDelta = 0;
        for (int y = 0; y < e.height; y++) {
            for (int x = 0; x < e.width; x++) {
                final int off = (y * e.width + x) * 4;
                final int r = rgba[off] & 0xFF;
                final int g = rgba[off + 1] & 0xFF;
                final int b = rgba[off + 2] & 0xFF;
                final int a = rgba[off + 3] & 0xFF;

                if (a == 0) {
                    skippedTransparent++;
                    continue;
                }

                final int expectedR = a == 255 ? r : unpremultiplyChannel(r, a);
                final int expectedG = a == 255 ? g : unpremultiplyChannel(g, a);
                final int expectedB = a == 255 ? b : unpremultiplyChannel(b, a);
                final int tolerance = a == 255 ? 0 : ALPHA_BLENDED_TOLERANCE;

                final int actual = bitmap.getPixel(x, y);
                final int actualA = (actual >>> 24) & 0xFF;
                final int actualR = (actual >>> 16) & 0xFF;
                final int actualG = (actual >>> 8) & 0xFF;
                final int actualB = actual & 0xFF;

                final int dr = Math.abs(actualR - expectedR);
                final int dg = Math.abs(actualG - expectedG);
                final int db = Math.abs(actualB - expectedB);
                final int da = Math.abs(actualA - a);

                if (a != 255) {
                    worstBlendedDelta = Math.max(worstBlendedDelta,
                            Math.max(dr, Math.max(dg, db)));
                }

                if (dr > tolerance || dg > tolerance || db > tolerance || da > 0) {
                    log.append("  FAIL: ").append(e.name).append(" frame ").append(e.frame)
                       .append(" pixel (").append(x).append(',').append(y).append("): ")
                       .append("expected ARGB=(").append(a).append(',').append(expectedR)
                       .append(',').append(expectedG).append(',').append(expectedB)
                       .append(") got (").append(actualA).append(',').append(actualR)
                       .append(',').append(actualG).append(',').append(actualB)
                       .append(") tolerance=").append(tolerance).append('\n');
                    ok = false;
                } else if (a == 255) {
                    checkedOpaque++;
                } else {
                    checkedBlended++;
                }
            }
        }

        log.append("  ").append(e.name).append(" frame ").append(e.frame)
           .append(": opaque=").append(checkedOpaque).append(" blended=").append(checkedBlended)
           .append(" transparent-skipped=").append(skippedTransparent)
           .append(" worstBlendedDelta=").append(worstBlendedDelta)
           .append('/').append(ALPHA_BLENDED_TOLERANCE)
           .append(ok ? " -> OK" : " -> HAS FAILURES").append('\n');
        return ok;
    }

    public static void main(String[] args) {
        System.out.println("=== Chunk 7.4 (Part C) Leg 3: real android.graphics.Bitmap check ===");
        if (args.length != 1) {
            System.err.println("usage: BitmapGoldenMain <dir-containing-manifest.txt>");
            System.exit(2);
        }
        File dir = new File(args[0]);
        File manifestFile = new File(dir, "manifest.txt");

        List<ManifestEntry> entries;
        try {
            entries = readManifest(manifestFile);
        } catch (IOException ex) {
            System.err.println("cannot read manifest " + manifestFile + ": " + ex.getMessage());
            System.exit(2);
            return;
        }
        if (entries.isEmpty()) {
            System.err.println("manifest is empty at " + manifestFile
                    + " -- Leg 2 (golden_runner) produced no conversion artifacts");
            System.exit(2);
            return;
        }

        int failures = 0;
        StringBuilder log = new StringBuilder();
        for (ManifestEntry e : entries) {
            if (!checkEntry(dir, e, log)) failures++;
        }
        System.out.print(log);

        System.out.println();
        if (failures == 0) {
            System.out.println("Leg 3 (" + entries.size() + " frame(s)): PASS");
            System.exit(0);
        } else {
            System.out.println("Leg 3: FAIL (" + failures + "/" + entries.size()
                    + " frame(s) had at least one bad pixel)");
            System.exit(1);
        }
    }
}
