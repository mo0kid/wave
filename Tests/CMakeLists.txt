add_executable(WaveCoreTests WaveCoreTests.cpp)
target_link_libraries(WaveCoreTests
    PRIVATE
        WaveCore
        juce::juce_audio_formats
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

add_test(NAME WaveCoreTests COMMAND WaveCoreTests)
if(MSVC)
    # The DSP and firmware test fixtures need more than Windows' 1 MB default.
    target_link_options(WaveCoreTests PRIVATE /STACK:8388608)
endif()
set_tests_properties(WaveCoreTests PROPERTIES TIMEOUT 120)

find_package(Threads REQUIRED)
add_executable(M68000ThreadTests M68000ThreadTests.cpp ../Source/Firmware/M68000.cpp)
target_include_directories(M68000ThreadTests PRIVATE ../Source)
target_link_libraries(M68000ThreadTests PRIVATE WaveM68k Threads::Threads)
add_test(NAME M68000ThreadTests COMMAND M68000ThreadTests)
set_tests_properties(M68000ThreadTests PROPERTIES TIMEOUT 60)

add_executable(WaveHostStateTests WaveHostStateTests.cpp)
target_include_directories(WaveHostStateTests PRIVATE ../Source)
target_link_libraries(WaveHostStateTests PRIVATE WaveEmulation juce::juce_audio_utils Threads::Threads)
add_test(NAME WaveHostStateTests COMMAND WaveHostStateTests)
set_tests_properties(WaveHostStateTests PROPERTIES TIMEOUT 120)

add_executable(WavePublicEditorTests WavePublicEditorTests.cpp)
target_include_directories(WavePublicEditorTests PRIVATE ../Source)
target_link_libraries(WavePublicEditorTests PRIVATE WaveEmulation juce::juce_audio_utils)
add_test(NAME WavePublicEditorTests COMMAND WavePublicEditorTests)
set_tests_properties(WavePublicEditorTests PROPERTIES TIMEOUT 60)

if(WAVE_HAS_PRIVATE_ROMS)
    add_executable(WaveIntegrationTests WaveIntegrationTests.cpp)
    target_include_directories(WaveIntegrationTests PRIVATE ${CMAKE_SOURCE_DIR}/Source)
    target_link_libraries(WaveIntegrationTests
        PRIVATE
            WaveEmulation
            juce::juce_audio_utils
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
    add_test(NAME WaveIntegrationTests COMMAND WaveIntegrationTests)
    add_test(NAME WaveDiskTotalRecallTests COMMAND WaveIntegrationTests --disk-total-recall)
    add_test(NAME WaveStoreModeLampTests COMMAND WaveIntegrationTests --store-mode-exit)
    add_test(NAME WaveReopenBankTests COMMAND WaveIntegrationTests --reopen-banks)

    add_executable(WaveEditorTests WaveEditorTests.cpp)
    target_include_directories(WaveEditorTests PRIVATE ${CMAKE_SOURCE_DIR}/Source)
    target_link_libraries(WaveEditorTests
        PRIVATE
            WaveEmulation
            juce::juce_audio_utils
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
    add_test(NAME WaveEditorTests COMMAND WaveEditorTests)

    add_executable(WavePerformanceBenchmark WavePerformanceBenchmark.cpp)
    target_include_directories(WavePerformanceBenchmark PRIVATE ${CMAKE_SOURCE_DIR}/Source)
    target_link_libraries(WavePerformanceBenchmark
        PRIVATE
            WaveEmulation
            juce::juce_audio_utils
            juce::juce_recommended_config_flags)
endif()

if(WAVE_HAS_PRIVATE_ROMS)
    set_tests_properties(WaveCoreTests PROPERTIES ENVIRONMENT
        "WAVE_FIRMWARE_DIR=${CMAKE_SOURCE_DIR}/Firmware/wave_sys1_700;PPG_WAVETABLE_ROM=${CMAKE_SOURCE_DIR}/Firmware/wave_sys1_700/ppg-wave-2.3-v6-wavetables.rom;WAVE_FACTORY_SET=${CMAKE_SOURCE_DIR}/wave.set")
    set_tests_properties(WaveIntegrationTests WaveDiskTotalRecallTests WaveStoreModeLampTests WaveReopenBankTests PROPERTIES
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        ENVIRONMENT "WAVE_FACTORY_SET=${CMAKE_SOURCE_DIR}/wave.set")
    set_tests_properties(WaveEditorTests PROPERTIES
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        ENVIRONMENT "WAVE_FACTORY_SET=${CMAKE_SOURCE_DIR}/wave.set")
endif()

# Developer tool: drives the real WDV.SYS from a script and prints the ASIC/CV
# write stream for the oracle diff (needs no ROMs at build time).
add_executable(WaveVoiceTrace EXCLUDE_FROM_ALL WaveVoiceTrace.cpp)
target_include_directories(WaveVoiceTrace PRIVATE ../Source)
target_link_libraries(WaveVoiceTrace PRIVATE WaveCore)
