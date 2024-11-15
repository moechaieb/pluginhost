#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

namespace timeoffaudio {

    constexpr const char* PROCESS_UID = "pluginScanner";
    constexpr const char* PROCESS_NAME = "time off audio plugin scanner";

    class CustomPluginScanner final : public juce::KnownPluginList::CustomScanner {
    public:
        class SubprocessCoordinator final : private juce::ChildProcessCoordinator {
        public:
            SubprocessCoordinator() {
                // Firstly, look for the plugin scanner in the common application data directory
                auto pluginScannerLocation = juce::File::getSpecialLocation (juce::File::commonApplicationDataDirectory)
#if JUCE_MAC
                                                 .getChildFile ("Application Support")
#endif
                                                 .getChildFile (JucePlugin_Manufacturer)
                                                 .getChildFile (PROCESS_NAME);

                // If it doesn't exist, look in the user application data directory
                if (!pluginScannerLocation.existsAsFile()) {
                    pluginScannerLocation = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
#if JUCE_MAC
                                                .getChildFile ("Application Support")
#endif
                                                .getChildFile (JucePlugin_Manufacturer)
                                                .getChildFile (PROCESS_NAME);
                }

                if (!launchWorkerProcess (pluginScannerLocation.getFullPathName(), PROCESS_UID, 0, 0)) {
                    // TODO: what should I do here if this ever happens? Ideally fallback to in-process scanning.
                }
            }

            enum class State { timeout, gotResult, connectionLost };
            struct Response {
                State state;
                std::unique_ptr<juce::XmlElement> xml;
            };

            Response getResponse() {
                std::unique_lock<std::mutex> lock { mutex };

                if (!condvar.wait_for (
                        lock, std::chrono::milliseconds { 200 }, [&] { return gotResult || connectionLost; }))
                    return { State::timeout, nullptr };

                const auto state = connectionLost ? State::connectionLost : State::gotResult;
                connectionLost   = false;
                gotResult        = false;

                return { state, std::move (pluginDescription) };
            }

            using ChildProcessCoordinator::killWorkerProcess;
            using ChildProcessCoordinator::sendMessageToWorker;

        private:
            void handleMessageFromWorker (const juce::MemoryBlock& mb) override {
                const std::lock_guard<std::mutex> lock { mutex };
                pluginDescription = parseXML (mb.toString());
                gotResult         = true;
                condvar.notify_one();
            }

            void handleConnectionLost() override {
                const std::lock_guard<std::mutex> lock { mutex };
                connectionLost = true;
                condvar.notify_one();
            }

            std::mutex mutex;
            std::condition_variable condvar;

            std::unique_ptr<juce::XmlElement> pluginDescription;
            bool connectionLost = false;
            bool gotResult      = false;

            JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SubprocessCoordinator)
        };

        using ScanFilter = std::function<bool (const juce::PluginDescription&)>;

        CustomPluginScanner(ScanFilter filter) : filter(filter) {}
        ~CustomPluginScanner() override {}

        bool findPluginTypesFor (juce::AudioPluginFormat& format,
            juce::OwnedArray<juce::PluginDescription>& result,
            const juce::String& fileOrIdentifier) override {
            if (addPluginDescriptions (format.getName(), fileOrIdentifier, result)) return true;

            scanCoordinator = nullptr;
            return false;
        }

        void scanFinished() override { scanCoordinator = nullptr; }

    private:
        /*  Scans for a plugin with format 'formatName' and ID 'fileOrIdentifier' using a subprocess,
        and adds discovered plugin descriptions to 'result'.

        Returns true on success.

        Failure indicates that the subprocess is unrecoverable and should be terminated.
    */
        bool addPluginDescriptions (const juce::String& formatName,
            const juce::String& fileOrIdentifier,
            juce::OwnedArray<juce::PluginDescription>& result) {
            if (scanCoordinator == nullptr) scanCoordinator = std::make_unique<SubprocessCoordinator>();

            juce::MemoryBlock block;
            juce::MemoryOutputStream stream { block, true };
            stream.writeString (formatName);
            stream.writeString (fileOrIdentifier);

            if (!scanCoordinator->sendMessageToWorker (block)) return false;

            for (;;) {
                if (shouldExit()) return true;

                const auto response = scanCoordinator->getResponse();

                if (response.state == SubprocessCoordinator::State::timeout) continue;

                if (response.xml != nullptr) {
                    for (const auto* item : response.xml->getChildIterator()) {
                        auto desc = std::make_unique<juce::PluginDescription>();

                        if (desc->loadFromXml (*item)) {
                            // Only add the plugin description if the filter returns true
                            if (filter(*desc))
                                result.add (std::move (desc));
                        }
                    }
                }

                return (response.state == SubprocessCoordinator::State::gotResult);
            }
        }

        std::unique_ptr<SubprocessCoordinator> scanCoordinator;
        ScanFilter filter;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CustomPluginScanner)
    };

}
