#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
/**
    A desktop window containing a plugin's GUI.
*/
namespace timeoffaudio {
    class PluginWindow final : public juce::DocumentWindow {
    public:
        enum class Type { normal = 0, generic, debug, numTypes };

        struct Options {
            int xPos = 0;
            int yPos = 0;
            bool openAutomatically = true;
            std::string titlePrefix = JucePlugin_Name;
        };

        explicit PluginWindow (juce::AudioPluginInstance& pI,
            const int xPos = 100,
            const int yPos = 100, Type t = Type::normal, const std::string& windowTitlePrefix = JucePlugin_Name)
            : DocumentWindow (juce::String(windowTitlePrefix) + ": " + pI.getPluginDescription().name.toLowerCase(),
                  juce::Colours::black,
                  DocumentWindow::closeButton),
              pluginInstance (pI),
              type (t) {
            if (auto* ui = createProcessorEditor (pluginInstance, type)) {
                setContentOwned (ui, true);
                setResizable (ui->isResizable(), false);
            }
            setConstrainer (&constrainer);

            setTopLeftPosition (xPos, yPos);
            setAlwaysOnTop (true);
            Component::setVisible (true);
        }

        ~PluginWindow() override = default;

        void closeButtonPressed() override { setVisible (false); }

        static std::string getLastXProp (Type type) { return "uiLastX_" + getTypeName (type); }
        static std::string getLastYProp (Type type) { return "uiLastY_" + getTypeName (type); }
        static std::string getOpenProp (Type type) { return "uiopen_" + getTypeName (type); }

        void setWindowTitlePrefix (std::string newPrefix) {
            if(newPrefix.empty())
                setTitle (juce::String(JucePlugin_Name) + ": " + pluginInstance.getPluginDescription().name.toLowerCase());
            else
                setTitle (juce::String(newPrefix) + ": " + pluginInstance.getPluginDescription().name.toLowerCase());
        }

    private:
        juce::AudioPluginInstance& pluginInstance;
        const Type type;

        class DecoratorConstrainer final : public juce::BorderedComponentBoundsConstrainer {
        public:
            explicit DecoratorConstrainer (DocumentWindow& windowIn) : window (windowIn) {}

            juce::ComponentBoundsConstrainer* getWrappedConstrainer() const override {
                auto* editor = dynamic_cast<juce::AudioProcessorEditor*> (window.getContentComponent());
                return editor != nullptr ? editor->getConstrainer() : nullptr;
            }

            juce::BorderSize<int> getAdditionalBorder() const override {
                const auto nativeFrame = [&]() -> juce::BorderSize<int> {
                    if (auto* peer = window.getPeer())
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

            // if (type == PluginWindow::Type::debug) return new PluginDebugWindow (processor);

            jassertfalse;
            return {};
        }

        static std::string getTypeName (Type type) {
            switch (type) {
                case Type::normal:
                    return "Normal";
                case Type::generic:
                    return "Generic";
                case Type::debug:
                    return "Debug";
                case Type::numTypes:
                default:
                    return {};
            }
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginWindow)
    };

}
