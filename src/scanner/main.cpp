#include "Worker.h"
#include <juce_events/juce_events.h>

namespace timeoffaudio {
    namespace scanner {
        constexpr const char* PROCESS_UID = "pluginScanner";
        class Application : public juce::JUCEApplicationBase {
        public:
            const juce::String getApplicationName() override { return "time off audio plugin scanner"; }
            const juce::String getApplicationVersion() override { return "0.0.1"; }

            bool moreThanOneInstanceAllowed() override { return true; }

            void anotherInstanceStarted (const juce::String& commandLine) override {}
            void suspended() override {}
            void resumed() override {}
            void shutdown() override {}

            void systemRequestedQuit() override { quit(); }

            void unhandledException (const std::exception* exception,
                const juce::String& sourceFilename,
                int lineNumber) override {
                // for some reason, this doesn't actually get called and the runtime just terminates...
            }

            void initialise (const juce::String& commandLineParameters) override {
                auto scannerWorker = std::make_unique<timeoffaudio::scanner::Worker>();
                if (!scannerWorker->initialiseFromCommandLine (commandLineParameters, PROCESS_UID)) {
                    DBG ("Failed to initialise scanner worker and connect to coordinator");
                    return;
                }

                DBG ("Initialised scanner worker and connected to coordinator");
                worker = std::move (scannerWorker);
            }

            static void crashHandler (void*) {

            }
        private:
            std::unique_ptr<timeoffaudio::scanner::Worker> worker;
        };
    }
}

START_JUCE_APPLICATION (timeoffaudio::scanner::Application)
