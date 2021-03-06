// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <hypervisor/block.h>
#include <hypervisor/guest.h>

#include "linux.h"

#define ALIGN(x, alignment)    (((x) + (alignment - 1)) & ~(alignment - 1))

// See https://www.kernel.org/doc/Documentation/x86/boot.txt
// and https://www.kernel.org/doc/Documentation/x86/zero-page.txt
// for an explanation of the zero page, setup header and boot params.

// Screen info offsets
#define ZP_SI_8_VIDEO_MODE      0x0006     // Original video mode
#define ZP_SI_8_VIDEO_COLS      0x0007     // Original video cols
#define ZP_SI_8_VIDEO_LINES     0x000e     // Original video lines

// Setup header offsets
#define ZP_SH_8_E820_COUNT      0x01e8     // Number of entries in e820 map
#define ZP_SH_8_SETUP_SECTS     0x01f1     // Size of real mode kernel in sectors
#define ZP_SH_8_LOADER_TYPE     0x0210     // Type of bootloader
#define ZP_SH_8_LOAD_FLAGS      0x0211     // Boot protocol flags
#define ZP_SH_8_RELOCATABLE     0x0234     // Is the kernel relocatable?
#define ZP_SH_16_BOOTFLAG       0x01fe     // Bootflag, should match BOOT_FLAG_MAGIC
#define ZP_SH_16_VERSION        0x0206     // Boot protocol version
#define ZP_SH_16_XLOADFLAGS     0x0236     // 64-bit and EFI load flags
#define ZP_SH_32_SYSSIZE        0x01f4     // Size of protected-mode code + payload in 16-bytes
#define ZP_SH_32_HEADER         0x0202     // Header, should match HEADER_MAGIC
#define ZP_SH_32_COMMAND_LINE   0x0228     // Pointer to command line args string
#define ZP_SH_32_KERNEL_ALIGN   0x0230     // Kernel alignment
#define ZP_SH_64_PREF_ADDRESS   0x0258     // Preferred address for kernel to be loaded at
#define ZP_SH_XX_E820_MAP       0x02d0     // The e820 memory map

#define ZP8(p, off) (*((uint8_t*)((p) + (off))))
#define ZP16(p, off) (*((uint16_t*)((p) + (off))))
#define ZP32(p, off) (*((uint32_t*)((p) + (off))))
#define ZP64(p, off) (*((uint64_t*)((p) + (off))))

#define LF_LOAD_HIGH            1 << 0     // The protected mode code defaults to 0x100000
#define XLF_KERNEL_64           1 << 0     // Kernel has legacy 64-bit entry point at 0x200

#define BOOT_FLAG_MAGIC         0xaa55     // Boot flag value to match for Linux
#define HEADER_MAGIC            0x53726448 // Header value to match for Linux
#define LEGACY_64_ENTRY_OFFSET  0x200      // Offset for the legacy 64-bit entry point
#define LOADER_TYPE_UNSPECIFIED 0xff       // We are bootloader that Linux knows nothing about
#define MIN_BOOT_PROTOCOL       0x0200     // The minimum boot protocol we support (bzImage)
#define MAX_E820_ENTRIES        128        // The space reserved for e820, in entries

// Default address to load bzImage at
static const uintptr_t kDefaultKernelOffset = 0x100000;

static bool is_linux(const uintptr_t zero_page) {
    return ZP16(zero_page, ZP_SH_16_BOOTFLAG) == BOOT_FLAG_MAGIC &&
           ZP32(zero_page, ZP_SH_32_HEADER) == HEADER_MAGIC;
}

mx_status_t setup_linux(const uintptr_t addr, const size_t size, const uintptr_t first_page,
                        const int fd, const char* cmdline, uintptr_t* guest_ip,
                        uintptr_t* zero_page_addr) {
    if (!is_linux(first_page))
        return MX_ERR_NOT_SUPPORTED;

    if ((ZP16(first_page, ZP_SH_16_XLOADFLAGS) & XLF_KERNEL_64) == 0) {
        fprintf(stderr, "Kernel lacks the legacy 64-bit entry point\n");
        return MX_ERR_NOT_SUPPORTED;
    }

    uint16_t protocol = ZP16(first_page, ZP_SH_16_VERSION);
    uint8_t loadflags = ZP8(first_page, ZP_SH_8_LOAD_FLAGS);
    bool is_bzimage = (protocol >= MIN_BOOT_PROTOCOL) && (loadflags & LF_LOAD_HIGH);
    if (!is_bzimage) {
        fprintf(stderr, "Kernel is not a bzimage. Use a newer kernel\n");
        return MX_ERR_NOT_SUPPORTED;
    }

    // Default to the preferred address, then change if we're relocatable
    uintptr_t runtime_start = ZP64(first_page, ZP_SH_64_PREF_ADDRESS);
    if (ZP8(first_page, ZP_SH_8_RELOCATABLE)) {
        uint64_t kernel_alignment = ZP32(first_page, ZP_SH_32_KERNEL_ALIGN);
        uint64_t aligned_address = ALIGN(kDefaultKernelOffset, kernel_alignment);
        runtime_start = aligned_address;
    }

    // Move the zero-page. For a 64-bit kernel it can go almost anywhere,
    // so we'll put it just below the boot kernel.
    uintptr_t boot_params_off = runtime_start - PAGE_SIZE;
    uint8_t* zero_page = (uint8_t*)(addr + boot_params_off);
    memmove(zero_page, (void*)first_page, PAGE_SIZE);

    // Copy the command line string below the zero page.
    size_t cmdline_len = strlen(cmdline) + 1;
    uintptr_t cmdline_off = boot_params_off - cmdline_len;
    memcpy((char*)(addr + cmdline_off), cmdline, cmdline_len);

    // TODO(andymutton): Setup everything else.
    ZP8(zero_page, ZP_SH_8_LOADER_TYPE) = LOADER_TYPE_UNSPECIFIED;
    ZP32(zero_page, ZP_SH_32_COMMAND_LINE) = cmdline_off;

    // Zero video, columns and lines to skip early video init - just serial output for now.
    ZP8(zero_page, ZP_SI_8_VIDEO_MODE) = 0;
    ZP8(zero_page, ZP_SI_8_VIDEO_COLS) = 0;
    ZP8(zero_page, ZP_SI_8_VIDEO_LINES) = 0;

    // Add e820 entries.
    int num_entries = 0;
    uintptr_t e820_off = boot_params_off + ZP_SH_XX_E820_MAP;
    mx_status_t status = guest_create_e820_memory_map(addr, size, e820_off, &num_entries);
    if (status != MX_OK) {
        fprintf(stderr, "Failed to create the e820 memory map\n");
        return status;
    }
    if (num_entries > MAX_E820_ENTRIES) {
        fprintf(stderr, "Too many e820 entries created\n");
        return MX_ERR_BAD_STATE;
    }
    ZP8(zero_page, ZP_SH_8_E820_COUNT) = num_entries;

    int setup_sects = ZP8(zero_page, ZP_SH_8_SETUP_SECTS);
    if (setup_sects == 0) {
        // 0 here actually means 4, see boot.txt.
        setup_sects = 4;
    }

    // Read the rest of the bzImage into the protected_mode_kernel location.
    int protected_mode_off = (setup_sects + 1) * SECTOR_SIZE;
    if (lseek(fd, protected_mode_off, SEEK_SET) < 0) {
        fprintf(stderr, "Failed seek to protected mode kernel\n");
        return MX_ERR_IO;
    }

    size_t remaining = (ZP32(zero_page, ZP_SH_32_SYSSIZE) << 4);
    int ret = read(fd, (void*)(addr + runtime_start), remaining);

    if ((size_t)ret != remaining) {
        fprintf(stderr, "Failed to read linux image\n");
        return MX_ERR_IO;
    }

    *guest_ip = runtime_start + LEGACY_64_ENTRY_OFFSET;
    *zero_page_addr = boot_params_off;
    return MX_OK;
}
