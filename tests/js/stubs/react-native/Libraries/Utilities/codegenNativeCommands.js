// Stub for react-native/Libraries/Utilities/codegenNativeCommands.js (Flow
// source, not requireable directly under plain node — see the note in
// tests/js/stubs/react-native/index.js).
//
// Real behaviour this mirrors: builds one function per supported command that
// forwards to `RendererProxy.dispatchCommand(ref, command, args)`, which
// requires a live mounted host instance and throws otherwise
// (docs/new-architecture-design.md §3.3). Calls are recorded on the shared
// `__newArchDispatchedCalls` array so tests/js/commands.test.js can assert on
// them the same way it does on the Legacy `__dispatchedCalls`.
const rn = require('../../index.js');

function codegenNativeCommands(options) {
  const commandObj = {};
  options.supportedCommands.forEach(command => {
    commandObj[command] = (ref, ...args) => {
      // `__unmounted` is a test-only escape hatch, mirroring a stale ref
      // racing unmount — the real case `dispatchCommand` throws on.
      if (ref == null || ref.__unmounted === true) {
        throw new Error(
          `dispatchCommand: "${command}" called on an unmounted/stale ref`,
        );
      }
      rn.__newArchDispatchedCalls.push({ref, command, args});
    };
  });
  return commandObj;
}

module.exports = codegenNativeCommands;
