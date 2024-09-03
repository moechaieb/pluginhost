// project: common

#include "PluginHost.h"
#include "../../lib/instrumentation/Assertion.h"
#include "./KnownPluginListScanner.h"
#include "PluginScan.h"
#include "PluginWindow.h"
#include "choc/containers/choc_Value.h"
#include <memory>

namespace timeoffaudio {
    PluginHost::PluginHost (juce::PropertiesFile& configFile, ConnectionsRefreshFn cF) : configFile (configFile), getConnectionsFor (cF) {
        knownPlugins.setCustomScanner (std::make_unique<timeoffaudio::CustomPluginScanner>());
        if (auto savedPluginList = configFile.getXmlValue ("pluginList"))
            knownPlugins.recreateFromXml (*savedPluginList);
        else
            changeListenerCallback (&knownPlugins);

        formatManager.addFormat (new juce::VST3PluginFormat());
#if JUCE_MAC
        formatManager.addFormat (new juce::AudioUnitPluginFormat());
#endif
        knownPlugins.addChangeListener (this);

        // TODO: make a immer::differ on the plugins map so that we can intercept the removal of a plugin
        // and remove the PluginHost as a listener on the plugin instance
    }

    PluginHost::~PluginHost() {
        timeoffaudio_assert (isScanInProgress() == false);
        timeoffaudio_assert (listeners.isEmpty());

        knownPlugins.removeChangeListener (this);
        for (auto& [_, pluginBox] : plugins) pluginBox.get().instance->removeListener (this);
    }

    const juce::Array<juce::AudioPluginFormat*> PluginHost::getFormats() const { return formatManager.getFormats(); }

    const juce::Array<juce::PluginDescription> PluginHost::getAvailablePlugins() const {
        return knownPlugins.getTypes();
    }

    void PluginHost::deletePluginInstance (KeyType key) {
        withWriteAccess ([&] (TransientPluginMap& pluginMap) { deletePluginInstance (pluginMap, key); });
    }
    void PluginHost::deletePluginInstance (TransientPluginMap& pluginMap, KeyType key) {
        pluginMap.erase (key);
    }

    void PluginHost::movePluginInstance (KeyType fromKey, KeyType toKey) {
        withWriteAccess ([&] (TransientPluginMap& pluginMap) { movePluginInstance (pluginMap, fromKey, toKey); });
    }

    void PluginHost::movePluginInstance (TransientPluginMap& pluginMap, KeyType fromKey, KeyType toKey) {
        if (fromKey == toKey) return;
        if (!pluginMap.find (fromKey)) return;

        auto fromPluginEnabled = pluginMap[fromKey]->enabledParameter->getValue();

        if (pluginMap.find (toKey)) {
            // Swap the plugin instances and windows, if the destination key is already in use
            // Make sure to preserve the linked params by key, and only swap their values
            auto toPluginBox     = pluginMap[toKey];
            auto toPluginEnabled = toPluginBox->enabledParameter->getValue();

            pluginMap.update (toKey, [&] (auto pluginBox) {
                return pluginBox.update ([&] (auto plugin) {
                    plugin.instance         = pluginMap[fromKey]->instance;
                    plugin.window           = pluginMap[fromKey]->window;
                    plugin.enabledParameter = getEnabledParameterForKey (toKey);
                    plugin.enabledParameter->setValueAndNotifyHost (fromPluginEnabled);
                    return plugin;
                });
            });

            pluginMap.update (fromKey, [&] (auto pluginBox) {
                return pluginBox.update ([&] (auto plugin) {
                    plugin.instance         = toPluginBox->instance;
                    plugin.window           = toPluginBox->window;
                    plugin.enabledParameter = getEnabledParameterForKey (fromKey);
                    plugin.enabledParameter->setValueAndNotifyHost (toPluginEnabled);
                    return plugin;
                });
            });
        } else {
            // If the destination key is free,
            // move the plugin instance and window to the destination key, and clear the source key
            pluginMap.update (toKey, [&] (auto pluginBox) {
                return pluginBox.update ([&] (auto plugin) {
                    plugin.instance         = pluginMap[fromKey]->instance;
                    plugin.window           = pluginMap[fromKey]->window;
                    plugin.enabledParameter = getEnabledParameterForKey (toKey);
                    plugin.enabledParameter->setValueAndNotifyHost (fromPluginEnabled);
                    return plugin;
                });
            });

            pluginMap.erase (fromKey);
        }
    }

    void PluginHost::createPluginInstance (TransientPluginMap& pluginMap,
        const juce::PluginDescription pluginDescription,
        const KeyType key,
        const juce::MemoryBlock& initialState) {
        for (auto format : formatManager.getFormats()) {
            if (format->getName() == pluginDescription.pluginFormatName) {
                juce::String errorMessage;
                juce::StringPairArray logParameters;
                std::unique_ptr<juce::AudioPluginInstance> instance =
                    format->createInstanceFromDescription (pluginDescription, sampleRate, blockSize, errorMessage);

                if (errorMessage.isNotEmpty() || !instance) {
                    logParameters.set ("success", "false");
                    logParameters.set ("error_message", errorMessage);
                    // TODO: handle plugin loading errors here and notify listeners of error
                    // listeners.call (&Listener::pluginInstanceLoadFailed, key, errorMessage);
                    return;
                }

                logParameters.set ("success", "true");
                logParameters.set ("loaded_plugin_name", pluginDescription.descriptiveName);
                logParameters.set ("loaded_plugin_version", pluginDescription.version);
                logParameters.set ("loaded_plugin_format", pluginDescription.pluginFormatName);
                logParameters.set ("loaded_plugin_manufacturer", pluginDescription.manufacturerName);
                logParameters.set ("key", key);

                // Plugin setup
                instance->enableAllBuses();
                instance->prepareToPlay (sampleRate, blockSize);
                if (playhead) instance->setPlayHead (playhead);
                if (!initialState.isEmpty())
                    instance->setStateInformation (initialState.getData(), (int) initialState.getSize());
                instance->addListener (this);

                pluginMap.set (
                    key, immer::box<Plugin> (std::move (instance), nullptr, getEnabledParameterForKey (key)));

                juce::Analytics::getInstance()->logEvent ("plugin_load", logParameters);

                break;
            }
        }
    }

    template <>
    void PluginHost::withWriteAccess<PluginHost::PostUpdateAction::RefreshConnections> (
        std::function<void (TransientPluginMap&)> mutator) {
        const auto previousPlugins  = plugins;
        auto transientPlugins = previousPlugins.transient();
        mutator (transientPlugins);

        // Re-compute the connections after the plugin map is altered each time
        // TODO: Can be optimised via a another custom differ:
        // 1. If only plugin windows are altered, don't refresh connections
        // 2. If a new plugin is loaded, only refresh connections for that plugin
        // 3. If a plugin is removed, refresh connections for all plugins
        // 4. If plugins are swapped, refresh connections for all plugins
        // immer::diff (plugins,
        //     transientPlugins.persistent(),
        //     immer::make_differ ([] (const auto& added) { /* handle added elements */ },
        //         [] (const auto& removed) { /* handle removed elements */ },π
        //         [] (const auto& changed) { /* handle changed elements */ }));

        for (auto& [key, pluginBox] : transientPlugins) {
            transientPlugins.set (key, pluginBox.update ([&, key] (auto plugin) {
                plugin.connections = getConnectionsFor (key, transientPlugins);
                return plugin;
            }));
        }

        plugins = transientPlugins.persistent();
        diffAndNotifyListeners (previousPlugins, plugins);
    }

    template <>
    void PluginHost::withWriteAccess<PluginHost::PostUpdateAction::None> (
        std::function<void (TransientPluginMap&)> mutator) {
        const auto previousPlugins  = plugins;
        auto transientPlugins = previousPlugins.transient();
        mutator (transientPlugins);
        plugins = transientPlugins.persistent();
        diffAndNotifyListeners (previousPlugins, plugins);
    }

    void PluginHost::withWriteAccess (std::function<void (TransientPluginMap&)> mutator) {
        withWriteAccess<PostUpdateAction::RefreshConnections> (mutator);
    }

    void PluginHost::withReadOnlyAccess (std::function<void (const PluginMap)> accessor) const { accessor (plugins); }

    void PluginHost::startScan (const juce::String& format) {
        auto onScanProgress = [this] (float progress01, juce::String formatName, juce::String currentPlugin) {
            listeners.call (&Listener::scanProgressed, progress01, formatName, currentPlugin);
        };

        auto onScanFinished = [this]() {
            currentScan.reset();
            listeners.call (&Listener::scanFinished);
        };

        // TODO: this needs to be lifted outside of PluginHost so that it's customizable per
        // plugin and not fixed like it is now
        auto scanFilter = [] (const juce::PluginDescription& plugin) { return plugin.isInstrument || plugin.name == JucePlugin_Name; };

        for (auto formatCandidate : formatManager.getFormats())
            if (formatCandidate->getName() == format && formatCandidate->canScanForPlugins()) {
                auto failedToLoadPluginsFolder = configFile.getFile().getParentDirectory();

                currentScan.reset (new timeoffaudio::PluginScan (
                    knownPlugins, *formatCandidate, failedToLoadPluginsFolder, onScanProgress, onScanFinished, scanFilter));
                break;
            }
    }

    void PluginHost::abortOngoingScan() {
        if (currentScan != nullptr) currentScan->abort();
    }

    bool PluginHost::isScanInProgress() const { return currentScan != nullptr; }

    choc::value::Value PluginHost::getScanStatus() const {
        choc::value::Value status = choc::value::createObject ("PluginScanStatus");
        status.addMember ("inProgress", isScanInProgress());

        if (isScanInProgress()) {
            status.addMember ("format", currentScan->getFormatName().toStdString());
            status.addMember ("progress", currentScan->getProgress());
            status.addMember ("currentPlugin", currentScan->getCurrentPlugin().toStdString());
        }

        return status;
    }

    void PluginHost::clearAllAvailablePlugins() {
        timeoffaudio_assert (isScanInProgress() == false);
        knownPlugins.clear();
        knownPlugins.clearBlacklistedFiles();
    }

    void PluginHost::clearAvailablePlugin (const juce::PluginDescription& pluginToClear) {
        timeoffaudio_assert (isScanInProgress() == false);
        knownPlugins.removeType (pluginToClear);
    }

    void PluginHost::addPluginHostListener (Listener* listener) { listeners.add (listener); }

    void PluginHost::removePluginHostListener (Listener* listener) { listeners.remove (listener); }

    void PluginHost::changeListenerCallback (juce::ChangeBroadcaster* source) {
        if (source == &knownPlugins) {
            if (auto savedPluginList = knownPlugins.createXml()) {
                configFile.setValue ("pluginList", savedPluginList.get());
                configFile.saveIfNeeded();
            }

            listeners.call (&Listener::availablePluginsUpdated, knownPlugins.getTypes());
        }
    }

    void PluginHost::process (const Plugin& plugin, juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) {
        auto instance        = plugin.instance.get();
        auto bypassParameter = instance->getBypassParameter();
        bool isEnabled       = plugin.enabledParameter->getValue() == 1.f;

        if (!isEnabled && !bypassParameter) {
            // When getBypassParameter() returns a nullptr, we need to bypass the plugin
            // by calling processBlockBypassed
            instance->processBlockBypassed (buffer, midiMessages);
        } else {
            // When getBypassParameter() returns a valid pointer, we need to
            // set the bypass parameter, and process the plugin normally via processBlock
            bypassParameter->setValue (!isEnabled);
            instance->processBlock (buffer, midiMessages);
        }
    }

    void PluginHost::prepare (const int newSampleRate, const int newBlockSize, juce::AudioPlayHead* newPlayhead) {
        sampleRate = newSampleRate;
        blockSize  = newBlockSize;
        playhead   = newPlayhead;

        withReadOnlyAccess ([&] (const PluginMap& pluginMap) {
            for (auto& [key, pluginBox] : pluginMap) {
                auto instance = pluginBox.get().instance.get();

                instance->enableAllBuses();
                instance->prepareToPlay (sampleRate, blockSize);
                if (playhead) instance->setPlayHead (playhead);
            }
        });
    }

    void PluginHost::openPluginWindow (TransientPluginMap& pluginMap, std::string key, int xPos, int yPos) {
        pluginMap.update_if_exists (key, [&] (auto pluginBox) {
            return pluginBox.update ([&] (auto plugin) {
                if (auto pluginWindow = plugin.window) {
                    pluginWindow->setTopLeftPosition (xPos, yPos);
                    pluginWindow->toFront (false);
                    pluginWindow->setVisible (true);
                } else
                    plugin.window = std::make_shared<timeoffaudio::PluginWindow> (*plugin.instance, xPos, yPos);
                return plugin;
            });
        });
    }

    void PluginHost::openPluginWindow (std::string key, int xPos, int yPos) {
        withWriteAccess<PluginHost::PostUpdateAction::None> (
            [&] (PluginHost::TransientPluginMap& pluginMap) { openPluginWindow (pluginMap, key, xPos, yPos); });
    }

    void PluginHost::updatePluginWindowBorderColour (TransientPluginMap& pluginMap,
        std::string key,
        juce::Colour colour) {
        pluginMap.update_if_exists (key, [&] (auto pluginBox) {
            return pluginBox.update ([&] (auto plugin) {
                if (auto pluginWindow = plugin.window) pluginWindow->setBackgroundColour (colour);
                return plugin;
            });
        });
    }

    void PluginHost::updatePluginWindowBorderColour (std::string key, juce::Colour colour) {
        withWriteAccess<PluginHost::PostUpdateAction::None> ([&] (PluginHost::TransientPluginMap& pluginMap) {
            updatePluginWindowBorderColour (pluginMap, key, colour);
        });
    }

    void PluginHost::closePluginWindow (std::string key) {
        withWriteAccess<PluginHost::PostUpdateAction::None> (
            [&] (PluginHost::TransientPluginMap& pluginMap) { closePluginWindow (pluginMap, key); });
    }

    void PluginHost::closePluginWindow (TransientPluginMap& pluginMap, std::string key) {
        pluginMap.update_if_exists (key, [&] (auto pluginBox) {
            return pluginBox.update ([&] (auto plugin) {
                if (auto pluginWindow = plugin.window) pluginWindow->setVisible (false);
                return plugin;
            });
        });
    }

    void PluginHost::closeAllPluginWindows() {
        withWriteAccess<PluginHost::PostUpdateAction::None> ([&] (PluginHost::TransientPluginMap& pluginMap) {
            for (auto& plugin : pluginMap) closePluginWindow (pluginMap, plugin.first);
        });
    }

    void PluginHost::bringPluginWindowToFront (KeyType key) {
        withWriteAccess<PluginHost::PostUpdateAction::None> (
            [&] (PluginHost::TransientPluginMap& pluginMap) { bringPluginWindowToFront (pluginMap, key); });
    }

    void PluginHost::bringPluginWindowToFront (TransientPluginMap& pluginMap, KeyType key) {
        pluginMap.update_if_exists (key, [&] (auto pluginBox) {
            return pluginBox.update ([&] (auto plugin) {
                if (auto pluginWindow = plugin.window) pluginWindow->toFront (false);
                return plugin;
            });
        });
    }

    choc::value::Value PluginHost::getPluginState (KeyType key, const PluginMap pluginMap) const {
        auto pluginState = choc::value::createObject ("PluginState");

        if (auto pluginBox = pluginMap.find (key)) {
            auto pluginDescriptionXml = pluginBox->get().instance->getPluginDescription().createXml();
            if (!pluginDescriptionXml) {
                timeoffaudio_assert (false);
                return pluginState;
            }

            pluginState.addMember ("key", key);
            pluginState.addMember ("description",
                pluginDescriptionXml->toString (juce::XmlElement::TextFormat().singleLine()).toStdString());

            juce::MemoryBlock block;
            pluginBox->get().instance->getStateInformation (block);
            pluginState.addMember ("encoded_state", block.toBase64Encoding().toStdString());
        }

        return pluginState;
    }

    choc::value::Value PluginHost::getAllPluginsState() const {
        choc::value::Value allPluginsState = choc::value::createEmptyArray();

        withReadOnlyAccess ([&] (const PluginMap& pluginMap) {
            for (auto& [key, pluginBox] : pluginMap) allPluginsState.addArrayElement (getPluginState (key, pluginMap));
        });

        return allPluginsState;
    }

    void PluginHost::loadPluginFromState (TransientPluginMap& pluginMap, const choc::value::Value pluginState) {
        auto key = pluginState["key"].toString();
        juce::PluginDescription pluginDescription;

        if (auto pluginDescriptionXml = juce::XmlDocument::parse (pluginState["description"].toString())) {
            if (pluginDescription.loadFromXml (*pluginDescriptionXml.get())) {
                juce::MemoryBlock stateToLoad;
                stateToLoad.fromBase64Encoding (pluginState["encoded_state"].toString());
                createPluginInstance (pluginMap, pluginDescription, key, stateToLoad);
                return;
            }
        }

        timeoffaudio_assert (false);
    }

    void PluginHost::loadAllPluginsFromState (choc::value::Value allPluginsState) {
        withWriteAccess<PluginHost::PostUpdateAction::RefreshConnections> ([&] (TransientPluginMap& pluginMap) {
            for (const auto pluginState : allPluginsState)
                loadPluginFromState (pluginMap, choc::value::Value (pluginState));
        });
    }

    const juce::Array<juce::AudioProcessorParameter*> PluginHost::getParameters (KeyType key) const {
        // Filter out parameters that start with "midi cc", "internal", or "bypass", etc
        if (auto pluginBox = plugins.find (key)) {
            auto parameters = pluginBox->get().instance->getParameters();

            parameters.removeIf ([] (juce::AudioProcessorParameter* param) {
                for (auto prefix : { "midi cc", "internal", "bypass", "reserved" })
                    if (param->getName (1024).toLowerCase().startsWith (prefix)) return true;

                return false;
            });

            return parameters;
        }

        return {};
    }

    juce::AudioProcessorParameter* PluginHost::getParameter (KeyType key, int parameterIndex) const {
        juce::AudioProcessorParameter* parameter = nullptr;

        withReadOnlyAccess ([&] (const PluginMap& pluginMap) {
            // TODO: this is not very efficient, but it's the simplest way to get the key
            // figure out how to get the key from the PluginMap without scanning the whole map
            auto pluginBox = pluginMap.find (key);
            if (!pluginBox) return;

            auto pluginInstance = pluginBox->get().instance.get();
            if (!pluginInstance) return;
            parameter = pluginInstance->getHostedParameter (parameterIndex);
        });

        return parameter;
    }

    void PluginHost::beginChangeGestureForParameter (KeyType key, int parameterIndex) {
        if (auto parameter = getParameter (key, parameterIndex)) return parameter->beginChangeGesture();
        // TODO: add error notification here
    }

    void PluginHost::endChangeGestureForParameter (KeyType key, int parameterIndex) {
        if (auto parameter = getParameter (key, parameterIndex)) return parameter->endChangeGesture();
        // TODO: add error notification here
    }

    void PluginHost::setValueForParameter (KeyType key, int parameterIndex, float value) {
        if (auto parameter = getParameter (key, parameterIndex)) return parameter->setValue (value);
        // TODO: add error notification here
    }

    juce::String PluginHost::getDisplayValueForParameter (KeyType key, int parameterIndex, float value) {
        if (auto parameter = getParameter (key, parameterIndex)) return parameter->getText (value, 1024);

        return {};
    }

    void PluginHost::audioProcessorParameterChanged (juce::AudioProcessor* processor,
        int parameterIndex,
        float newValue) {
        auto pluginInstance = dynamic_cast<juce::AudioPluginInstance*> (processor);
        if (!pluginInstance) return;

        withReadOnlyAccess ([&] (const PluginMap& pluginMap) {
            // TODO: this is not very efficient, but it's the simplest way to get the key
            // figure out how to get the key from the PluginMap without scanning the whole map
            for (auto [key, pluginBox] : pluginMap) {
                if (pluginBox.get().instance.get() == pluginInstance) {
                    listeners.call (&Listener::pluginInstanceParameterChanged, key, parameterIndex, newValue);
                    break;
                }
            }
        });
    }

    void PluginHost::audioProcessorChanged (juce::AudioProcessor*, const juce::AudioProcessor::ChangeDetails& details) {
        if (details.latencyChanged) listeners.call (&Listener::latenciesChanged);
    }

    void PluginHost::audioProcessorParameterChangeGestureBegin (juce::AudioProcessor*, int) {}
    void PluginHost::audioProcessorParameterChangeGestureEnd (juce::AudioProcessor*, int) {}

    void PluginHost::debugPrintState() const {
        DBG ("=============================== Plugin Host State ===============================");
        withReadOnlyAccess ([&] (const PluginMap& pluginMap) {
            DBG ("Number of plugins: " + juce::String (pluginMap.size()));
            for (auto [key, pluginBox] : pluginMap) {
                if (auto instance = pluginBox->instance) {
                    DBG ("Plugin at key " + std::string (key) + " is " + instance->getName().toStdString());
                    if (auto enabledParam = pluginBox->enabledParameter) {
                        DBG ("Plugin linked enabled parameter UID: " + juce::String (enabledParam->getUID()));
                        DBG ("Plugin linked enabled parameter name: " + juce::String (enabledParam->getName (1024)));
                        DBG ("Plugin linked enabled parameter value: " + juce::String (enabledParam->getValue()));
                    }
                    DBG ("Plugin num input channels: " + juce::String (instance->getTotalNumInputChannels()));
                    DBG ("Plugin num output channels: " + juce::String (instance->getTotalNumOutputChannels()));
                    DBG ("Plugin input bus count: " + juce::String (instance->getBusCount (true)));
                    DBG ("Plugin output bus count: " + juce::String (instance->getBusCount (false)));

                    DBG (
                        "Plugin main bus num input channels: " + juce::String (instance->getMainBusNumInputChannels()));
                    DBG ("Plugin main bus num output channels: "
                         + juce::String (instance->getMainBusNumOutputChannels()));
                } else
                    DBG ("Plugin at key " + std::string (key) + " is empty");
            }
        });
        DBG ("=================================================================================");
    }
}
