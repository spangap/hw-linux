# The `esp_driver_spi` component on the host target. ESP-IDF's own is a chip
# peripheral driver and registers nothing here, so a component that names it in
# REQUIRES — RadioLib does — cannot be registered at all: an unresolved REQUIRES
# is fatal, whether or not anything calls into it. This one takes the name and
# registers nothing: there is no SPI bus on this board, the modem's driver talks
# to the chip model in-process, and the two SPI type names the firmware's own
# declarations mention come from the `driver` shim beside this.
idf_component_register()
