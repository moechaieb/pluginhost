## time off audio plugin host

The `timeoffaudio::PluginHost` class provides the primary interface for managing and interacting with audio plugins within a host application.

### basic architecture

The architecture of the plugin host revolves around several key classes and functionalities:

- **PluginHost**: Manages the lifecycle and operations of audio plugins.
- **PluginScan**: Handles the scanning of available plugins and manages asynchronous loading.
- **Plugin**: Represents a single plugin instance with associated functionalities like window management and parameter control.

### usage

Initialize the `timeoffaudio::PluginHost` as a member variable in your class:

```cpp
class MyAudioProcessor {
public:
    MyAudioProcessor() : pluginHost(juce::File("path/to/plugin/list/file")) {
        // Initialization code here
    }

private:
    timeoffaudio::PluginHost pluginHost;
    // Other member variables
};
```

### data model

The data model of the plugin host is built around the concept of plugins and their interactions. Key components include:

- **PluginMap**: Stores all active plugin instances. This map is an immutable data structure provided by the `immer` library, which allows for efficient and safe sharing of state across threads without needing locks. The map uses unique keys (`KeyType` which is typically a string) to identify each plugin instance.
- **TransientPluginMap**: Used for temporary changes to the plugin instances. It is a mutable version of `PluginMap` that allows changes to be made before being committed back to the immutable `PluginMap`.
- **Plugin**: Encapsulates an audio plugin instance along with its GUI window and connection information.

### plugin discovery

Plugin discovery is managed by the `PluginScan` class, which supports asynchronous scanning of plugins across multiple threads. Key functions include:

- **startScan**: Initiates the scanning process for a specified plugin format.
- **abortOngoingScan**: Allows for the cancellation of any currently running scan.
- **isScanInProgress**: Returns a boolean indicating whether a scan is currently active.
- **getScanStatus**: Provides a snapshot of the current scan progress.

### access patterns

The PluginHost class provides thread-safe access patterns for reading and writing plugin data. * A fundamental assumption made throughout the design is that the plugin map will only be modified by a single, non-realtime thread (typically the UI/message thread).*

- **withWriteAccess**: Used for operations that modify the plugin map from the non-realtime thread. It ensures changes are made within a controlled scope, safely committed back to the immutable `PluginMap`, then notifies listeners of the changes.

```cpp
pluginHost.withWriteAccess([&] (PluginHost::TransientPluginMap& pluginMap) {
    // Modify pluginMap here
});
```

- **withReadonlyAccess**: Used for read-only operations on the plugin map from the non-realtime thread. It provides a consistent snapshot of the plugin state.

```cpp
pluginHost.withReadonlyAccess([&] (const PluginHost::PluginMap& pluginMap) {
    // Read from pluginMap here
});
```

- **withRealtimeAccess**: Used for accessing the plugin map from the realtime thread. It ensures thread-safe and realtime-safe access to the latest plugin state (as read-only).

```cpp
pluginHost.withRealtimeAccess([&] (const PluginHost::PluginMap& pluginMap) {
    // Access pluginMap in realtime context here
});
```

### events / callbacks

The `PluginHost::Listener` interface provides callbacks for various events:

- **scanProgressed**: Updates on plugin scan progress.
- **scanFinished**: Called when a plugin scan completes.
- **availablePluginsUpdated**: Fired when the list of available plugins is updated.
- **pluginInstanceLoadSuccessful**: Occurs when a plugin instance is successfully loaded.
- **pluginInstanceLoadFailed**: Triggered if a plugin instance fails to load.
- **pluginInstanceUpdated**: Called when an existing plugin instance undergoes a significant change.
- **pluginInstanceDeleted**: Occurs when a plugin instance is removed.
- **pluginInstanceParameterChanged**: Fired when a parameter within a plugin instance changes.
- **latenciesChanged**: Called when the latency of one or more plugins changes.
- **pluginWindowUpdated**: Triggered when a plugin window is opened or closed.

### plugin windows

The `PluginWindow` class manages the GUI for individual plugins. It provides options for customizing the appearance and behavior of plugin windows.

### coming soon

Future updates may include built-in support for defining and playing arbitrary plugin graphs.
