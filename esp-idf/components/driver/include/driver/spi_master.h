/**
 * driver/spi_master.h — the two SPI names the firmware's declarations need.
 *
 * Nothing on the host talks to an SPI bus: the one driver that would is
 * replaced by a HAL that hands its bytes to a chip model instead. These types
 * exist so headers that mention a bus and a device handle still parse.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SPI1_HOST = 0,
    SPI2_HOST = 1,
    SPI3_HOST = 2,
} spi_host_device_t;

typedef void* spi_device_handle_t;

#ifdef __cplusplus
}
#endif
