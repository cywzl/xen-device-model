/* Copyright (c) Citrix Systems Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms,
 * with or without modification, are permitted provided
 * that the following conditions are met:
 *
 * *   Redistributions of source code must retain the above
 *     copyright notice, this list of conditions and the
 *     following disclaimer.
 * *   Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the
 *     following disclaimer in the documentation and/or other
 *     materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "hw.h"
#include "pci.h"
#include "qemu-xen.h"
#include "xen_pvdevice.h"

extern FILE *logfile;

static uint32_t xen_pv_mmio_read(void *opaque, target_phys_addr_t addr)
{
    return ~0;
}

static void xen_pv_mmio_write(void *opaque, target_phys_addr_t addr,
                              uint32_t val)
{
}

static CPUReadMemoryFunc *xen_pv_mmio_read_funcs[3] = {
    xen_pv_mmio_read,
    xen_pv_mmio_read,
    xen_pv_mmio_read,
};

static CPUWriteMemoryFunc *xen_pv_mmio_write_funcs[3] = {
    xen_pv_mmio_write,
    xen_pv_mmio_write,
    xen_pv_mmio_write,
};

static void xen_pv_mmio_map(PCIDevice *d, int region_num,
                            uint32_t addr, uint32_t size, int type)
{
    int mmio_io_addr;

    mmio_io_addr = cpu_register_io_memory(0, xen_pv_mmio_read_funcs,
                                          xen_pv_mmio_write_funcs, NULL);

    cpu_register_physical_memory(addr, size, mmio_io_addr);
}

static void xen_pv_save(QEMUFile *f, void *opaque)
{
    PCIDevice *d = opaque;

    if (d)
        pci_device_save(d, f);
}

static int xen_pv_load(QEMUFile *f, void *opaque, int version_id)
{
    PCIDevice *d = opaque;
    int rc = 0;

    if (d)
        rc = pci_device_load(d, f);
    return rc;
}

void xen_pvdevice_init(PCIBus *bus, int devfn)
{
    PCIDevice *d;
    struct pci_config_header *pch;
    const int io_region_size = 0x400000;
    const char* pvdevice_name = "xen-pvdevice";

    fprintf(logfile, "Register %s.\n", pvdevice_name);
    d = pci_register_device(
        bus, pvdevice_name, sizeof(PCIDevice), devfn, NULL, NULL);

    pch = (struct pci_config_header *)d->config;
    pch->vendor_id = 0x5853;
    pch->device_id = 0xC000;
    pch->command = 2; /* Memory access */
    pch->status = 0;
    pch->revision = 1;
    pch->class = 0x08; /* Base System Peripherals */
    pch->subclass = 0x80; /* Other system peripheral */
    pch->api = 0;
    pch->header_type = 0;
    pch->subsystem_vendor_id = pch->vendor_id;
    pch->subsystem_id = pch->device_id;
    pch->interrupt_line = 0;
    pch->interrupt_pin = 3;

    fprintf(logfile, "%s: vendor-id=%04x device-id=%04x revsion=%02x size=%08x\n",
            pvdevice_name,
            pch->vendor_id,
            pch->device_id,
            pch->revision,
            io_region_size);

    pci_register_io_region(d, 1, io_region_size,
                           PCI_ADDRESS_SPACE_MEM_PREFETCH, xen_pv_mmio_map);

    register_savevm(pvdevice_name, -1, 1, xen_pv_save, xen_pv_load, d);
    fprintf(logfile, "Done register %s.\n", pvdevice_name);
}
