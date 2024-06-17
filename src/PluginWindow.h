#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
/**
    A desktop window containing a plugin's GUI.
*/
namespace timeoffaudio {
    class PluginWindow final : public juce::DocumentWindow {
    public:
        enum class Type { normal = 0, generic, debug, araHost, numTypes };

        PluginWindow (juce::AudioPluginInstance& pI, int xPos = 100, int yPos = 100, Type t = Type::normal)
            : DocumentWindow ("plugin window",
                juce::Colours::black,
                DocumentWindow::minimiseButton | DocumentWindow::closeButton),
              pluginInstance (pI),
              type (t) {
            setSize (400, 300);

            if (auto* ui = createProcessorEditor (pluginInstance, type)) {
                setContentOwned (ui, true);
                setResizable (ui->isResizable(), false);
            }
            setConstrainer (&constrainer);

            setTopLeftPosition (xPos, yPos);
            setAlwaysOnTop(true);
            setVisible (true);
        }

        ~PluginWindow() override { clearContentComponent(); }

        void closeButtonPressed() override {
            setVisible (false);
        }

        static std::string getLastXProp (Type type) { return "uiLastX_" + getTypeName (type); }
        static std::string getLastYProp (Type type) { return "uiLastY_" + getTypeName (type); }
        static std::string getOpenProp (Type type) { return "uiopen_" + getTypeName (type); }

        juce::BorderSize<int> getBorderThickness() override { return DocumentWindow::getBorderThickness(); }

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

            if (type == PluginWindow::Type::araHost) {
#if JUCE_PLUGINHOST_ARA && (JUCE_MAC || JUCE_WINDOWS || JUCE_LINUX)
                if (auto* araPluginInstanceWrapper = dynamic_cast<ARAPluginInstanceWrapper*> (&processor))
                    if (auto* ui = araPluginInstanceWrapper->createARAHostEditor()) return ui;
#endif
                return {};
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
                case Type::araHost:
                    return "ARAHost";
                case Type::numTypes:
                default:
                    return {};
            }
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginWindow)
    };

}
