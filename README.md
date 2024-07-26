## time off audio plugin host

The `timeoffaudio::PluginHost` class provides primary interface for managing and interacting with audio plugins within a host application.

### basic architecture

The architecture of the plugin host revolves around several key classes and functionalities:

- **PluginHost**: Manages the lifecycle and operations of audio plugins.
- **PluginScan**: Handles the scanning of available plugins and manages asynchronous loading.
- **Plugin**: Represents a single plugin instance with associated functionalities like window management and parameter control.

### usage

All you need to do is inherit from the `timeoffaudio::PluginHost` class.

```cpp
class MyPluginHost : public timeoffaudio::PluginHost {
public:
    MyPluginHost() : PluginHost() {
        // Initialization code here
    }

    // Implement required methods
};
```

### data model

The data model of the plugin host is built around the concept of plugins and their interactions. Key components include:

- **PluginMap**: Stores all active plugin instances. This map is an immutable data data structure provided by the `immer` library, which allows for efficient and safe sharing of state across threads without needing locks. The map uses unique keys (`KeyType` which is typically a string) to identify each plugin instance. These keys serve as unique identifiers for each plugin within the host environment, allowing the implementer to manage and reference plugins effectively within their plugin graph or chain. The keys can be used to retrieve specific plugin instances, manage their state, or manipulate their position and connections within the overall plugin architecture.
- **TransientPluginMap**: Used for temporary changes to the plugin instances. It is a mutable version of `PluginMap` that allows changes to be made before being committed back to the immutable `PluginMap`.
- **Plugin**: Encapsulates an audio plugin instance along with its GUI window and connection information.

### access patterns

- **withWriteAccess**: Functions that modify the `TransientPluginMap` must be wrapped in a `withWriteAccess` call. This ensures that changes are made within a controlled scope, and the modified map is safely committed back as an immutable `PluginMap` at the end of the block. This method is used for operations like creating, deleting, or moving plugin instances.


```cpp
void PluginHost::deletePluginInstance (KeyType key) {
    withWriteAccess ([&] (TransientPluginMap& pluginMap) { deletePluginInstance (pluginMap, key); });
}
```

Most operations that mutate the plugin map have a version that takes a `TransientPluginMap` as an argument, which allows them to be chained arbitrarily and committed back to the immutable `PluginMap` at the end of the block in a transactional way.

- **withReadOnlyAccess**: For operations that only need to read from the `PluginMap` without modifying it, `withReadOnlyAccess` is used. This method passes a constant reference of the `PluginMap` to the provided function, ensuring that no modifications can occur.

```cpp
void PluginHost::withReadOnlyAccess (std::function<void (const PluginMap)> accessor) const {
    accessor (plugins);
}
```

These access patterns ensure thread safety and data integrity by controlling how and when changes to the plugin map are made and committed.

### events / callbacks

### events / callbacks

The `PluginHost::Listener` interface provides a set of callbacks that allow for real-time monitoring and response to various events within the plugin host environment. These callbacks are crucial for applications that need to react to changes in plugin status, configuration, or interaction. Here is a breakdown of each callback and its functionality:

- **scanProgressed**: Triggered during a plugin scan to provide updates on the scan's progress. It reports the percentage completed, the format of the plugin being scanned, and the name of the current plugin. This is useful for updating a UI progress bar or providing user feedback during long operations.

- **scanFinished**: Called when a plugin scan completes. This can be used to perform cleanup, update UI elements, or trigger other processes that depend on the completion of the scan.

- **availablePluginsUpdated**: Fired when the list of available plugins is updated. This could happen after a scan or when plugins are added or removed manually. It provides an updated list of plugin descriptions, allowing the host application to refresh its UI or internal data structures.

- **pluginInstanceLoadSuccessful**: Occurs when a plugin instance is successfully loaded. It provides the unique identifier for the plugin and the instance itself, enabling the application to manage the plugin directly or update related data structures.

- **pluginInstanceLoadFailed**: Triggered if a plugin instance fails to load. It includes the unique identifier and a string describing the error, which can be logged or displayed to the user.

- **pluginInstanceUpdated**: Called when an existing plugin instance undergoes a significant change that might affect its operation or representation in the UI. This includes changes like parameter updates or internal state changes.

- **pluginInstanceDeleted**: Occurs when a plugin instance is removed from the system. It provides the unique identifier and the instance itself, which can be used to update UI elements or internal mappings.

- **pluginInstanceParameterChanged**: Fired when a parameter within a plugin instance changes. It reports the unique identifier of the plugin, the index of the parameter that changed, and the new value. This is essential for keeping UI elements like sliders or knobs in sync with the plugin's state.

These callbacks form a comprehensive system for managing and responding to events within the plugin host, ensuring that applications can maintain accurate and up-to-date information about their plugins and react promptly to changes.

### plugin discovery

Plugin discovery is managed by the `PluginScan` class, which supports asynchronous scanning of plugins across multiple threads. This class is integral to identifying available plugins and updating the host system with new or removed plugins. It provides several key functions and callbacks to enhance interaction and responsiveness:

- **startScan**: Initiates the scanning process for a specified plugin format. This function is useful for starting the scan based on user input or system initialization.
- **abortOngoingScan**: Allows for the cancellation of any currently running scan, providing a way to stop the operation gracefully if needed.
- **isScanInProgress**: Returns a boolean indicating whether a scan is currently active, which can be used to update UI elements or manage system states.
- **getScanStatus**: Provides a snapshot of the current scan progress, including which plugins are being scanned and the overall progress percentage.

#### callbacks

- **scanProgressed**: This callback is triggered during the scanning process to provide real-time updates on the progress. It includes parameters such as the progress percentage, the name of the plugin format being scanned, and the current plugin being processed.
- **scanFinished**: Called when a scan completes, this callback can be used to trigger post-scan processes such as refreshing the UI or updating internal data structures.
- **availablePluginsUpdated**: Triggered when the list of available plugins is updated (either by adding new plugins or removing unavailable ones), this callback helps maintain a current view of all plugins that the system can utilize.

These functions and callbacks ensure that the plugin discovery process is robust, responsive, and integrated with the host application's overall functionality, allowing for dynamic updates and management of plugin resources.

### coming soon

Plugin graph support: define arbitrary plugin graphs, and PluginHost will help play it for you. At the moment, it's really up to the implementer to manage how they playback their plugin graph, by calling `withReadOnlyAccess` and then iterating over the plugins in the graph in the right order.

For example, in dime, since I'm playing back N parallel plugin chains, I'm able to achieve that with:

```cpp
PluginHost::withReadOnlyAccess ([&] (const PluginHost::PluginMap& plugins) {
    for (size_t i = 0; i < PLUGIN_GRAPH.size(); ++i)
        for (size_t j = 0; j < PLUGIN_GRAPH[i].size(); ++j) {
            if (plugins.count (PLUGIN_GRAPH[i][j]) == 0) break;

            plugins.at (PLUGIN_GRAPH[i][j])
                ->instance->processBlock (splitBuffersWithSidechain[i], midiBuffer);
        }
});
```
