#include "Worker.h"
#include <juce_events/juce_events.h>

namespace timeoffaudio {
    namespace scanner {
        constexpr const char* PROCESS_UID = "pluginScanner";
        class Application : public juce::JUCEApplicationBase {
        public:
            const juce::String getApplicationName() override { return "time off audio plugin scanner"; }
            const juce::String getApplicationVersion() override { return "0.0.3"; }

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
                auto scannerWorker = std::make_unique<timeoffaudio::scanner::Worker> ();
                if (!scannerWorker->initialiseFromCommandLine (commandLineParameters, PROCESS_UID)) {
                    return;
                }

                juce::SystemStats::setApplicationCrashHandler(crashHandler);

                DBG ("Initialised scanner worker and connected to coordinator");
                worker = std::move (scannerWorker);
            }

            static void crashHandler (void*) {
                // Setting a custom crash handler that does nothing,
                // simply to swallow the crash windows appearing on macOS
                // TODO: log something
            }

        private:
            std::unique_ptr<timeoffaudio::scanner::Worker> worker;
        };
    }
}

START_JUCE_APPLICATION (timeoffaudio::scanner::Application)
