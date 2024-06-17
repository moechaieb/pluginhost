#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

namespace timeoffaudio {
    namespace scanner {
        class Worker final : private juce::ChildProcessWorker,
                             private juce::AsyncUpdater {
        public:
            Worker() {
                formatManager.addFormat (new juce::VST3PluginFormat());
#if JUCE_MAC
        formatManager.addFormat (new juce::AudioUnitPluginFormat());
#endif
            }

            using ChildProcessWorker::initialiseFromCommandLine;

        private:
            void handleMessageFromCoordinator (const juce::MemoryBlock& mb) override {
                if (mb.isEmpty())
                    return;

                if (!doScan (mb)) {
                    {
                        const std::lock_guard<std::mutex> lock (mutex);
                        pendingBlocks.emplace (mb);
                    }

                    triggerAsyncUpdate();
                }
            }

            void handleConnectionLost() override {
                juce::JUCEApplicationBase::quit();
            }

            void handleAsyncUpdate() override {
                for (;;) {
                    const auto block = [&]() -> juce::MemoryBlock {
                        const std::lock_guard<std::mutex> lock (mutex);

                        if (pendingBlocks.empty())
                            return {};

                        auto out = std::move (pendingBlocks.front());
                        pendingBlocks.pop();
                        return out;
                    }();

                    if (block.isEmpty())
                        return;

                    doScan (block);
                }
            }

            bool doScan (const juce::MemoryBlock& block) {
                juce::MemoryInputStream stream { block, false };
                const auto formatName = stream.readString();
                const auto identifier = stream.readString();

                juce::PluginDescription pd;
                pd.fileOrIdentifier = identifier;
                pd.uniqueId = pd.deprecatedUid = 0;

                const auto matchingFormat = [&]() -> juce::AudioPluginFormat* {
                    for (auto* format : formatManager.getFormats())
                        if (format->getName() == formatName)
                            return format;

                    return nullptr;
                }();

                if (matchingFormat == nullptr
                    || (!juce::MessageManager::getInstance()->isThisTheMessageThread()
                        && !matchingFormat->requiresUnblockedMessageThreadDuringCreation (pd))) {
                    return false;
                }

                juce::OwnedArray<juce::PluginDescription> results;
                matchingFormat->findAllTypesForFile (results, identifier);
                sendPluginDescriptions (results);
                return true;
            }

            void sendPluginDescriptions (const juce::OwnedArray<juce::PluginDescription>& results) {
                juce::XmlElement xml ("LIST");

                for (const auto& desc : results)
                    xml.addChildElement (desc->createXml().release());

                const auto str = xml.toString();
                sendMessageToCoordinator ({ str.toRawUTF8(), str.getNumBytesAsUTF8() });
            }

            std::mutex mutex;
            std::queue<juce::MemoryBlock> pendingBlocks;

            // After construction, this will only be accessed by doScan so there's no need
            // to worry about synchronisation.
            juce::AudioPluginFormatManager formatManager;
        };

    }
}
