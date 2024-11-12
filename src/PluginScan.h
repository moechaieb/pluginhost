#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>

namespace timeoffaudio {
    class PluginScan final : private juce::Timer {
        constexpr static int NUM_THREADS = 1;

    public:
        using ScanProgressCallback =
            std::function<void (float progress01, juce::String formatName, juce::String currentPlugin)>;
        using ScanFinishedCallback = std::function<void()>;

        PluginScan (juce::KnownPluginList& l,
            juce::AudioPluginFormat& format,
            juce::File& failedToLoadPluginsFolder,
            ScanProgressCallback oSP,
            ScanFinishedCallback oSF,
            bool allowPluginsWhichRequireAsynchronousInstantiation = true,
            int threads                                            = NUM_THREADS)
            : allowAsync (allowPluginsWhichRequireAsynchronousInstantiation),
              numThreads (threads),
              list (l),
              formatToScan (format),
              failedToLoadPluginsFolder (failedToLoadPluginsFolder),
              onScanProgress (std::move(oSP)),
              onScanFinished (std::move(oSF)) {
            const auto blacklisted = list.getBlacklistedFiles();
            directoryScanner = std::make_unique<juce::PluginDirectoryScanner> (list,
                formatToScan,
                formatToScan.getDefaultLocationsToSearch(),
                true,
                failedToLoadPluginsFolder.getChildFile ("failedToLoadPlugins"),
                allowAsync);
            pool = std::make_unique<juce::ThreadPool>(juce::ThreadPoolOptions().withNumberOfThreads(numThreads));
            // You need to use at least one thread when scanning plug-ins asynchronously
            jassert (!allowAsync || (numThreads > 0));

            start();
        }

        ~PluginScan() override = default;

        void abort() const { finish(); }
        [[nodiscard]] float getProgress() const { return directoryScanner->getProgress(); }
        [[nodiscard]] juce::String getCurrentPlugin() const
        {
            return pluginBeingScanned.fromLastOccurrenceOf("\\", false, true);
        }
        [[nodiscard]] juce::String getFormatName() const { return formatToScan.getName(); }

    private:
        bool allowAsync = false;
        const int numThreads;
        juce::KnownPluginList& list;
        juce::AudioPluginFormat& formatToScan;
        ScanProgressCallback onScanProgress;
        ScanFinishedCallback onScanFinished;
        std::unique_ptr<juce::PluginDirectoryScanner> directoryScanner;
        juce::String pluginBeingScanned;
        std::unique_ptr<juce::ThreadPool> pool;
        juce::File& failedToLoadPluginsFolder;

        void start() {
            for (int i = numThreads; --i >= 0;) pool->addJob (new ScanJob (*this), true);

            startTimerHz (20);
        }

        void finish() const {
            // Setting the first argument to true will interrupt the scan jobs that are currently running
            // This is important because it allows the scan to be aborted mid-way through
            pool->removeAllJobs (true, 1000);
            jassert (pool->getNumJobs() == 0);

            for (const auto& failed : directoryScanner->getFailedFiles())
                list.addToBlacklist(failed);

            onScanFinished(); // This should be called last as it will cause the PluginScan to go out of scope and be destroyed
        }

        bool scanNextPlugin() {
            // This check is important because it allows the scan to be aborted mid-way through
            if (directoryScanner->scanNextFile (true, pluginBeingScanned)) {
                onScanProgress ((float) getProgress(), formatToScan.getName(), pluginBeingScanned.fromLastOccurrenceOf("\\", false, true));
                return true;
            }
            return false;
        }

        void timerCallback() override {
            if (pool->getNumJobs() == 0) {
                // This function triggers the finish() function to be called
                // which will destroy the PluginScan object via the onScanFinished callback
                // Therefore, it must be the very last thing to call in the lifecycle of
                // the PluginScan object, or it will result in leaks
                finish();
            }
        }

        struct ScanJob final : public juce::ThreadPoolJob {
            explicit ScanJob (PluginScan& s) : ThreadPoolJob ("pluginScanJob"), scan (s) {}

            JobStatus runJob() override {
                while (!shouldExit() && scan.scanNextPlugin()) {}
                return ThreadPoolJob::JobStatus::jobHasFinished;
            }

            PluginScan& scan;
            JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScanJob)
        };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginScan)
    };
}
