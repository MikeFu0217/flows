#pragma once

namespace juce { class Component; }

namespace flows::app {
// Message thread only, after the standalone editor has a native peer and WKWebView.
// Window and editor must be made non-opaque before attaching the WebView. This
// function never changes the JUCE peer's lifetime or its component hierarchy.
// Returns false before attachment, in plugin/foreign windows, or if WebKit refuses
// transparency. Callers may retry after attachment; no other application's UI is touched.
// CSS must leave html/body transparent and apply alpha only to the desired surfaces.
bool prepareTransparentWindow(juce::Component& editor) noexcept;
}
