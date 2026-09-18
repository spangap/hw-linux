# The `driver` component on the host target. ESP-IDF's own registers nothing
# there, so this one takes the name and supplies the pin table plus the two
# SPI type names the firmware's declarations mention.
idf_component_register(
    SRCS "src/gpio_shim.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos
)
