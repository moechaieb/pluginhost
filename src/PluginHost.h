// project: common

#pragma once
#include "PluginScan.h"
#include "PluginWindow.h"
#include <choc/containers/choc_Value.h>
#include <immer/algorithm.hpp>
#include <immer/box.hpp>
#include <immer/map.hpp>
#include <immer/map_transient.hpp>
#include <immer/set.hpp>
#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>

namespace timeoffaudio {
    class PluginHost : private juce::ChangeListener, private juce::AudioProcessorListener {
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
            imagiro::Parameter* enabledParameter;
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
                imagiro::Parameter* enabledParameter)
                : instance (std::move (inst)), window (std::move (win)), enabledParameter (enabledParameter) {}
        };

        using PluginMap            = immer::map<KeyType, immer::box<Plugin>>;
        using TransientPluginMap   = PluginMap::transient_type;
        using ConnectionsRefreshFn = std::function<Plugin::ConnectionList (KeyType, const TransientPluginMap&)>;

        PluginHost (
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
            const juce::PluginDescription pluginDescription,
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
        */
        void withWriteAccess (std::function<void (TransientPluginMap&)> mutator);

        // Templated version of withWriteAccess to control when to refresh connections
        enum class PostUpdateAction { None, RefreshConnections };
        template <PostUpdateAction action>
        void withWriteAccess (std::function<void (TransientPluginMap&)> mutator);

        /*
            PluginMap is passed by value to implicitly create a copy of the PluginMap inside the lambda
            This is useful for read-only access to the PluginMap without the need for locking
            the PluginMap.

            Even if another thread updates the PluginMap, the copy inside the lambda will not be
            affected. It will be out-of-date technically, but that's okay since the next time the lambda
            is called, the PluginMap will be a fresh copy anyways.
        */
        void withReadOnlyAccess (std::function<void (const PluginMap)> accessor) const;

        void traversePluginsFrom (KeyType key, std::function<void (Plugin)> visitor) const;

        // Plugin discovery
        const juce::Array<juce::AudioPluginFormat*> getFormats() const;
        const juce::Array<juce::PluginDescription> getAvailablePlugins() const;
        void clearAllAvailablePlugins();
        void clearAvailablePlugin (const juce::PluginDescription& pluginToClear);
        void startScan (const juce::String& format);
        bool isScanInProgress() const;
        void abortOngoingScan();
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
        const juce::Array<juce::AudioProcessorParameter*> getParameters (KeyType key) const;
        void beginChangeGestureForParameter (KeyType key, int parameterIndex);
        void endChangeGestureForParameter (KeyType key, int parameterIndex);
        void setValueForParameter (KeyType key, int parameterIndex, float value);
        juce::String getDisplayValueForParameter (KeyType key, int parameterIndex, float value);
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
        void prepare (const int newSampleRate, const int newBlockSize, juce::AudioPlayHead* newPlayhead = nullptr);

        // Plugin persistence
        choc::value::Value getPluginState (KeyType key, const PluginMap pluginMap) const;
        choc::value::Value getAllPluginsState() const;

        void loadPluginFromState (TransientPluginMap& pluginMap, const choc::value::Value pluginState);
        void loadAllPluginsFromState (choc::value::Value allPluginsState);

        virtual imagiro::Parameter* getEnabledParameterForKey (KeyType key) { return nullptr; }

    private:
        juce::File pluginListFile;

        int sampleRate;
        int blockSize;
        juce::AudioPlayHead* playhead;

        PluginMap plugins;
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
                        listeners.call (
                            &Listener::pluginInstanceLoadSuccessful, added.first, added.second->instance.get());
                    },
                    [&] (const PluginMap::value_type& removed) {
                        listeners.call (
                            &Listener::pluginInstanceDeleted, removed.first, removed.second->instance.get());
                    },
                    [&] (const PluginMap::value_type& changedFrom, const PluginMap::value_type& changedTo) {
                        // Notify listeners that the old plugin at the updated key is deleted
                        listeners.call (
                            &Listener::pluginInstanceDeleted, changedFrom.first, changedFrom.second->instance.get());

                        // Notify listeners that the new plugin at the updated key is loaded
                        listeners.call (
                            &Listener::pluginInstanceLoadSuccessful, changedTo.first, changedTo.second->instance.get());
                    }));
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginHost)
    };
}
