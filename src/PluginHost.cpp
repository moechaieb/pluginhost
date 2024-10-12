#include "PluginHost.h"

#include "../../lib/instrumentation/Assertion.h"
#include "./KnownPluginListScanner.h"
#include "PluginScan.h"
#include "PluginWindow.h"
#include "choc/containers/choc_Value.h"

namespace timeoffaudio {
    PluginHost::PluginHost (juce::File pLF, ConnectionsRefreshFn cF, GetEnabledParameterFn gEF)
        : pluginListFile (pLF), getConnectionsFor (cF), getEnabledParameterFor (gEF) {
        startTimerHz (120);
        // TODO: this needs to be lifted outside of PluginHost so that it's customizable per
        // plugin and not fixed like it is now
        knownPlugins.setCustomScanner (
            std::make_unique<timeoffaudio::CustomPluginScanner> ([] (const juce::PluginDescription& plugin) {
                if (plugin.isInstrument) return false;
                if (plugin.name == JucePlugin_Name) return false;
                // Add other exclusions here

                return true;
            }));

        if (pluginListFile.exists()) {
            if (auto savedPluginList = juce::parseXML (pluginListFile.loadFileAsString()))
                knownPlugins.recreateFromXml (*savedPluginList);
            else
                PluginHost::changeListenerCallback (&knownPlugins);
        } else
            PluginHost::changeListenerCallback (&knownPlugins);

        formatManager.addFormat (new juce::VST3PluginFormat());
#if JUCE_MAC
        formatManager.addFormat (new juce::AudioUnitPluginFormat());
#endif
        knownPlugins.addChangeListener (this);
    }

    PluginHost::~PluginHost() {
        timeoffaudio_assert (isScanInProgress() == false);
        timeoffaudio_assert (listeners.isEmpty());

        stopTimer();
        abortOngoingScan();

        knownPlugins.removeChangeListener (this);
        for (auto& [_, pluginBox] : nonRealtimeSafePlugins) {
            pluginBox.get().instance->removeListener (this);
            if (const auto pluginWindow = pluginBox->window) pluginWindow->removeComponentListener (this);
        }
    }

    juce::Array<juce::AudioPluginFormat*> PluginHost::getFormats() const { return formatManager.getFormats(); }

    juce::Array<juce::PluginDescription> PluginHost::getAvailablePlugins() const { return knownPlugins.getTypes(); }

    void PluginHost::deletePluginInstance (KeyType key) {
        withWriteAccess ([&] (TransientPluginMap& pluginMap) { deletePluginInstance (pluginMap, key); });
    }
    void PluginHost::deletePluginInstance (TransientPluginMap& pluginMap, KeyType key) {
        // Before deleting a plugin, we first re-enable its plugin parameter
        pluginMap.update (key, [&] (auto pluginBox) {
            return pluginBox.update ([&] (auto plugin) {
                plugin.enabledParameter->setValue (1.f);
                return plugin;
            });
        });
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
            const auto toPluginBox = pluginMap[toKey];
            auto toPluginEnabled   = toPluginBox->enabledParameter->getValue();

            pluginMap.update (toKey, [&] (auto pluginBox) {
                return pluginBox.update ([&] (auto plugin) {
                    plugin.instance = pluginMap[fromKey]->instance;
                    plugin.window   = pluginMap[fromKey]->window;
                    plugin.window->setPluginInstanceKey (toKey);
                    plugin.enabledParameter = getEnabledParameterFor (toKey);
                    plugin.enabledParameter->setValue (fromPluginEnabled);
                    return plugin;
                });
            });

            pluginMap.update (fromKey, [&] (auto pluginBox) {
                return pluginBox.update ([&] (auto plugin) {
                    plugin.instance = toPluginBox->instance;
                    plugin.window   = toPluginBox->window;
                    plugin.window->setPluginInstanceKey (fromKey);
                    plugin.enabledParameter = getEnabledParameterFor (fromKey);
                    plugin.enabledParameter->setValue (toPluginEnabled);
                    return plugin;
                });
            });
        } else {
            // If the destination key is free,
            // move the plugin instance and window to the destination key, and clear the source key
            pluginMap.update (toKey, [&] (auto pluginBox) {
                return pluginBox.update ([&] (auto plugin) {
                    plugin.instance = pluginMap[fromKey]->instance;
                    plugin.window   = pluginMap[fromKey]->window;
                    plugin.window->setPluginInstanceKey (toKey);
                    plugin.enabledParameter = getEnabledParameterFor (toKey);
                    plugin.enabledParameter->setValue (fromPluginEnabled);
                    return plugin;
                });
            });

            pluginMap.erase (fromKey);
        }
    }

    void PluginHost::createPluginInstance (TransientPluginMap& pluginMap,
        const juce::PluginDescription pluginDescription,
        const KeyType key,
        timeoffaudio::PluginWindow::Options windowOptions,
        const juce::MemoryBlock& initialState) {
        for (auto format : formatManager.getFormats()) {
            if (format->getName() == pluginDescription.pluginFormatName) {
                juce::String errorMessage;
                juce::StringPairArray logParameters;
                std::unique_ptr<juce::AudioPluginInstance> instance =
                    format->createInstanceFromDescription (pluginDescription, sampleRate, blockSize, errorMessage);

                if (errorMessage.isNotEmpty() || !instance) {
                    const juce::ScopedReadLock lock (listenersLock);
                    logParameters.set ("success", "false");
                    logParameters.set ("error_message", errorMessage);
                    listeners.call (&Listener::pluginInstanceLoadFailed, key, errorMessage.toStdString());
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

                pluginMap.set (key, immer::box<Plugin> (std::move (instance), nullptr, getEnabledParameterFor (key)));
                if (windowOptions.openAutomatically) openPluginWindow (pluginMap, key, windowOptions);

                juce::Analytics::getInstance()->logEvent ("plugin_load", logParameters);

                break;
            }
        }
    }

    void PluginHost::startScan (const juce::String& format) {
        auto onScanProgress = [this] (float progress01, juce::String formatName, juce::String currentPlugin) {
            const juce::ScopedReadLock lock (listenersLock);
            listeners.call (&Listener::scanProgressed, progress01, formatName, currentPlugin);
        };

        auto onScanFinished = [this]() {
            const juce::ScopedReadLock lock (listenersLock);
            currentScan.reset();
            listeners.call (&Listener::scanFinished);
        };

        for (const auto formatCandidate : formatManager.getFormats())
            if (formatCandidate->getName() == format && formatCandidate->canScanForPlugins()) {
                auto failedToLoadPluginsFolder = pluginListFile.getParentDirectory();

                currentScan = std::make_unique<timeoffaudio::PluginScan> (
                    knownPlugins, *formatCandidate, failedToLoadPluginsFolder, onScanProgress, onScanFinished);
                break;
            }
    }

    void PluginHost::abortOngoingScan() const {
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

    void PluginHost::addPluginHostListener (Listener* listener) {
        juce::ScopedWriteLock lock (listenersLock);
        listeners.add (listener);
    }

    void PluginHost::removePluginHostListener (Listener* listener) {
        juce::ScopedWriteLock lock (listenersLock);
        listeners.remove (listener);
    }

    void PluginHost::changeListenerCallback (juce::ChangeBroadcaster* source) {
        if (source == &knownPlugins) {
            if (const auto savedPluginList = knownPlugins.createXml()) {
                auto saveResult = pluginListFile.create();
                jassert (saveResult.wasOk());
                auto writeSuccessful = pluginListFile.replaceWithText (savedPluginList->toString());
                jassert (writeSuccessful);
            }

            {
                const juce::ScopedWriteLock lock (listenersLock);
                listeners.call (&Listener::availablePluginsUpdated, knownPlugins.getTypes());
            }
        }
    }

    void PluginHost::process (const Plugin& plugin,
        juce::AudioBuffer<float>& allBusesBuffer,
        juce::MidiBuffer& midiMessages) /* context: realtime */ {
        const auto instance = plugin.instance.get();

        // If the plugin instance accepts side chain input, pass it the entire input buffer
        // Otherwise, explicitly get the main bus buffer out of the input buffer
        juce::AudioBuffer<float> bufferToPass = allBusesBuffer;
        if (instance->getChannelCountOfBus (true, 1) == 0) {
            bufferToPass = instance->getBusBuffer (allBusesBuffer, true, 0);
        }

        if (const auto bypassParameter = instance->getBypassParameter(); !bypassParameter) {
            // When getBypassParameter() returns a nullptr, we need to bypass the plugin
            // by calling processBlockBypassed
            instance->processBlockBypassed (bufferToPass, midiMessages);
        } else {
            // When getBypassParameter() returns a valid pointer, we need to
            // set the bypass parameter, and process the plugin normally via processBlock

            // For safety, let's check that we have defined an "enabled parameter" from the host application/plugin
            if (plugin.enabledParameter) {
                const bool isEnabled = plugin.enabledParameter->getValue() >= 0.5f;
                bypassParameter->setValue (!isEnabled);
            } else {
                jassertfalse;
            }

            instance->processBlock (bufferToPass, midiMessages);
        }
    }

    void PluginHost::prepare (const int newSampleRate, const int newBlockSize, juce::AudioPlayHead* newPlayhead) {
        sampleRate = newSampleRate;
        blockSize  = newBlockSize;
        playhead   = newPlayhead;

        withWriteAccess ([&] (const PluginHost::TransientPluginMap& pluginMap) {
            for (auto& [key, pluginBox] : pluginMap) {
                const auto instance = pluginBox.get().instance.get();

                instance->enableAllBuses();
                instance->prepareToPlay (sampleRate, blockSize);
                if (playhead) instance->setPlayHead (playhead);
            }
        });
    }

    void PluginHost::openPluginWindow (TransientPluginMap& pluginMap, std::string key, PluginWindow::Options options) {
        pluginMap.update_if_exists (key, [&] (auto pluginBox) {
            return pluginBox.update ([&] (auto plugin) {
                if (auto pluginWindow = plugin.window) {
                    pluginWindow->toFront (false);
                    pluginWindow->setVisible (true);
                } else {
                    plugin.window = std::make_shared<timeoffaudio::PluginWindow> (
                        key, *plugin.instance, timeoffaudio::PluginWindow::Type::normal, options);
                    plugin.window->addComponentListener (this);
                }

                plugin.lastWindowStateUpdate = PluginWindow::UpdateType::Opened;

                return plugin;
            });
        });
    }

    void PluginHost::openPluginWindow (std::string key, PluginWindow::Options options) {
        withWriteAccess (
            [&] (PluginHost::TransientPluginMap& pluginMap) { openPluginWindow (pluginMap, key, options); });
    }

    void PluginHost::updatePluginWindowBorderColour (TransientPluginMap& pluginMap,
        std::string key,
        juce::Colour colour) {
        // TODO: Currently, on Windows, this causes the plugin window to flicker
        // So I'm disabling this behaviour altogether for now as it's simply a nice-to-have
        // Investigate this further if it ever becomes a bigger issue/concern.
        // Seems like the fix is related to this JUCE forum thread:
        // https://forum.juce.com/t/bug-fixed-in-develop-juce-8-direct2d-standalone-app-repaint-flashing/63693
#if !JUCE_WINDOWS
        pluginMap.update_if_exists (key, [&] (auto pluginBox) {
            return pluginBox.update ([&] (auto plugin) {
                if (auto pluginWindow = plugin.window) pluginWindow->setBackgroundColour (colour);
                return plugin;
            });
        });
#endif
    }

    void PluginHost::updatePluginWindowBorderColour (std::string key, juce::Colour colour) {
        withWriteAccess ([&] (PluginHost::TransientPluginMap& pluginMap) {
            updatePluginWindowBorderColour (pluginMap, key, colour);
        });
    }

    void PluginHost::closePluginWindow (std::string key) {
        withWriteAccess ([&] (PluginHost::TransientPluginMap& pluginMap) { closePluginWindow (pluginMap, key); });
    }

    void PluginHost::closePluginWindow (TransientPluginMap& pluginMap, std::string key) {
        pluginMap.update_if_exists (key, [&] (auto pluginBox) {
            return pluginBox.update ([&] (auto plugin) {
                if (auto pluginWindow = plugin.window) {
                    pluginWindow->setVisible (false);
                    plugin.lastWindowStateUpdate = PluginWindow::UpdateType::Closed;
                }
                return plugin;
            });
        });
    }

    void PluginHost::closeAllPluginWindows() {
        withWriteAccess ([&] (PluginHost::TransientPluginMap& pluginMap) {
            for (const auto& [key, snd] : pluginMap) closePluginWindow (pluginMap, key);
        });
    }

    void PluginHost::bringPluginWindowToFront (KeyType key) {
        withWriteAccess (
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

    choc::value::Value PluginHost::getPluginState (KeyType key, const PluginMap& pluginMap) const {
        auto pluginState = choc::value::createObject ("PluginState");

        if (const auto pluginBox = pluginMap.find (key)) {
            const auto pluginDescriptionXml = pluginBox->get().instance->getPluginDescription().createXml();
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

            pluginState.addMember ("window_xPos", 0);
            pluginState.addMember ("window_yPos", 0);
            if (const auto window = pluginBox->get().window) {
                pluginState.setMember ("window_xPos", window->getPosition().x);
                pluginState.setMember ("window_yPos", window->getPosition().y);
            }
        }

        return pluginState;
    }

    choc::value::Value PluginHost::getAllPluginsState() const {
        choc::value::Value allPluginsState = choc::value::createEmptyArray();

        for (auto& [key, pluginBox] : nonRealtimeSafePlugins)
            allPluginsState.addArrayElement (getPluginState (key, nonRealtimeSafePlugins));

        return allPluginsState;
    }

    void PluginHost::loadPluginFromState (TransientPluginMap& pluginMap, const choc::value::Value& pluginState) {
        const auto key = pluginState["key"].toString();

        if (const auto pluginDescriptionXml = juce::XmlDocument::parse (pluginState["description"].toString())) {
            if (juce::PluginDescription pluginDescription; pluginDescription.loadFromXml (*pluginDescriptionXml)) {
                juce::MemoryBlock stateToLoad;
                stateToLoad.fromBase64Encoding (pluginState["encoded_state"].toString());

                PluginWindow::Options options;
                options.openAutomatically = false;
                options.xPos              = pluginState["window_xPos"].getWithDefault (0);
                options.yPos              = pluginState["window_yPos"].getWithDefault (0);
                createPluginInstance (pluginMap, pluginDescription, key, options, stateToLoad);

                return;
            }
        }

        timeoffaudio_assert (false);
    }

    void PluginHost::loadAllPluginsFromState (const choc::value::Value& allPluginsState) {
        withWriteAccess (
            [&] (TransientPluginMap& pluginMap) {
                for (const auto pluginState : allPluginsState)
                    loadPluginFromState (pluginMap, choc::value::Value (pluginState));
            },
            PostUpdateAction::RefreshConnections);
    }

    juce::Array<juce::AudioProcessorParameter*> PluginHost::getParameters (KeyType key) const {
        juce::Array<juce::AudioProcessorParameter*> parameters;

        withReadonlyAccess ([&] (const PluginMap& pluginMap) {
            // Filter out parameters that start with "midi cc", "internal", or "bypass", etc
            if (const auto pluginBox = pluginMap.find (key)) {
                parameters = pluginBox->get().instance->getParameters();

                parameters.removeIf ([] (const juce::AudioProcessorParameter* param) {
                    for (const auto prefix : { "midi cc", "internal", "bypass", "reserved", "in", "out", "-" })
                        if (param->getName (1024).toLowerCase().startsWith (prefix)) return true;

                    return false;
                });
            }
        });

        return parameters;
    }

    juce::AudioProcessorParameter* PluginHost::getParameter (KeyType key, int parameterIndex) const {
        // TODO: this is not very efficient, but it's the simplest way to get the key
        // figure out how to get the key from the PluginMap without scanning the whole map
        const auto pluginBox = nonRealtimeSafePlugins.find (key);
        if (!pluginBox) return nullptr;

        const auto pluginInstance = pluginBox->get().instance.get();
        if (!pluginInstance) return nullptr;

        return pluginInstance->getHostedParameter (parameterIndex);
    }

    void PluginHost::beginChangeGestureForParameter (const KeyType& key, const int parameterIndex) const {
        if (const auto parameter = getParameter (key, parameterIndex)) return parameter->beginChangeGesture();
        // TODO: add error notification here
    }

    void PluginHost::endChangeGestureForParameter (const KeyType& key, const int parameterIndex) const {
        if (const auto parameter = getParameter (key, parameterIndex)) return parameter->endChangeGesture();
        // TODO: add error notification here
    }

    void PluginHost::setValueForParameter (const KeyType& key, const int parameterIndex, const float value) const {
        if (const auto parameter = getParameter (key, parameterIndex)) return parameter->setValue (value);
        // TODO: add error notification here
    }

    juce::String PluginHost::getDisplayValueForParameter (const KeyType& key,
        const int parameterIndex,
        const float value) const {
        if (const auto parameter = getParameter (key, parameterIndex)) return parameter->getText (value, 1024);

        return {};
    }

    void PluginHost::audioProcessorParameterChanged (juce::AudioProcessor* processor,
        int parameterIndex,
        float newValue) {
        const auto pluginInstance = dynamic_cast<juce::AudioPluginInstance*> (processor);
        if (!pluginInstance) return;

        withReadonlyAccess ([&] (const PluginMap& pluginMap) {
            // TODO: this is not very efficient, but it's the simplest way to get the key
            // figure out how to get the key from the PluginMap without scanning the whole map
            for (auto [key, pluginBox] : pluginMap) {
                if (pluginBox.get().instance.get() == pluginInstance) {
                    {
                        const juce::ScopedReadLock lock (listenersLock);
                        listeners.call (&Listener::pluginInstanceParameterChanged, key, parameterIndex, newValue);
                    }

                    break;
                }
            }
        });
    }

    void PluginHost::audioProcessorChanged (juce::AudioProcessor*, const juce::AudioProcessor::ChangeDetails& details) {
        if (!details.latencyChanged) return;

        {
            const juce::ScopedReadLock lock (listenersLock);
            listeners.call (&Listener::latenciesChanged);
        }
    }

    void PluginHost::audioProcessorParameterChangeGestureBegin (juce::AudioProcessor*, int) {}
    void PluginHost::audioProcessorParameterChangeGestureEnd (juce::AudioProcessor*, int) {}

    void PluginHost::debugPrintState() const {
        DBG ("=============================== Plugin Host State ===============================");
        withReadonlyAccess ([&] (const PluginMap& pluginMap) {
            DBG ("Number of plugins: " + juce::String (pluginMap.size()));
            for (const auto& [key, pluginBox] : pluginMap) {
                if (const auto instance = pluginBox->instance) {
                    DBG ("Plugin at key " + std::string (key) + " is " + instance->getName().toStdString());
                    if (const auto enabledParam = pluginBox->enabledParameter) {
                        DBG ("Plugin linked enabled parameter UID: " + juce::String (enabledParam->getParameterID()));
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
