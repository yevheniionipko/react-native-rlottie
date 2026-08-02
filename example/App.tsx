/**
 * react-native-rlottie example app (Chunk 8.1).
 *
 * One screen exercising the real public surface of the library rather than a
 * hello-world: two `RlottieView`s (a bundled asset:/// source and an inline
 * JSON object source), every imperative ref command, the playback-shaping
 * props, the three override prop arrays, `metricsEnabled`/`onMetrics`, every
 * lifecycle event, and the global `Rlottie` module. See example/README.md for
 * how to run this on Android/iOS.
 *
 * Legacy Architecture only (newArchEnabled=false) — see android/gradle.properties
 * and ios/Podfile, and the root CLAUDE.md.
 *
 * @format
 */

import React, {useCallback, useRef, useState} from 'react';
import {
  Alert,
  Platform,
  ScrollView,
  StatusBar,
  StyleSheet,
  Switch,
  Text,
  TextInput,
  TouchableOpacity,
  View,
} from 'react-native';
import RlottieView, {
  Rlottie,
  type RlottieColorOverride,
  type RlottieErrorEvent,
  type RlottieLoadedEvent,
  type RlottieMarker,
  type RlottieMetricsEvent,
  type RlottieOpacityOverride,
  type RlottieResizeMode,
  type RlottieStrokeWidthOverride,
  type RlottieViewRef,
} from 'react-native-rlottie';

// Bundled from android/app/src/main/assets/example-lottie.json (Android) and
// the iOS app bundle (ios/RlottieExample/example-lottie.json, added as a Copy
// Bundle Resources entry) — see docs/bridge-contract.md's `asset:///` scheme,
// identical on both platforms.
const ASSET_SOURCE = {uri: 'asset:///example-lottie.json'} as const;

// The inline-object path: Metro inlines a `require()`d .json as a plain
// object (it does NOT go through the asset registry), so this exercises
// `source={{json: <object>}}` — hoisted to module scope, per src/types.ts's
// warning, so it is stringified/hashed exactly once rather than on every
// render.
const INLINE_JSON_SOURCE = {
  json: require('./assets/example-lottie.json'),
} as const;

const RESIZE_MODES: RlottieResizeMode[] = [
  'contain',
  'cover',
  'stretch',
  'center',
];

function Section({title, children}: {title: string; children: React.ReactNode}) {
  return (
    <View style={styles.section}>
      <Text style={styles.sectionTitle}>{title}</Text>
      {children}
    </View>
  );
}

function Btn({
  label,
  onPress,
  disabled,
}: {
  label: string;
  onPress: () => void;
  disabled?: boolean;
}) {
  return (
    <TouchableOpacity
      style={[styles.btn, disabled && styles.btnDisabled]}
      onPress={onPress}
      disabled={disabled}>
      <Text style={styles.btnText}>{label}</Text>
    </TouchableOpacity>
  );
}

function Row({children}: {children: React.ReactNode}) {
  return <View style={styles.row}>{children}</View>;
}

export default function App() {
  const primaryRef = useRef<RlottieViewRef>(null);

  const [log, setLog] = useState<string[]>([]);
  const appendLog = useCallback((line: string) => {
    setLog(prev => [`${new Date().toLocaleTimeString()}  ${line}`, ...prev].slice(0, 30));
  }, []);

  // --- Playback-shaping props -------------------------------------------
  const [loop, setLoop] = useState(true);
  const [autoPlay, setAutoPlay] = useState(true);
  const [speed, setSpeed] = useState(1);
  const [resizeModeIdx, setResizeModeIdx] = useState(0);
  const resizeMode = RESIZE_MODES[resizeModeIdx];

  // --- Dynamic property overrides ----------------------------------------
  const [overridesOn, setOverridesOn] = useState(false);
  const [keyPath, setKeyPath] = useState('**');
  const [colorHex, setColorHex] = useState('#FF3B30');
  const colorOverrides: RlottieColorOverride[] | undefined = overridesOn
    ? [{keyPath, color: colorHex}]
    : undefined;
  const opacityOverrides: RlottieOpacityOverride[] | undefined = overridesOn
    ? [{keyPath, opacity: 0.5}]
    : undefined;
  const strokeWidthOverrides: RlottieStrokeWidthOverride[] | undefined =
    overridesOn ? [{keyPath, width: 8}] : undefined;

  // --- Metrics -------------------------------------------------------------
  const [metricsEnabled, setMetricsEnabled] = useState(false);
  const [metrics, setMetrics] = useState<RlottieMetricsEvent | undefined>();

  // --- Loaded info / markers ------------------------------------------------
  const [loadedInfo, setLoadedInfo] = useState<RlottieLoadedEvent | undefined>();
  const [markers, setMarkers] = useState<RlottieMarker[]>([]);

  // --- Global RlottieModule -------------------------------------------------
  const [nativeVersion, setNativeVersion] = useState<string>('(not queried)');
  const [cacheSizeInput, setCacheSizeInput] = useState('4');

  // Deliberately NOT SafeAreaView: RN 0.81 deprecates it (it warns at runtime)
  // and its replacement, react-native-safe-area-context, is a dependency this
  // example does not need. ScrollView's contentInsetAdjustmentBehavior handles
  // the iOS inset; Android needs the status-bar height added by hand.
  return (
    <View style={styles.safe}>
      <StatusBar barStyle="dark-content" />
      <ScrollView
        contentInsetAdjustmentBehavior="automatic"
        contentContainerStyle={[
          styles.scrollContent,
          Platform.OS === 'android' && {
            paddingTop: (StatusBar.currentHeight ?? 0) + 16,
          },
        ]}>
        <Text style={styles.title}>react-native-rlottie example</Text>
        <Text style={styles.subtitle}>Legacy Architecture · RN 0.81.0</Text>

        <Section title="Bundled asset:/// source (imperative controls target this one)">
          <View style={styles.playerBox}>
            <RlottieView
              ref={primaryRef}
              source={ASSET_SOURCE}
              autoPlay={autoPlay}
              loop={loop}
              speed={speed}
              resizeMode={resizeMode}
              metricsEnabled={metricsEnabled}
              colorOverrides={colorOverrides}
              opacityOverrides={opacityOverrides}
              strokeWidthOverrides={strokeWidthOverrides}
              style={styles.player}
              onAnimationLoaded={e => {
                setLoadedInfo(e.nativeEvent);
                setMarkers(e.nativeEvent.markers);
                appendLog(
                  `onAnimationLoaded ${e.nativeEvent.width}x${e.nativeEvent.height} ` +
                    `${e.nativeEvent.totalFrames}f @ ${e.nativeEvent.frameRate}fps`,
                );
              }}
              onAnimationError={(e: {nativeEvent: RlottieErrorEvent}) =>
                appendLog(`onAnimationError ${e.nativeEvent.code}: ${e.nativeEvent.message}`)
              }
              onAnimationStart={() => appendLog('onAnimationStart')}
              onAnimationPause={() => appendLog('onAnimationPause')}
              onAnimationLoop={() => appendLog('onAnimationLoop')}
              onAnimationFinish={() => appendLog('onAnimationFinish')}
              onMetrics={e => setMetrics(e.nativeEvent)}
            />
          </View>
          {loadedInfo && (
            <Text style={styles.meta}>
              {loadedInfo.width}x{loadedInfo.height} · {loadedInfo.totalFrames} frames ·{' '}
              {loadedInfo.frameRate}fps · {loadedInfo.duration.toFixed(2)}s
              {markers.length > 0 ? ` · markers: ${markers.map(m => m.name).join(', ')}` : ''}
            </Text>
          )}
        </Section>

        <Section title="Inline JSON object source (source={{json: <object>}})">
          <View style={styles.playerBox}>
            <RlottieView
              source={INLINE_JSON_SOURCE}
              autoPlay
              loop
              style={styles.player}
              onAnimationError={(e: {nativeEvent: RlottieErrorEvent}) =>
                appendLog(`(inline) onAnimationError ${e.nativeEvent.code}`)
              }
            />
          </View>
        </Section>

        <Section title="Imperative commands">
          <Row>
            <Btn label="Play" onPress={() => primaryRef.current?.play()} />
            <Btn label="Pause" onPress={() => primaryRef.current?.pause()} />
            <Btn label="Resume" onPress={() => primaryRef.current?.resume()} />
            <Btn label="Stop" onPress={() => primaryRef.current?.stop()} />
            <Btn label="Reset" onPress={() => primaryRef.current?.reset()} />
          </Row>
          <Row>
            <Btn
              label="Seek 25%"
              onPress={() => primaryRef.current?.seekToProgress(0.25)}
            />
            <Btn
              label="Seek 50%"
              onPress={() => primaryRef.current?.seekToProgress(0.5)}
            />
            <Btn
              label="Seek 75%"
              onPress={() => primaryRef.current?.seekToProgress(0.75)}
            />
            <Btn
              label="Seek frame 10"
              onPress={() => primaryRef.current?.seekToFrame(10)}
            />
          </Row>
          <Row>
            <Btn
              label="Play range 10..40"
              onPress={() => primaryRef.current?.play({startFrame: 10, endFrame: 40})}
            />
          </Row>
          {markers.length > 0 ? (
            <Row>
              {markers.map(m => (
                <Btn
                  key={m.name}
                  label={`Play marker "${m.name}"`}
                  onPress={() => primaryRef.current?.play({marker: m.name})}
                />
              ))}
            </Row>
          ) : (
            <Text style={styles.meta}>
              (playMarker buttons appear here once the source reports markers via
              onAnimationLoaded)
            </Text>
          )}
        </Section>

        <Section title="Playback-shaping props">
          <Row>
            <Text style={styles.label}>autoPlay</Text>
            <Switch value={autoPlay} onValueChange={setAutoPlay} />
            <Text style={styles.label}>loop</Text>
            <Switch value={loop} onValueChange={setLoop} />
          </Row>
          <Row>
            <Btn
              label="setSpeed 0.5x"
              onPress={() => {
                setSpeed(0.5);
                primaryRef.current?.setSpeed(0.5);
              }}
            />
            <Btn
              label="setSpeed 1x"
              onPress={() => {
                setSpeed(1);
                primaryRef.current?.setSpeed(1);
              }}
            />
            <Btn
              label="setSpeed 2x"
              onPress={() => {
                setSpeed(2);
                primaryRef.current?.setSpeed(2);
              }}
            />
            <Btn
              label="setSpeed -1x (reverse)"
              onPress={() => {
                setSpeed(-1);
                primaryRef.current?.setSpeed(-1);
              }}
            />
          </Row>
          <Row>
            <Btn
              label={`resizeMode: ${resizeMode} (tap to cycle)`}
              onPress={() => setResizeModeIdx(i => (i + 1) % RESIZE_MODES.length)}
            />
          </Row>
        </Section>

        <Section title="Dynamic property overrides (colorOverrides / opacityOverrides / strokeWidthOverrides)">
          <Row>
            <Text style={styles.label}>enabled</Text>
            <Switch value={overridesOn} onValueChange={setOverridesOn} />
          </Row>
          <Row>
            <TextInput
              style={styles.input}
              value={keyPath}
              onChangeText={setKeyPath}
              placeholder="rlottie keyPath, e.g. **"
            />
            <TextInput
              style={styles.input}
              value={colorHex}
              onChangeText={setColorHex}
              placeholder="#RRGGBB"
            />
          </Row>
          <Text style={styles.meta}>
            Applies {'{keyPath, color}'} / {'{keyPath, opacity: 0.5}'} /{' '}
            {'{keyPath, width: 8}'} to the bundled-asset view above while enabled. A
            keyPath that doesn't match any layer is a documented no-op, not an error.
          </Text>
        </Section>

        <Section title="Metrics (onMetrics, throttled to <=1/sec while metricsEnabled)">
          <Row>
            <Text style={styles.label}>metricsEnabled</Text>
            <Switch value={metricsEnabled} onValueChange={setMetricsEnabled} />
          </Row>
          {metrics ? (
            <Text style={styles.mono}>
              {`parseMs=${metrics.parseMs.toFixed(2)} firstFrameMs=${metrics.firstFrameMs.toFixed(2)}\n`}
              {`renderP50/95/99=${metrics.renderP50Ms.toFixed(2)}/${metrics.renderP95Ms.toFixed(2)}/${metrics.renderP99Ms.toFixed(2)} ms\n`}
              {`framesRendered=${metrics.framesRendered} framesDropped=${metrics.framesDropped}\n`}
              {`bufferAllocCount=${metrics.bufferAllocCount} peakBufferBytes=${metrics.peakBufferBytes}\n`}
              {`uiStallCount=${metrics.uiStallCount} uiStallMaxMs=${metrics.uiStallMaxMs.toFixed(2)}`}
            </Text>
          ) : (
            <Text style={styles.meta}>(no onMetrics payload received yet)</Text>
          )}
        </Section>

        <Section title="Global RlottieModule">
          <Row>
            <Btn
              label="getNativeVersion()"
              onPress={async () => {
                try {
                  const v = await Rlottie.getNativeVersion();
                  setNativeVersion(
                    `rlottieCommit=${v.rlottieCommit.slice(0, 12)} modelCacheSize=${v.modelCacheSize}`,
                  );
                } catch (err) {
                  setNativeVersion(`error: ${String(err)}`);
                }
              }}
            />
            <Btn
              label="clearModelCache()"
              onPress={async () => {
                try {
                  await Rlottie.clearModelCache();
                  appendLog('RlottieModule.clearModelCache() ok');
                } catch (err) {
                  Alert.alert('clearModelCache failed', String(err));
                }
              }}
            />
          </Row>
          <Row>
            <TextInput
              style={styles.input}
              value={cacheSizeInput}
              onChangeText={setCacheSizeInput}
              keyboardType="number-pad"
            />
            <Btn
              label="configure({modelCacheSize})"
              onPress={async () => {
                const n = parseInt(cacheSizeInput, 10);
                try {
                  await Rlottie.configure({modelCacheSize: Number.isFinite(n) ? n : 0});
                  appendLog(`RlottieModule.configure({modelCacheSize: ${n}}) ok`);
                } catch (err) {
                  Alert.alert('configure failed', String(err));
                }
              }}
            />
          </Row>
          <Text style={styles.meta}>{nativeVersion}</Text>
          <Text style={styles.meta}>isAvailable(): {String(Rlottie.isAvailable())}</Text>
        </Section>

        <Section title="Event log (most recent first)">
          {log.length === 0 ? (
            <Text style={styles.meta}>(no events yet)</Text>
          ) : (
            log.map((line, i) => (
              <Text key={i} style={styles.mono}>
                {line}
              </Text>
            ))
          )}
        </Section>
      </ScrollView>
    </View>
  );
}

const styles = StyleSheet.create({
  safe: {flex: 1, backgroundColor: '#fff'},
  scrollContent: {padding: 16, paddingBottom: 48},
  title: {fontSize: 20, fontWeight: '700'},
  subtitle: {fontSize: 13, color: '#666', marginBottom: 12},
  section: {
    marginTop: 16,
    paddingTop: 12,
    borderTopWidth: StyleSheet.hairlineWidth,
    borderTopColor: '#ccc',
  },
  sectionTitle: {fontSize: 14, fontWeight: '600', marginBottom: 8},
  playerBox: {
    width: '100%',
    height: 220,
    backgroundColor: '#f2f2f2',
    borderRadius: 8,
    overflow: 'hidden',
  },
  player: {width: '100%', height: '100%'},
  meta: {fontSize: 12, color: '#555', marginTop: 6},
  mono: {fontSize: 11, color: '#333', fontFamily: 'Courier'},
  row: {flexDirection: 'row', flexWrap: 'wrap', alignItems: 'center', marginTop: 8, gap: 8},
  label: {fontSize: 13, marginRight: 4},
  btn: {
    backgroundColor: '#2f6feb',
    paddingHorizontal: 10,
    paddingVertical: 8,
    borderRadius: 6,
    marginRight: 6,
    marginBottom: 6,
  },
  btnDisabled: {opacity: 0.4},
  btnText: {color: '#fff', fontSize: 12, fontWeight: '600'},
  input: {
    borderWidth: StyleSheet.hairlineWidth,
    borderColor: '#999',
    borderRadius: 6,
    paddingHorizontal: 8,
    paddingVertical: 6,
    minWidth: 90,
    fontSize: 12,
    marginRight: 6,
    marginBottom: 6,
  },
});
