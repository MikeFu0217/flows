#include "MacAppearance.hpp"
#include <juce_gui_basics/juce_gui_basics.h>

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#import <WebKit/WebKit.h>

namespace flows::app {
namespace {
WKWebView* findWebView(NSView* view, unsigned int depth = 0) {
    if (view == nil || depth > 64) return nil;
    if ([view isKindOfClass:[WKWebView class]]) return static_cast<WKWebView*>(view);
    for (NSView* child in view.subviews)
        if (auto* web = findWebView(child, depth + 1)) return web;
    return nil;
}
}

bool prepareTransparentWindow(juce::Component& editor) noexcept {
    auto* messages = juce::MessageManager::getInstanceWithoutCreating();
    if (messages == nullptr || !messages->isThisTheMessageThread() || ![NSThread isMainThread] ||
        !juce::JUCEApplicationBase::isStandaloneApp()) return false;

    // The shared editor also ships inside AU/VST3. A JUCE-based host may itself
    // be a standalone JUCE application, so the process bundle is a second guard.
    if (![[NSBundle mainBundle].bundleIdentifier isEqualToString:@"audio.flows.standalone"]) return false;
    auto* top = editor.getTopLevelComponent();
    auto* document = dynamic_cast<juce::DocumentWindow*>(top);
    if (document == nullptr || dynamic_cast<juce::DialogWindow*>(top) != nullptr) return false;
    // Alpha mode belongs to window construction. Changing it here would rebuild
    // the JUCE peer, invalidate native handles and detach the loaded WebView.
    if (document->isOpaque() || editor.isOpaque()) return false;
    auto* peer = top->getPeer();
    if (peer == nullptr || editor.getPeer() != peer) return false;

    try {
        @autoreleasepool {
            @try {
                auto* view = (__bridge NSView*) peer->getNativeHandle();
                if (view == nil || ![view isKindOfClass:[NSView class]]) return false;
                auto* window = view.window;
                if (window == nil || [window isKindOfClass:[NSPanel class]]) return false;
                if (view != window.contentView && ![view isDescendantOf:window.contentView]) return false;
                auto* web = findWebView(view);
                if (web == nil || web.window != window) return false;

                // Public API controls the under-page/scroll-bounce background.
                // https://developer.apple.com/documentation/webkit/wkwebview/underpagebackgroundcolor
                if (@available(macOS 12.0, *)) web.underPageBackgroundColor = NSColor.clearColor;

                // WebKit still has no public macOS equivalent of the old WebView
                // drawsBackground setter. underPageBackgroundColor alone does not
                // disable the page's native opaque backing. Keep this compatibility
                // operation isolated and exception-guarded; a future removal simply
                // leaves the standalone window opaque and reports failure.
                // https://bugs.webkit.org/show_bug.cgi?id=221663
                [web setValue:@NO forKey:@"drawsBackground"];
                id drawsBackground = [web valueForKey:@"drawsBackground"];
                if (![drawsBackground isKindOfClass:[NSNumber class]] || [drawsBackground boolValue]) return false;

                // The JUCE peer is already transparent. Only configure its native
                // backing; never recreate it after acquiring these native handles.
                // Do not lower NSWindow.alphaValue: that would fade text and controls.
                if (view.layer != nil) {
                    view.layer.opaque = NO;
                    view.layer.backgroundColor = NSColor.clearColor.CGColor;
                }
                window.opaque = NO;
                window.backgroundColor = NSColor.clearColor;
                [window invalidateShadow];
                [view setNeedsDisplay:YES];
                editor.repaint();
                return !window.opaque && window.backgroundColor.alphaComponent == 0.0;
            } @catch (NSException*) {
                return false;
            }
        }
    } catch (...) {
        return false;
    }
}
}
