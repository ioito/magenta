// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include <stdint.h>
#include <magenta/types.h>

mx_status_t pci_pio_cfg_read(uint8_t bus, uint8_t dev, uint8_t func,
                             uint8_t offset, uint32_t* val, size_t width);
mx_status_t pci_pio_cfg_write(uint8_t bus, uint8_t dev, uint8_t func,
                              uint8_t offset, uint32_t val, size_t width);
int test_func(void);
