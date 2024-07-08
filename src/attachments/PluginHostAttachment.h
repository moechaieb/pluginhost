#pragma once
#include "../PluginHost.h"
#include "choc/containers/choc_Value.h"
#include "choc/text/choc_JSON.h"
#include <imagiro_processor/imagiro_processor.h>
#include <imagiro_webview/imagiro_webview.h>

namespace timeoffaudio {
    class PluginHostAttachment : public imagiro::WebUIAttachment, private timeoffaudio::PluginHost::Listener {
    public:
        PluginHostAttachment (imagiro::WebProcessor& p, imagiro::WebViewManager& w, timeoffaudio::PluginHost& h)
            : imagiro::WebUIAttachment (p, w), pluginHost (h) {}

        void addListeners() override { pluginHost.addPluginHostListener (this); }

        ~PluginHostAttachment() override { pluginHost.removePluginHostListener (this); }

        void scanProgressed (float progress01, juce::String formatName, juce::String currentPlugin) override {
            auto status = choc::value::createObject ("scan_progress");
            status.addMember ("inProgress", true);
            status.addMember ("progress", progress01);
            status.addMember ("currentPlugin", currentPlugin.toStdString());
            status.addMember ("format", formatName.toStdString());

            this->webViewManager.evaluateJavascript (
                "window.ui.onPluginScanProgress(" + choc::json::toString (status) + ")");
        }

        void scanFinished() override { this->webViewManager.evaluateJavascript ("window.ui.onPluginScanFinished()"); }

        void availablePluginsUpdated (const juce::Array<juce::PluginDescription>& pluginDescriptions) override {
            auto availablePlugins = choc::value::createEmptyArray();
            for (auto plugin : pluginDescriptions)
                availablePlugins.addArrayElement (buildPluginDescriptionValue (plugin));

            this->webViewManager.evaluateJavascript (
                "window.ui.onAvailablePluginsUpdated(" + choc::json::toString (availablePlugins) + ")");
        }

        void pluginInstanceLoadSuccessful (std::string uuid, juce::AudioPluginInstance* pluginInstance) override {
            this->webViewManager.evaluateJavascript (
                "window.ui.onPluginInstanceLoadSuccess('" + uuid + "', "
                + choc::json::toString (buildPluginParametersListValue (uuid, pluginInstance)) + ")");
        }

        void pluginInstanceLoadFailed (std::string /*uuid*/, std::string /*error*/) override {
            // TODO: implement this
            // juce::String s = "window.ui.onPluginInstanceLoadFailure(";
            // this->webViewManager.evaluateJavascript (s.toStdString());
        }

        void pluginInstanceUpdated (std::string uuid, juce::AudioPluginInstance* plugin) override {
            auto pluginInstanceValue = buildPluginDescriptionValue (plugin->getPluginDescription());
            pluginInstanceValue.addMember ("key", uuid);
            pluginInstanceValue.addMember ("parameters", buildPluginParametersListValue (uuid, plugin));

            this->webViewManager.evaluateJavascript (
                "window.ui.onPluginInstanceUpdated(" + choc::json::toString (pluginInstanceValue) + ")");
        }

        void pluginInstanceDeleted (std::string uuid, juce::AudioPluginInstance* /*plugin*/) override {
            this->webViewManager.evaluateJavascript ("window.ui.onPluginInstanceDeleted('" + uuid + "')");
        }

        void pluginInstanceParameterChanged (PluginHost::KeyType uuid, int parameterIndex, float newValue) override {
            auto hostedParameterKey = uuid + "." + std::to_string(parameterIndex);
            this->webViewManager.evaluateJavascript ("window.ui.onPluginInstanceParameterUpdated('" + hostedParameterKey + "',"
                                                     + std::to_string (newValue) + ")");
        }

        void addBindings() override {
            webViewManager.bind (
                "juce_startPluginScan", [&] (const choc::value::ValueView& args) -> choc::value::Value {
                    auto formatName = args[0].toString();
                    pluginHost.startScan (formatName);
                    return {};
                });

            webViewManager.bind (
                "juce_abortOngoingPluginScan", [&] (const choc::value::ValueView& /*args*/) -> choc::value::Value {
                    pluginHost.abortOngoingScan();
                    return {};
                });

            webViewManager.bind ("juce_getInProgressScanStatus",
                [&] (const choc::value::ValueView&) -> choc::value::Value { return pluginHost.getScanStatus(); });

            webViewManager.bind (
                "juce_getAvailablePluginFormats", [&] (const choc::value::ValueView&) -> choc::value::Value {
                    auto result = choc::value::createEmptyArray();

                    for (auto format : pluginHost.getFormats())
                        result.addArrayElement (format->getName().toStdString());

                    return result;
                });

            webViewManager.bind ("juce_getAvailablePlugins", [&] (const choc::value::ValueView&) -> choc::value::Value {
                auto result = choc::value::createEmptyArray();

                for (auto plugin : pluginHost.getAvailablePlugins())
                    result.addArrayElement (buildPluginDescriptionValue (plugin));

                return result;
            });

            webViewManager.bind (
                "juce_clearAllAvailablePlugins", [&] (const choc::value::ValueView&) -> choc::value::Value {
                    pluginHost.clearAllAvailablePlugins();

                    return {};
                });

            webViewManager.bind (
                "juce_clearAvailablePlugins", [&] (const choc::value::ValueView& args) -> choc::value::Value {
                    auto pluginsArray = args[0];
                    for (size_t i = 0; i < pluginsArray.size(); ++i) {
                        auto pluginDescription = findPluginDescriptionFromValue (pluginsArray[static_cast<int> (i)]);
                        if (pluginDescription) {
                            pluginHost.clearAvailablePlugin (*pluginDescription);
                        }
                    }

                    return {};
                });

            webViewManager.bind (
                "juce_getPluginInstances", [&] (const choc::value::ValueView& /*args*/) -> choc::value::Value {
                    auto pluginInstancesValue = choc::value::createEmptyArray();

                    pluginHost.withReadOnlyAccess ([&] (const PluginHost::PluginMap& pluginInstances) {
                        for (const auto& [key, plugin] : pluginInstances) {
                            auto pluginInstanceValue =
                                buildPluginDescriptionValue (plugin->instance->getPluginDescription());

                            pluginInstanceValue.addMember ("key", key);
                            pluginInstancesValue.addArrayElement (pluginInstanceValue);
                        }
                    });

                    return pluginInstancesValue;
                });

            webViewManager.bind (
                "juce_getPluginInstanceParameters", [&] (const choc::value::ValueView& args) -> choc::value::Value {
                    auto key                      = args[0].toString();
                    auto pluginInstanceParameters = choc::value::createEmptyArray();

                    pluginHost.withReadOnlyAccess ([&] (const PluginHost::PluginMap& pluginInstances) {
                        auto pluginBox = pluginInstances.find (key);
                        if (!pluginBox) return;

                        auto pluginInstance = pluginBox->get().instance.get();
                        if (!pluginInstance) return;

                        pluginInstanceParameters =
                                buildPluginParametersListValue (key, pluginInstance);
                    });

                    return pluginInstanceParameters;
                });

            webViewManager.bind (
                "juce_requestToLoadPlugin", [&] (const choc::value::ValueView& args) -> choc::value::Value {
                    // Async: Request the pluginHost to create a new plugin instance
                    auto pluginDescription = findPluginDescriptionFromValue (args[0]);
                    auto key               = args[1].toString();

                    pluginHost.withWriteAccess<PluginHost::PostUpdateAction::RefreshConnections> (
                        [&] (PluginHost::TransientPluginMap& plugins) {
                            pluginHost.createPluginInstance (plugins, *pluginDescription, key);
                        });

                    return {};
                });

            webViewManager.bind ("juce_deletePluginInstanceAndUpdateKeys",
                [&] (const choc::value::ValueView& args) -> choc::value::Value {
                    auto keyToDelete   = args[0].toString();
                    auto keyUpdateList = args[1];

                    pluginHost.withWriteAccess ([&] (PluginHost::TransientPluginMap& plugins) {
                        pluginHost.deletePluginInstance (plugins, keyToDelete);
                        for (auto keyUpdate : keyUpdateList)
                            pluginHost.movePluginInstance (
                                plugins, keyUpdate["fromKey"].toString(), keyUpdate["toKey"].toString());
                    });

                    return {};
                });

            webViewManager.bind (
                "juce_updatePluginInstanceKey", [&] (const choc::value::ValueView& args) -> choc::value::Value {
                    auto fromKey = args[0].toString();
                    auto toKey   = args[1].toString();

                    pluginHost.movePluginInstance (fromKey, toKey);

                    return {};
                });

            webViewManager.bind (
                "juce_openPluginInstanceWindow", [&] (const choc::value::ValueView& args) -> choc::value::Value {
                    // Sync: Request the pluginHost to open a window showing the plugin's UI
                    auto key = args[0].toString();

                    if (args.size() == 3) {
                        auto xPos = args[1].getInt64();
                        auto yPos = args[2].getInt64();
                        pluginHost.openPluginWindow (key, (int) xPos, (int) yPos);
                    } else
                        pluginHost.openPluginWindow (key);

                    return {};
                });

            webViewManager.bind (
                "juce_highlightPluginWindow", [&] (const choc::value::ValueView& args) -> choc::value::Value {
                    auto key    = args[0].toString();
                    auto colour = juce::Colour::fromString (args.size() == 2 ? args[1].toString() : "ff000000");

                    pluginHost.withWriteAccess<PluginHost::PostUpdateAction::None> (
                        [&] (PluginHost::TransientPluginMap& plugins) {
                            pluginHost.updatePluginWindowBorderColour (plugins, key, colour);
                            pluginHost.bringPluginWindowToFront (plugins, key);
                        });

                    return {};
                });

            webViewManager.bind ("juce_startPluginInstanceParameterGesture",
                [&] (const choc::value::ValueView& args) -> choc::value::Value {
                    auto paramKey        = args[0].toString();
                    auto splittedParamKey = choc::text::splitString(paramKey, '.', false);
                    auto pluginKey = splittedParamKey[0];
                    auto paramIndex = std::stoi(splittedParamKey.at(1));
                    pluginHost.beginChangeGestureForParameter (pluginKey, (int) paramIndex);
                    return {};
                });

            webViewManager.bind ("juce_endPluginInstanceParameterGesture",
                [&] (const choc::value::ValueView& args) -> choc::value::Value {
                    auto paramKey        = args[0].toString();
                    auto splittedParamKey = choc::text::splitString(paramKey, '.', false);
                    auto pluginKey = splittedParamKey[0];
                    auto paramIndex = std::stoi(splittedParamKey.at(1));
                    pluginHost.endChangeGestureForParameter (pluginKey, (int) paramIndex);
                    return {};
                });

            webViewManager.bind (
                "juce_updatePluginInstanceParameter", [&] (const choc::value::ValueView& args) -> choc::value::Value {
                    auto paramKey        = args[0].toString();
                    auto splittedParamKey = choc::text::splitString(paramKey, '.', false);
                    auto pluginKey = splittedParamKey[0];
                    auto paramIndex = std::stoi(splittedParamKey.at(1));
                    auto value      = args[1].getFloat64();

                    pluginHost.setValueForParameter (pluginKey, (int) paramIndex, (float) value);

                    return choc::value::Value (value);
                });

            webViewManager.bind ("juce_getPluginInstanceParameterDisplayValue",
                [&] (const choc::value::ValueView& args) -> choc::value::Value {
                    auto paramKey        = args[0].toString();
                    auto splittedParamKey = choc::text::splitString(paramKey, '.', false);
                    auto pluginKey = splittedParamKey[0];
                    auto paramIndex = std::stoi(splittedParamKey.at(1));
                    auto value      = 0.f;

                    if (auto parameter = pluginHost.getParameters (pluginKey)[(int) paramIndex])
                        value = parameter->getValue();
                    if (args.size() > 1 && !args[1].isVoid()) value = args[1].getWithDefault (0.f);

                    auto displayValue = choc::value::createObject ("DisplayValue");
                    displayValue.addMember ("value",
                        pluginHost.getDisplayValueForParameter (pluginKey, (int) paramIndex, (float) value).toStdString());

                    return displayValue;
                });
        }

    private:
        choc::value::Value buildPluginDescriptionValue (const juce::PluginDescription& pluginDescription) {
            auto result = choc::value::createObject ("Plugin");

            result.addMember ("name", pluginDescription.name.toStdString());
            result.addMember ("descriptiveName", pluginDescription.descriptiveName.toStdString());
            result.addMember ("format", pluginDescription.pluginFormatName.toStdString());
            result.addMember ("category", pluginDescription.category.toStdString());
            result.addMember ("company", pluginDescription.manufacturerName.toStdString());
            result.addMember ("version", pluginDescription.version.toStdString());
            result.addMember ("fileOrIdentifier", pluginDescription.fileOrIdentifier.toStdString());
            result.addMember ("uniqueId", pluginDescription.uniqueId);
            result.addMember ("isInstrument", pluginDescription.isInstrument);
            result.addMember ("lastFileModTime", pluginDescription.lastFileModTime.toISO8601 (true).toStdString());
            result.addMember (
                "lastInfoUpdateTime", pluginDescription.lastInfoUpdateTime.toISO8601 (true).toStdString());
            result.addMember ("numInputChannels", pluginDescription.numInputChannels);
            result.addMember ("numOutputChannels", pluginDescription.numOutputChannels);
            result.addMember ("hasSharedContainer", pluginDescription.hasSharedContainer);
            result.addMember ("hasARAExtension", pluginDescription.hasARAExtension);

            return result;
        }

        juce::PluginDescription* findPluginDescriptionFromValue (
            const choc::value::ValueView& pluginDescriptionAsValue) {
            auto availablePlugins = pluginHost.getAvailablePlugins();
            for (auto& pluginDescription : availablePlugins) {
                if (pluginDescription.name.toStdString() != pluginDescriptionAsValue["name"].toString()) continue;

                if (pluginDescription.manufacturerName.toStdString() != pluginDescriptionAsValue["company"].toString())
                    continue;

                if (pluginDescription.version.toStdString() != pluginDescriptionAsValue["version"].toString()) continue;

                if (pluginDescription.pluginFormatName.toStdString() != pluginDescriptionAsValue["format"].toString())
                    continue;

                return &pluginDescription;
            }

            return nullptr;
        }

        choc::value::Value buildPluginParametersListValue (const PluginHost::KeyType& key,
            const juce::AudioPluginInstance* pluginInstance) {
            auto result = choc::value::createEmptyArray();
            if (!pluginInstance) return result;

            // Filter out parameters that start with "midi cc", "internal", or "bypass", etc
            for (auto param : pluginInstance->getParameters()) {
                auto skip = false;

                for (auto prefix : {"midi cc", "internal", "bypass"})
                    if (param->getName(1024).toLowerCase().startsWith(prefix)) {
                        skip = true;
                        break;
                    }

                if (skip) continue;

                result.addArrayElement (buildPluginParameterValue (key, *param));
            }

            return result;
        }

        choc::value::Value buildPluginParameterValue (const PluginHost::KeyType& key,
            const juce::AudioProcessorParameter& pluginParameter) {
            auto result = choc::value::createObject ("Parameter");

            auto hostedParameterKey = key + "." + std::to_string(pluginParameter.getParameterIndex());
            result.addMember ("uid", hostedParameterKey);
            result.addMember ("name", pluginParameter.getName (1024).toStdString());
            result.addMember ("value01", pluginParameter.getValue());
            result.addMember ("defaultVal01", pluginParameter.getDefaultValue());
            result.addMember ("normalizedStep", (1.0 / (pluginParameter.getNumSteps() - 1)));

            auto range = choc::value::createObject ("Range");
            range.addMember ("min", 0);
            range.addMember ("max", 1);
            range.addMember ("step", 1.0 / (pluginParameter.getNumSteps() - 1));
            result.addMember ("range", range);

            auto choices = choc::value::createEmptyArray();
            for (auto valueString : pluginParameter.getAllValueStrings())
                choices.addArrayElement (valueString.toStdString());
            result.addMember ("choices", choices);

            return result;
        }

        timeoffaudio::PluginHost& pluginHost;
    };
}
