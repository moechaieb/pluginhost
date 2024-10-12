#pragma once
#include "PluginHost.h"
#include "PluginWindowLookAndFeel.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

/**
    A desktop window containing a plugin's GUI.
*/
namespace timeoffaudio {
    class PluginWindow final : public juce::DocumentWindow {
    public:
        enum class Type { normal = 0, generic };

        struct Options {
            int xPos, yPos = 0;
            bool openAutomatically                      = true;
            std::string titlePrefix                     = JucePlugin_Name;
            std::optional<std::string> backgroundHexRGB = "ff000000", textColourHexRGB = "ffffffff";
        };

        static const Options DEFAULT_OPTIONS;

        enum class UpdateType { None = 0, Opened, Closed };

        explicit PluginWindow (const std::string& key,
            juce::AudioPluginInstance& pI,
            const Type t           = Type::normal,
            const Options& options = DEFAULT_OPTIONS)
            : pluginInstanceKey (key),
              DocumentWindow (juce::String (options.titlePrefix) + ": " + pI.getPluginDescription().name.toLowerCase(),
                  juce::Colours::black,
                  DocumentWindow::closeButton),
              pluginInstance (pI),
              type (t) {
            jassert (!key.empty());
            jassert (!pluginInstanceKey.empty());

            if (options.textColourHexRGB.has_value()) {
                pluginWindowLookAndFeel.setTitleBarTextColour (
                    juce::Colour::fromString (options.textColourHexRGB.value()));
            }

            if (options.backgroundHexRGB.has_value()) {
                pluginWindowLookAndFeel.setTitleBarBackgroundColour (
                    juce::Colour::fromString (options.backgroundHexRGB.value()));
            }

            if (auto* ui = createProcessorEditor (pluginInstance, type)) {
                setContentOwned (ui, true);
                setResizable (ui->isResizable(), false);
            }

            setConstrainer (&constrainer);
            setTopLeftPosition (options.xPos, options.yPos);

            setLookAndFeel (&pluginWindowLookAndFeel);
            if (currentDawRequiresPluginWindowsInFront()) setAlwaysOnTop (true);

            Component::setVisible (true);
        }

        ~PluginWindow() override {
            setLookAndFeel (nullptr);
            clearContentComponent();
        }

        void closeButtonPressed() override { setVisible (false); }

        void setWindowTitlePrefix (const std::string& newPrefix) {
            if (newPrefix.empty())
                setTitle (
                    juce::String (JucePlugin_Name) + ": " + pluginInstance.getPluginDescription().name.toLowerCase());
            else
                setTitle (juce::String (newPrefix) + ": " + pluginInstance.getPluginDescription().name.toLowerCase());
        }

        std::string getPluginInstanceKey() const { return pluginInstanceKey; }
        void setPluginInstanceKey (const std::string& newPluginInstanceKey) {
            pluginInstanceKey = newPluginInstanceKey;
        }

    private:
        std::string pluginInstanceKey;
        juce::AudioPluginInstance& pluginInstance;
        const Type type;
        timeoffaudio::PluginWindowLookAndFeel pluginWindowLookAndFeel;
        const juce::PluginHostType currentDAW;

        class DecoratorConstrainer final : public juce::BorderedComponentBoundsConstrainer {
        public:
            explicit DecoratorConstrainer (DocumentWindow& windowIn) : window (windowIn) {}

            juce::ComponentBoundsConstrainer* getWrappedConstrainer() const override {
                auto* editor = dynamic_cast<juce::AudioProcessorEditor*> (window.getContentComponent());
                return editor != nullptr ? editor->getConstrainer() : nullptr;
            }

            juce::BorderSize<int> getAdditionalBorder() const override {
                const auto nativeFrame = [&]() -> juce::BorderSize<int> {
                    if (const auto* peer = window.getPeer())
                        if (const auto frameSize = peer->getFrameSizeIfPresent()) return *frameSize;

                    return {};
                }();

                return nativeFrame.addedTo (window.getContentComponentBorder());
            }

        private:
            DocumentWindow& window;
        };

        DecoratorConstrainer constrainer { *this };

        float getDesktopScaleFactor() const override { return 1.0f; }

        static juce::AudioProcessorEditor* createProcessorEditor (juce::AudioPluginInstance& pluginInstance,
            PluginWindow::Type type) {
            if (type == PluginWindow::Type::normal) {
                if (pluginInstance.hasEditor())
                    if (auto* ui = pluginInstance.createEditorIfNeeded()) return ui;

                type = PluginWindow::Type::generic;
            }

            if (type == PluginWindow::Type::generic) {
                auto* result = new juce::GenericAudioProcessorEditor (pluginInstance);
                result->setResizeLimits (200, 300, 1'000, 10'000);
                return result;
            }

            jassertfalse;
            return {};
        }

        static std::string getTypeName (Type type) {
            switch (type) {
                case Type::normal:
                    return "Normal";
                case Type::generic:
                    return "Generic";
                default:
                    return {};
            }
        }

        /*
         * Given different DAWs handle plugin windows differently, we need to conditionally set hosted plugin windows
         * to be "always in front" (i.e. call setAlwaysInFront(true).
         *
         * The desired behaviour is that hosted plugin windows and the pluginhost's window (whether a standalone app
         * or a plugin itself) always come to the front when focused, and don't hide each other even when in focus.
         *
         * This helper method ensures we can achieve that on all hosts.
         */
        bool currentDawRequiresPluginWindowsInFront() const {
            if (currentDAW.getPluginLoadedAs() == juce::AudioProcessor::wrapperType_Standalone) return false;
            if (currentDAW.isJUCEPluginHost()) return false;

            return true;
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginWindow)
    };

}
