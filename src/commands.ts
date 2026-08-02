// Chunk 4.2 — imperative command dispatch.
//
// SIGNATURE FROZEN: Chunk 4.3 (`RlottieView.tsx`) is written against the
// exports below, so their names and types must not change without updating that
// file in the same commit. The BODIES marked TODO(4.2) are Chunk 4.2's to
// implement.
//
// This module is the ONLY place `findNodeHandle` and
// `UIManager.dispatchViewManagerCommand` may appear (CLAUDE.md layering rule:
// consumers never touch platform methods).

import type {Component} from 'react';
import {UIManager, findNodeHandle} from 'react-native';

import {RLOTTIE_COMPONENT_NAME} from './RlottieNativeComponent';

/**
 * Stable numeric ids from `docs/bridge-contract.md`. Never renumber: released
 * JS bundles carry these values.
 *
 * Prefer dispatching by NAME. On iOS an integer id is a raw array index into
 * the view manager's exported-method list, and an out-of-range value raises
 * NSRangeException instead of failing gracefully — see the "iOS numeric-id
 * fragility" section of the contract doc.
 */
export enum RlottieCommand {
  Play = 1,
  Pause = 2,
  Resume = 3,
  Stop = 4,
  Reset = 5,
  SeekToProgress = 6,
  SeekToFrame = 7,
  SetSpeed = 8,
}

export type RlottieCommandName =
  | 'play'
  | 'pause'
  | 'resume'
  | 'stop'
  | 'reset'
  | 'seekToProgress'
  | 'seekToFrame'
  | 'setSpeed';

/** Maps each name to its stable numeric id, for the integer dispatch path. */
export const RLOTTIE_COMMAND_IDS: Record<RlottieCommandName, RlottieCommand> = {
  play: RlottieCommand.Play,
  pause: RlottieCommand.Pause,
  resume: RlottieCommand.Resume,
  stop: RlottieCommand.Stop,
  reset: RlottieCommand.Reset,
  seekToProgress: RlottieCommand.SeekToProgress,
  seekToFrame: RlottieCommand.SeekToFrame,
  setSpeed: RlottieCommand.SetSpeed,
};

/** Anything with a native tag — what `useRef` on the host component yields. */
export type RlottieViewHandle = Component | number | null | undefined;

/**
 * Resolves whatever a consumer's `useRef` produced into a native tag, without
 * ever throwing.
 *
 * `findNodeHandle` is a thin wrapper around the renderer's internal instance
 * map; passing it something that isn't a mounted host/composite instance (a
 * stale ref after unmount, a ref object instead of `.current`, etc.) is a
 * documented no-op that returns `null` on every RN version we support, but we
 * still wrap it defensively — this function's entire contract is "never
 * throw", and a private renderer implementation detail is not something to
 * trust blindly across the 0.68–0.81 range.
 */
function resolveNativeTag(view: RlottieViewHandle): number | null {
  if (view == null) {
    return null;
  }
  if (typeof view === 'number') {
    return Number.isFinite(view) ? view : null;
  }
  try {
    const tag = findNodeHandle(view);
    return typeof tag === 'number' ? tag : null;
  } catch {
    return null;
  }
}

/**
 * Resolves a command name to whatever `UIManager.dispatchViewManagerCommand`
 * should actually receive.
 *
 * Why not just hardcode `RLOTTIE_COMMAND_IDS[command]`? Per
 * `docs/bridge-contract.md` ("iOS numeric-id fragility"), RN's legacy iOS
 * bridge resolves an integer command id as `moduleData.methods[commandID]` —
 * a raw array index into `RNRlottieViewManager`'s `RCT_EXPORT_METHOD`s in
 * declaration order (see `RCTUIManager.mm dispatchViewManagerCommand:...`,
 * verified against the vendored RN 0.81 source at
 * `node_modules/react-native/React/Modules/RCTUIManager.mm`). A hardcoded
 * literal that drifts from the native declaration order (a reordered or added
 * `RCT_EXPORT_METHOD`) turns into an out-of-range index and an
 * `NSRangeException` that crashes the whole app — not a recoverable JS error.
 *
 * The robust alternative RN itself provides is
 * `UIManager.getViewManagerConfig('RlottieView').Commands`, which
 * `PaperUIManager.js` computes *dynamically* by walking the manager's actual
 * exported methods at runtime (`lazifyViewManagerConfig`) — so it can never
 * disagree with the native declaration order, unlike a value baked into a JS
 * bundle at build time. We prefer that id when it is available.
 *
 * When it is not (the config hasn't been fetched yet on this RN version, or
 * `getViewManagerConfig` throws for some host environment), fall back to
 * passing the command NAME straight through as a string. This also works
 * end-to-end on the legacy bridge: `RCTUIManager.mm` resolves a string
 * command id via `moduleData.methodsByName[commandID]` (keyed by the
 * `RCT_EXPORT_METHOD`'s JS-exported name, i.e. exactly the names in the
 * contract's command table) instead of an index, so it cannot go
 * out-of-range; and `docs/bridge-contract.md` requires both platforms'
 * `receiveCommand` to accept the string name directly. Unlike the numeric
 * fallback, a string that doesn't match anything just fails to resolve a
 * method (Android/iOS both no-op or log) — it never indexes out of bounds.
 *
 * `RLOTTIE_COMMAND_IDS` is intentionally NOT used here — it exists for
 * consumers/tests that want the stable contractual id, not as this module's
 * dispatch mechanism, precisely to avoid reintroducing the hardcoded-integer
 * fragility this function exists to route around.
 */
function resolveCommandId(command: RlottieCommandName): number | string {
  try {
    const config = UIManager.getViewManagerConfig?.(RLOTTIE_COMPONENT_NAME);
    const dynamicId = config?.Commands?.[command];
    if (typeof dynamicId === 'number') {
      return dynamicId;
    }
  } catch {
    // Fall through to the string path below.
  }
  return command;
}

/**
 * Sends one command to the native view.
 *
 * Returns `false` (and does nothing) when the view is not mounted yet, rather
 * than throwing: the contract says a command arriving before there is a live
 * native handle is dropped silently, and a consumer calling `ref.play()` in an
 * effect that races mount must not crash the app.
 *
 * This function (together with `resolveNativeTag`/`resolveCommandId` above)
 * is the only place `findNodeHandle` and
 * `UIManager.dispatchViewManagerCommand` may appear in this package.
 */
export function dispatchRlottieCommand(
  view: RlottieViewHandle,
  command: RlottieCommandName,
  args: readonly unknown[] = [],
): boolean {
  const tag = resolveNativeTag(view);
  if (tag == null) {
    return false;
  }

  const commandId = resolveCommandId(command);
  const safeArgs = Array.isArray(args) ? Array.from(args) : [];

  try {
    UIManager.dispatchViewManagerCommand(tag, commandId, safeArgs);
    return true;
  } catch {
    // Never throw out of an imperative ref call — see the function doc above.
    return false;
  }
}
