#include "FlowsProcessor.hpp"
#include <juce_gui_extra/juce_gui_extra.h>

namespace flows::app {
class MainWindow final : public juce::DocumentWindow {
public:
    FlowsProcessor processor{true};
    juce::AudioDeviceManager devices;
    juce::AudioProcessorPlayer player;
    // Establish the peer's alpha mode before attaching the native WebView.
    // Changing Component::setOpaque afterwards recreates the macOS window.
    MainWindow() : DocumentWindow(FLOWS_STANDALONE_NAME,juce::Colours::transparentBlack,allButtons) {
        const auto workspace=juce::SystemStats::getEnvironmentVariable("FLOWS_WORKSPACE",{});
        if(workspace.isNotEmpty()) setName(juce::String(FLOWS_STANDALONE_NAME)+" - "+juce::File(workspace).getFileName());
        setUsingNativeTitleBar(true); setResizable(true,false);
        setContentOwned(processor.createEditor(),true); centreWithSize(1440,920); setVisible(true);
        auto deviceError = devices.initialise(0,2,nullptr,true);
        player.setProcessor(&processor); devices.addAudioCallback(&player);
        for (const auto& input : juce::MidiInput::getAvailableDevices()) devices.setMidiInputDeviceEnabled(input.identifier,true);
        devices.addMidiInputDeviceCallback({},&player);
        processor.showAudioSettings = [this] {
            auto* content = new juce::AudioDeviceSelectorComponent(devices,0,0,1,2,true,false,true,false);
            content->setSize(580,480); juce::DialogWindow::LaunchOptions options;
            options.content.setOwned(content); options.dialogTitle = "Audio & MIDI";
            options.dialogBackgroundColour = juce::Colour(0xff202326); options.escapeKeyTriggersCloseButton = true;
            options.useNativeTitleBar = true; options.resizable = false; options.launchAsync();
        };
        if (deviceError.isNotEmpty()) juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,"Audio device",deviceError);
    }
    ~MainWindow() override {
        processor.showAudioSettings = {}; devices.removeMidiInputDeviceCallback({},&player);
        devices.removeAudioCallback(&player); player.setProcessor(nullptr); clearContentComponent();
    }
    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
};
class Application final : public juce::JUCEApplication {
    std::unique_ptr<MainWindow> window;
public:
    const juce::String getApplicationName() override { return FLOWS_STANDALONE_NAME; }
    const juce::String getApplicationVersion() override { return JucePlugin_VersionString; }
    bool moreThanOneInstanceAllowed() override { return juce::SystemStats::getEnvironmentVariable("FLOWS_ALLOW_MULTIPLE",{}) == "1"; }
    void initialise(const juce::String&) override { window = std::make_unique<MainWindow>(); }
    void shutdown() override { window.reset(); }
    void systemRequestedQuit() override { quit(); }
    void anotherInstanceStarted(const juce::String&) override { if (window) window->toFront(true); }
};
}
START_JUCE_APPLICATION(flows::app::Application)
