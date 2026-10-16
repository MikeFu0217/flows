#pragma once
#include "FlowsProcessor.hpp"
#include <juce_gui_extra/juce_gui_extra.h>

namespace flows::app {
class FlowsEditor final : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit FlowsEditor(FlowsProcessor&);
    ~FlowsEditor() override;
    void resized() override;
private:
    FlowsProcessor& processor_;
    std::unique_ptr<juce::WebBrowserComponent> browser_;
    std::unique_ptr<juce::FileChooser> chooser_;
    std::uint64_t timerCount_ = 0;
    juce::String previousState_;
    void timerCallback() override;
    void handle(const juce::Array<juce::var>&,juce::WebBrowserComponent::NativeFunctionCompletion);
};
}
