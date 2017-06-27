// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include <assert.h>
#include <kernel/mutex.h>
#include <magenta/types.h>
#include <lib/pci/pio.h>

#ifdef ARCH_X86
#include <arch/x86.h>

// TODO: This library exists as a shim for the awkward period between bringing
// PCI legacy support online, and moving PCI to userspace. Initially, it exists
// as a kernel library that userspace accesses via syscalls so that a userspace
// process never causes a race condition with the bus driver's accesses. Later,
// all accesses will go through the library itself in userspace and the syscalls
// will no longer exist.
#define PCI_CONFIG_ADDRESS (0xCF8)
#define PCI_CONFIG_DATA    (0xCFC)
#define PCI_BDF_ADDR(bus, dev, func, off) \
    ((1 << 31) | ((bus & 0xFF) << 16) | ((dev & 0x1F) << 11) | ((func & 0x7) << 8) | (off & 0xFC))

static mutex_t lock = MUTEX_INITIAL_VALUE(lock);

int test_func(void) {
    volatile int x = 42;
    return x;
}

mx_status_t pci_pio_cfg_read(uint8_t bus, uint8_t dev, uint8_t func,
                             uint8_t offset, uint32_t* val, size_t width) {
    mutex_acquire(&lock);
    mx_status_t status = MX_OK;
    size_t shift = (offset & 0x3) * 8;

    if (shift + width > 32) {
        status = MX_ERR_INVALID_ARGS;
        goto err;
    }

    uint32_t addr = PCI_BDF_ADDR(bus, dev, func, offset);
    outpd(PCI_CONFIG_ADDRESS, addr);
    uint32_t tmp_val = inpd(PCI_CONFIG_DATA);
    uint32_t width_mask = (1 << width) - 1;

    // Align the read to the correct offset, then mask based on byte width
    *val = (tmp_val >> shift) & width_mask;

err:
    mutex_release(&lock);
    return status;
}

mx_status_t pci_pio_cfg_write(uint8_t bus, uint8_t dev, uint8_t func,
                              uint8_t offset, uint32_t val, size_t width) {
    mutex_acquire(&lock);
    mx_status_t status = MX_OK;
    size_t shift = (offset & 0x3) * 8;
    uint32_t width_mask = (1 << width) - 1;
    uint32_t write_mask = width_mask << shift;

    if (shift + width > 32) {
        status = MX_ERR_INVALID_ARGS;
        goto err;
    }

    uint32_t addr = PCI_BDF_ADDR(bus, dev, func, offset);
    outpd(PCI_CONFIG_ADDRESS, addr);
    uint32_t tmp_val = inpd(PCI_CONFIG_DATA);

    val &= width_mask;
    tmp_val &= ~write_mask;
    tmp_val |= (val << shift);
    outpd(PCI_CONFIG_DATA, tmp_val);

err:
    mutex_release(&lock);
    return status;
}

#else // not x86
mx_status_t pci_pio_cfg_read(uint8_t bus, uint8_t dev, uint8_t func,
                             uint8_t offset, uint32_t* val, size_t width) {
    return MX_ERR_NOT_SUPPORTED;
}

mx_status_t pci_pio_cfg_write(uint8_t bus, uint8_t dev, uint8_t func,
                              uint8_t offset, uint32_t val, size_t width) {
    return MX_ERR_NOT_SUPPORTED;;
}

#endif // ARCH_X86
