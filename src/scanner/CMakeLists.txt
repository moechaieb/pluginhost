project(PluginScanner VERSION "0.0.1")
juce_add_console_app(PluginScanner
    PLUGINHOST_AU TRUE
)

target_sources(PluginScanner
    PRIVATE
    main.cpp
    Worker.h
)

target_link_libraries(PluginScanner
    PRIVATE
    juce::juce_core
    juce::juce_audio_processors
    juce::juce_events
    juce::juce_audio_formats

    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
    juce::juce_recommended_lto_flags
)

target_compile_definitions(PluginScanner
    PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
)
