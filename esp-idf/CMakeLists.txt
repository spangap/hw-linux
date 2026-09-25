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
        "src/fdwait.cpp"
        "src/tick.cpp"
        ${SPANGAP_CONDITIONAL_SRCS}
    INCLUDE_DIRS "include" ${SPANGAP_CONDITIONAL_INCLUDE_DIRS}
    REQUIRES
        ${SPANGAP_REQUIRES}
        esp_event
        freertos
        log
)
# Every select() in the image is src/fdwait.cpp's, which blocks the calling
# task until a descriptor is ready, in place of the one IDF interposes on this
# target (esp_additions/FreeRTOSSimulator_wrappers.c), which polls. A
# redefinition would collide with IDF's; the wrap leaves it unreferenced. -u
# pulls the wrapper in whatever order the archives are scanned.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=select" "-Wl,-u,__wrap_select")

# The kernel is tickless (src/tick.cpp). ESP-IDF offers that only with power
# management, which this target does not build, so the two values its
# FreeRTOSConfig.h reads are given here, to every source alike; the kernel's
# sources are also given the port's suppress, which FreeRTOS.h takes only if
# it is defined before it. The port's own idle hook, a 15 ms sleep on every
# pass, is replaced by the one in src/tick.cpp.
idf_build_set_property(COMPILE_DEFINITIONS "CONFIG_FREERTOS_USE_TICKLESS_IDLE=1" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP=2" APPEND)
idf_component_get_property(freertos_lib freertos COMPONENT_LIB)
target_compile_options(${freertos_lib} PRIVATE "-include${CMAKE_CURRENT_LIST_DIR}/src/suppress.h")
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=esp_vApplicationIdleHook"
    "-Wl,-u,__wrap_esp_vApplicationIdleHook"
    "-Wl,-u,vPortSuppressTicksAndSleep")
