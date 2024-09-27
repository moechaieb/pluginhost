#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

namespace timeoffaudio::scanner {
    class Worker final : private juce::ChildProcessWorker, private juce::AsyncUpdater {
    public:
        Worker() : logger (juce::FileLogger::createDateStampedLogger ("time off audio", "PluginScanner", ".log", "")) {
            formatManager.addFormat (new juce::VST3PluginFormat());
#if JUCE_MAC
            formatManager.addFormat (new juce::AudioUnitPluginFormat());
#endif
            logger->logMessage ("[timeoffaudio::scanner::Worker] Constructor completed.");
        }

        bool initialiseFromCommandLine (const juce::String& commandLine,
            const juce::String& commandLineUniqueID,
            int timeoutMs = 0) {
            logger->logMessage ("[timeoffaudio::scanner::Worker] Initialised from command line.");
            logger->logMessage ("[timeoffaudio::scanner::Worker] With arguments: commandLine:" + commandLine
                                + ", commandLineUniqueID:" + commandLineUniqueID
                                + ", timeoutMs:" + juce::String (timeoutMs));
            const bool result =
                juce::ChildProcessWorker::initialiseFromCommandLine (commandLine, commandLineUniqueID, timeoutMs);

            if (result) {
                logger->logMessage (
                    "[timeoffaudio::scanner::Worker] juce::ChildProcessWorker::initialiseFromCommandLine successful.");
            } else {
                logger->logMessage (
                    "[timeoffaudio::scanner::Worker] juce::ChildProcessWorker::initialiseFromCommandLine failed.");
            }

            return result;
        }

    private:
        void handleMessageFromCoordinator (const juce::MemoryBlock& mb) override {
            logger->logMessage ("[timeoffaudio::scanner::Worker] Received message from coordinator, of size "
                                + juce::String (mb.getSize()));

            if (mb.isEmpty()) return;

            if (!doScan (mb)) {
                {
                    const std::lock_guard<std::mutex> lock (mutex);
                    pendingBlocks.emplace (mb);
                }

                triggerAsyncUpdate();
            }
        }

        void handleConnectionLost() override {
            logger->logMessage ("[timeoffaudio::scanner::Worker] Connection lost.");
            juce::JUCEApplicationBase::quit();
        }

        void handleAsyncUpdate() override {
            for (;;) {
                const auto block = [&]() -> juce::MemoryBlock {
                    const std::lock_guard lock (mutex);

                    if (pendingBlocks.empty()) return {};

                    auto out = std::move (pendingBlocks.front());
                    pendingBlocks.pop();
                    return out;
                }();

                if (block.isEmpty()) return;

                doScan (block);
            }
        }

        bool doScan (const juce::MemoryBlock& block) {
            juce::MemoryInputStream stream { block, false };
            const auto formatName = stream.readString();
            const auto identifier = stream.readString();

            logger->logMessage (
                "[timeoffaudio::scanner::Worker] doScan: formatName: " + formatName + ", identifier: " + identifier);

            juce::PluginDescription pd;
            pd.fileOrIdentifier = identifier;
            pd.uniqueId = pd.deprecatedUid = 0;

            const auto matchingFormat = [&]() -> juce::AudioPluginFormat* {
                for (auto* format : formatManager.getFormats())
                    if (format->getName() == formatName) return format;

                return nullptr;
            }();

            if (matchingFormat == nullptr) {
                logger->logMessage ("[timeoffaudio::scanner::Worker] doScan failed. Did not find matching format");
                return false;
            }

            if (!matchingFormat->fileMightContainThisPluginType (identifier)) {
                logger->logMessage (
                    "[timeoffaudio::scanner::Worker] doScan failed. fileMightContainThisPluginType returned false for identifier: "
                    + identifier);
                return false;
            }

            juce::OwnedArray<juce::PluginDescription> results;
            matchingFormat->findAllTypesForFile (results, identifier);

            if (results.isEmpty()) {
                logger->logMessage (
                    "[timeoffaudio::scanner::Worker] [Warning] No plugin descriptions found for identifier: "
                    + identifier);
            }

            sendPluginDescriptions (results);

            return true;
        }

        void sendPluginDescriptions (const juce::OwnedArray<juce::PluginDescription>& results) {
            juce::XmlElement xml ("LIST");

            for (const auto& desc : results) xml.addChildElement (desc->createXml().release());

            const auto str = xml.toString();
            sendMessageToCoordinator ({ str.toRawUTF8(), str.getNumBytesAsUTF8() });

            logger->logMessage (
                "[timeoffaudio::scanner::Worker] Sent plugin descriptions to coordinator. Message: " + str);
        }

        std::mutex mutex;
        std::queue<juce::MemoryBlock> pendingBlocks;

        // After construction, this will only be accessed by doScan so there's no need
        // to worry about synchronisation.
        juce::AudioPluginFormatManager formatManager;
        juce::FileLogger* logger;
    };

}
