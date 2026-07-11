#ifndef _INCLUDE_NARF_IO_H_
#define _INCLUDE_NARF_IO_H_

#include <stdint.h>
#include <stdbool.h>

//! @brief Initialize the narf_io layer.
//!
//! This is typically implemented by the platform-specific I/O layer.
//!
//! @return true on success.
bool narf_io_open(void);

//! @brief Deinitialize the narf_io layer.
//!
//! This is typically implemented by the platform-specific I/O layer.
//!
//! @return true on success.
bool narf_io_close(void);

//! @brief Get the size of the underlying device in sectors.
//!
//! This is typically implemented by the platform-specific I/O layer.
//!
//! @return Number of sectors supported by the device.
uint32_t narf_io_sectors(void);

//! @brief Write one sector to the underlying device.
//!
//! This is typically implemented by the platform-specific I/O layer.
//!
//! @param sector Sector address to write.
//! @param data Pointer to one sector of data to write.
//! @return true on success.
bool narf_io_write(uint32_t sector, void *data);

//! @brief Read one sector from the underlying device.
//!
//! This is typically implemented by the platform-specific I/O layer.
//!
//! @param sector Sector address to read.
//! @param data Pointer to one sector of read buffer.
//! @return true on success.
bool narf_io_read(uint32_t sector, void *data);

#endif

// vim:set ai softtabstop=3 shiftwidth=3 tabstop=3 expandtab: ff=unix
