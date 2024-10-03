#pragma once

#include "PluginScan.h"
#include "PluginWindow.h"
#include <choc/containers/choc_Value.h>
#include <imagiro_util/imagiro_util.h>
#include <immer/algorithm.hpp>
#include <immer/box.hpp>
#include <immer/map.hpp>
#include <immer/map_transient.hpp>
#include <immer/set.hpp>
#include <juce_audio_processors/juce_audio_processors.h>

namespace timeoffaudio {
    class PluginHost : private juce::ChangeListener, private juce::AudioProcessorListener, private juce::Timer {
    public:
        using KeyType = std::string;

        class Listener {
        public:
            virtual ~Listener() = default;
            virtual void
                scanProgressed (float /*progress01*/, juce::String /*formatName*/, juce::String /*currentPlugin*/) {}
            virtual void scanFinished() {}
            virtual void availablePluginsUpdated (const juce::Array<juce::PluginDescription>& /*pluginDescriptions*/) {}
            virtual void pluginInstanceLoadSuccessful (PluginHost::KeyType /*uuid*/,
                juce::AudioPluginInstance* /*plugin*/) {}
            virtual void pluginInstanceDeleted (PluginHost::KeyType /*uuid*/, juce::AudioPluginInstance* /*plugin*/) {}
            virtual void pluginInstanceParameterChanged (PluginHost::KeyType /*uuid*/,
                int /*parameterIndex*/,
                float /*newValue*/) {}
            virtual void latenciesChanged() {}

            // TODO: these two are not used anywhere at the moment
            virtual void pluginInstanceUpdated (PluginHost::KeyType /*uuid*/, juce::AudioPluginInstance* /*plugin*/) {}
            virtual void pluginInstanceLoadFailed (PluginHost::KeyType /*uuid*/, std::string /*error*/) {}
        };

        struct Plugin {
            using ConnectionList = immer::set<KeyType>;

            std::shared_ptr<juce::AudioPluginInstance> instance;
            std::shared_ptr<PluginWindow> window;
            juce::RangedAudioParameter* enabledParameter = nullptr;
            ConnectionList connections;

            Plugin() = default;

            // Comparison operators
            auto operator<=> (const Plugin&) const = default;

            // Copy constructor
            Plugin (const Plugin& other)
                : instance (other.instance),
                  window (other.window),
                  enabledParameter (other.enabledParameter),
                  connections (other.connections) {}

            // Move constructor
            Plugin (Plugin&& other) noexcept
                : instance (std::move (other.instance)),
                  window (std::move (other.window)),
                  enabledParameter (other.enabledParameter),
                  connections (std::move (other.connections)) {}

            Plugin (std::shared_ptr<juce::AudioPluginInstance> inst,
                std::shared_ptr<PluginWindow> win,
                juce::RangedAudioParameter* enabledParameter)
                : instance (std::move (inst)), window (std::move (win)), enabledParameter (enabledParameter) {}
        };

        using PluginMap            = immer::map<KeyType, immer::box<Plugin>>;
        using TransientPluginMap   = PluginMap::transient_type;
        using ConnectionsRefreshFn = std::function<Plugin::ConnectionList (KeyType, const TransientPluginMap&)>;

        explicit PluginHost (
            juce::File pluginListFile,
            ConnectionsRefreshFn connectionFactory = [] (KeyType, const TransientPluginMap&) -> Plugin::ConnectionList {
                return {};
            });
        ~PluginHost() override;

        void process (const Plugin& plugin, juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages);

        void addPluginHostListener (Listener* listener);
        void removePluginHostListener (Listener* listener);

        // Plugin management
        void createPluginInstance (TransientPluginMap&,
            juce::PluginDescription pluginDescription,
            KeyType key,
            timeoffaudio::PluginWindow::Options options = {},
            const juce::MemoryBlock& initialState       = juce::MemoryBlock());
        void deletePluginInstance (KeyType key);
        void deletePluginInstance (TransientPluginMap&, KeyType key);
        void movePluginInstance (KeyType fromKey, KeyType toKey);
        void movePluginInstance (TransientPluginMap&, KeyType fromKey, KeyType toKey);

        /*
            TransientPluginMap is passed by reference to the lambda, so the lambda can mutate the
            TransientPluginMap. This is useful for mutating the TransientPluginMap without the need
            for locking the TransientPluginMap.

            Use this to write to the plugin graph from the non-realtime thread.
            This will re-compute the connections of each node in the graph when using the RefreshConnections PostUpdateAction flag.
        */
        enum class PostUpdateAction { None, RefreshConnections };
        template <typename NonRealtimeMutator>
        void withWriteAccess (NonRealtimeMutator&& mutator,
            PostUpdateAction postUpdateAction = PostUpdateAction::None) {
            assertMessageThread();

            auto previousNonRealtimeSafePlugins = nonRealtimeSafePlugins;
            auto transientPlugins               = nonRealtimeSafePlugins.transient();
            std::forward<NonRealtimeMutator>(mutator) (transientPlugins);

            // Re-compute the connections after the plugin map is altered each time
            // TODO: Can be optimised via a custom differ:
            // 1. If only plugin windows are altered, don't refresh connections
            // 2. If a new plugin is loaded, only refresh connections for that plugin
            // 3. If a plugin is removed, refresh connections for all plugins
            // 4. If plugins are swapped, refresh connections for all plugins
            // immer::diff (plugins,
            //     transientPlugins.persistent(),
            //     immer::make_differ ([] (const auto& added) { /* handle added elements */ },
            //         [] (const auto& removed) { /* handle removed elements */ },π
            //         [] (const auto& changed) { /* handle changed elements */ }));

            if (postUpdateAction == PostUpdateAction::RefreshConnections) {
                for (auto& [key, pluginBox] : transientPlugins) {
                    transientPlugins.set (key, pluginBox.update ([&, key] (auto plugin) {
                        plugin.connections = getConnectionsFor (key, transientPlugins);
                        return plugin;
                    }));
                }
            }

            nonRealtimeSafePlugins = transientPlugins.persistent();
            diffAndNotifyListeners (previousNonRealtimeSafePlugins, nonRealtimeSafePlugins);

            auto result = synchronizationQueue.enqueue (nonRealtimeSafePlugins);
            jassert (result);
        }

        /*
            Use this to access the plugin graph from the non-realtime thread, in a read-only fashion.
        */
        template <typename NonRealtimeReadonlyAccessor>
        void withReadonlyAccess (NonRealtimeReadonlyAccessor&& accessor) const {
            assertMessageThread();

            // Access the non-realtime-safe copy of the plugin map, which is set to const& to ensure it's read-only
            std::forward<NonRealtimeReadonlyAccessor> (accessor) (static_cast<const PluginMap&>(nonRealtimeSafePlugins));
        }

        /*
            Use this to access the plugin graph from the realtime thread.
        */
        template <typename RealtimeAccessor>
        void withRealtimeAccess (RealtimeAccessor&& accessor) {
            bool isNewCopy = false;
            // Get the latest PluginMap submitted for the realtime thread
            while (synchronizationQueue.try_dequeue (realtimeSafePlugins)) {
                isNewCopy = true;
            }

            // Access the realtime-safe copy of the plugin map, which is set to const& to ensure it's read-only
            std::forward<RealtimeAccessor> (accessor) (static_cast<const PluginMap&>(realtimeSafePlugins));

            if (isNewCopy) {
                auto result = deallocationQueue.try_enqueue (realtimeSafePlugins);
                jassert (result);
            }
        }

        void traversePluginsFrom (KeyType key, std::function<void (Plugin)> visitor) const;

        // Plugin discovery
        juce::Array<juce::AudioPluginFormat*> getFormats() const;
        juce::Array<juce::PluginDescription> getAvailablePlugins() const;
        void clearAllAvailablePlugins();
        void clearAvailablePlugin (const juce::PluginDescription& pluginToClear);
        void startScan (const juce::String& format);
        bool isScanInProgress() const;
        void abortOngoingScan() const;
        choc::value::Value getScanStatus() const;

        // Plugin Windows
        void openPluginWindow (KeyType key, timeoffaudio::PluginWindow::Options options = {});
        void openPluginWindow (TransientPluginMap&, KeyType key, timeoffaudio::PluginWindow::Options options = {});

        void closePluginWindow (TransientPluginMap&, KeyType key);
        void closePluginWindow (KeyType key);

        void closeAllPluginWindows();

        void updatePluginWindowBorderColour (KeyType key, juce::Colour colour);
        void updatePluginWindowBorderColour (TransientPluginMap&, KeyType key, juce::Colour colour);

        void bringPluginWindowToFront (KeyType key);
        void bringPluginWindowToFront (TransientPluginMap&, KeyType key);

        // Plugin parameters
        juce::Array<juce::AudioProcessorParameter*> getParameters (KeyType key) const;
        void beginChangeGestureForParameter (const KeyType& key, int parameterIndex) const;
        void endChangeGestureForParameter (const KeyType& key, int parameterIndex) const;
        void setValueForParameter (const KeyType& key, int parameterIndex, float value) const;
        juce::String getDisplayValueForParameter (const KeyType& key, int parameterIndex, float value) const;
        void audioProcessorParameterChanged (juce::AudioProcessor* processor,
            int parameterIndex,
            float newValue) override;
        void audioProcessorChanged (juce::AudioProcessor* processor,
            const juce::AudioProcessor::ChangeDetails& details) override;
        void audioProcessorParameterChangeGestureBegin (juce::AudioProcessor* processor, int parameterIndex) override;
        void audioProcessorParameterChangeGestureEnd (juce::AudioProcessor* processor, int parameterIndex) override;

        void debugPrintState() const;

    protected:
        juce::KnownPluginList knownPlugins;
        std::unique_ptr<timeoffaudio::PluginScan> currentScan { nullptr };
        void prepare (int newSampleRate, int newBlockSize, juce::AudioPlayHead* newPlayhead = nullptr);

        // Plugin persistence
        choc::value::Value getPluginState (KeyType key, const PluginMap& pluginMap) const;
        choc::value::Value getAllPluginsState() const;

        void loadPluginFromState (TransientPluginMap& pluginMap, const choc::value::Value& pluginState);
        void loadAllPluginsFromState (const choc::value::Value& allPluginsState);

        virtual juce::RangedAudioParameter* getEnabledParameterForKey (KeyType key) { return nullptr; }

    private:
        juce::File pluginListFile;
        juce::ReadWriteLock listenersLock;

        int sampleRate;
        int blockSize;
        juce::AudioPlayHead* playhead;

        PluginMap realtimeSafePlugins, nonRealtimeSafePlugins, deallocationCopyPlugins;
        moodycamel::ReaderWriterQueue<PluginMap> synchronizationQueue { 100 };
        moodycamel::ReaderWriterQueue<PluginMap> deallocationQueue { 100 };

        ConnectionsRefreshFn getConnectionsFor;

        juce::AudioPluginFormatManager formatManager;
        juce::ListenerList<Listener> listeners;
        void changeListenerCallback (juce::ChangeBroadcaster* source) override;

        juce::AudioProcessorParameter* getParameter (KeyType key, int parameterIndex) const;

        void diffAndNotifyListeners (const PluginMap& previousPlugins, const PluginMap& newPlugins) {
            immer::diff (previousPlugins,
                newPlugins,
                immer::make_differ (
                    [&] (const PluginMap::value_type& added) {
                        const juce::ScopedReadLock lock (listenersLock);
                        listeners.call (
                            &Listener::pluginInstanceLoadSuccessful, added.first, added.second->instance.get());
                    },
                    [&] (const PluginMap::value_type& removed) {
                        const juce::ScopedReadLock lock (listenersLock);
                        listeners.call (
                            &Listener::pluginInstanceDeleted, removed.first, removed.second->instance.get());
                    },
                    [&] (const PluginMap::value_type& changedFrom, const PluginMap::value_type& changedTo) {
                        const juce::ScopedReadLock lock (listenersLock);

                        const auto& [changedFromKey, changedFromPluginBox] = changedFrom;
                        const auto& [changedToKey, changedToPluginBox]     = changedTo;

                        if (changedFromKey != changedToKey) {
                            // Notify listeners that the old plugin at the updated key is deleted
                            listeners.call (
                                &Listener::pluginInstanceDeleted, changedFromKey, changedFromPluginBox->instance.get());

                            // Notify listeners that the new plugin at the updated key is loaded
                            return listeners.call (&Listener::pluginInstanceLoadSuccessful,
                                changedTo.first,
                                changedTo.second->instance.get());
                        }

                        // If we're here, it means a plugin has been updated in-place, i.e. its window was opened,
                        // its connections have been updated, etc
                        // TODO: add other listener notifications here for in-place plugin updates as needed
                    }));
        }

        void timerCallback() override {
            // We want to ensure that we keep a copy of the plugin map that is only used to ensure that we don't
            // deallocate on the RT thread
            // Why? Without this extra @deallocationCopyPlugins, we run into the risk of realtimeSafePlugins being the
            // last holding on to certain memory (like when a plugin is deleted)
            // This will trigger a deallocation on the realtime thread, which is NOT realtime safe.

            // Instead, we keep this extra deallocationCopyPlugins, and run this loop on the message thread to ensure
            // these deallocations happen away from the realtime thread
            // We can technically run this on any non-RT thread, as long as it's synchronized with the message thread
            while (deallocationQueue.try_dequeue (deallocationCopyPlugins)) {
            }
        }

        static void assertMessageThread() {
#if JUCE_DEBUG
            if (const auto messageManager = juce::MessageManager::getInstanceWithoutCreating();
                !messageManager->isThisTheMessageThread())
                jassertfalse;
#endif
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginHost)
    };
}
