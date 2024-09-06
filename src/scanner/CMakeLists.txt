cmake_minimum_required(VERSION 3.15)
project(PluginScanner VERSION "0.0.3")

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../juce ${CMAKE_BINARY_DIR}/juce)

if(APPLE)
    set (CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum OS X deployment version" FORCE)
    set (CMAKE_OSX_ARCHITECTURES "x86_64; arm64" CACHE STRING "Architectures" FORCE)
endif()

juce_add_gui_app(PluginScanner
PRODUCT_NAME "time off audio plugin scanner"
    BUNDLE_ID "com.timeoffaudio.pluginscanner"
    PLUGINHOST_AU TRUE
    COMPANY_NAME "time off audio"
    COMPANY_WEBSITE "https://timeoff.audio"
    COMPANY_EMAIL "support@timeoff.audio"
    DESCRIPTION "Utility executable for finding plugins on the system"
    HARDENED_RUNTIME_ENABLED TRUE
    HARDENED_RUNTIME_OPTIONS com.apple.security.cs.allow-unsigned-executable-memory com.apple.security.cs.disable-library-validation com.apple.security.get-task-allow
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
    CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
)
