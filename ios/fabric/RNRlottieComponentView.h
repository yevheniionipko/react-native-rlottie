// Chunk 9.8 — the Fabric component view. See docs/new-architecture-design.md
// §3.6 for the ownership rationale (composes RNRlottieView rather than
// reimplementing it) and the full contract this class implements.
#ifdef RCT_NEW_ARCH_ENABLED

#import <React/RCTViewComponentView.h>
#import <react/renderer/components/RNRlottieSpec/RCTComponentViewHelpers.h>

NS_ASSUME_NONNULL_BEGIN

@interface RNRlottieComponentView : RCTViewComponentView <RCTRlottieViewViewProtocol>
@end

NS_ASSUME_NONNULL_END

#endif // RCT_NEW_ARCH_ENABLED
