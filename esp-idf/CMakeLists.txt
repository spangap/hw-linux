# hw-linux board straddle — the station's "hardware" when the station is a
# process. The component name starts with `hw-`, which is what makes
# spangap-core ask the linker for detect_hw.
#
# The GPIO shim and the esp_timer implementation are sub-components under
# components/, picked up because the buildable adds every staged straddle's
# components/ dir to EXTRA_COMPONENT_DIRS — later in the search order than
# ESP-IDF's own, so they override the target's stubs.
include(${CMAKE_CURRENT_LIST_DIR}/spangap_requires.cmake)
idf_component_register(
    SRCS
        "src/hwlinux.cpp"
        "src/detect.cpp"
        ${SPANGAP_CONDITIONAL_SRCS}
    INCLUDE_DIRS "include" ${SPANGAP_CONDITIONAL_INCLUDE_DIRS}
    REQUIRES
        ${SPANGAP_REQUIRES}
        esp_event
)
