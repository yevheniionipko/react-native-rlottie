// Chunk 3.1 — the wire format for load/error events polled from Kotlin.
//
// The poll-based sink (AndroidFrameSink) hands the UI thread a PlayerEvent; JNI
// returns it to Kotlin as ONE string rather than building a Java object graph
// from C++ (no cached jclass/jmethodID, no local-ref bookkeeping in the JNI
// layer). RlottieView parses it in Chunk 3.2.
//
//   "loaded:<w>,<h>,<frameRate>,<totalFrames>,<duration>,<markerCount>[,<name>,<start>,<end>]..."
//   "error:<CODE>:<message>"     — split with limit 3; the message is the tail
//
// <CODE> comes from cpp/ErrorCode.h, the single source of truth both platform
// adapters map through, so the strings JS sees are identical on iOS and Android.
//
// Deliberately JNI-free so tests/cpp/android_tests.cpp can pin the format.
#pragma once

#include <string>

#include "ErrorCode.h"
#include "FrameSink.h"

namespace rnrlottie {

inline std::string sanitizeInline(std::string s) {
    for (char& c : s) {
        if (c == '\n' || c == '\r') c = ' ';
    }
    return s;
}

// Marker names are attacker-influenced (they come from the Lottie JSON) and the
// loaded payload is comma-delimited, so commas and newlines are neutralized.
inline std::string sanitizeMarkerName(std::string s) {
    for (char& c : s) {
        if (c == ',' || c == '\n' || c == '\r') c = ' ';
    }
    return s;
}

inline std::string encodeEvent(const PlayerEvent& ev) {
    if (ev.type == PlayerEvent::Type::Loaded) {
        const auto& md = ev.metadata;
        std::string out = "loaded:";
        out += std::to_string(md.width) + "," + std::to_string(md.height) + "," +
               std::to_string(md.frameRate) + "," + std::to_string(md.totalFrames) + "," +
               std::to_string(md.duration) + "," + std::to_string(md.markers.size());
        for (const auto& m : md.markers) {
            out += "," + sanitizeMarkerName(m.name) + "," + std::to_string(m.startFrame) + "," +
                   std::to_string(m.endFrame);
        }
        return out;
    }
    return std::string("error:") + errorCodeString(ev.error.code) + ":" +
           sanitizeInline(ev.error.message);
}

}  // namespace rnrlottie
