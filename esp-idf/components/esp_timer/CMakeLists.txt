# esp_timer on the host target. ESP-IDF's own registers headers only there, so
# every esp_timer_* call would be an undefined symbol; this component takes the
# name and supplies the implementation behind the same headers.
idf_component_get_property(original_esp_timer_dir esp_timer COMPONENT_OVERRIDEN_DIR)

idf_component_register(
    SRCS "src/esp_timer_host.c"
    INCLUDE_DIRS "${original_esp_timer_dir}/include"
    REQUIRES freertos esp_common
)
