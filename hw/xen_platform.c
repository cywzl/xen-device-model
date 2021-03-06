/*
 * XEN platform fake pci device, formerly known as the event channel device
 * 
 * Copyright (c) 2003-2004 Intel Corp.
 * Copyright (c) 2006 XenSource
 * Copyright (c) 2010 Citrix Systems Inc.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "hw.h"
#include "pc.h"
#include "pci.h"
#include "irq.h"
#include "qemu-xen.h"
#include "net.h"
#include "xen_platform.h"

#include <assert.h>
#include <xenguest.h>

extern FILE *logfile;

static uint8_t platform_flags;
static int throttling_disabled;

#define PFFLAG_ROM_LOCK 1 /* Sets whether ROM memory area is RW or RO */

static uint8_t
get_platform_flags(void)
{
    return platform_flags;
}

static void
set_platform_flags(uint8_t flags)
{
    hvmmem_type_t mem_type;

    mem_type = (flags & PFFLAG_ROM_LOCK) ? HVMMEM_ram_ro : HVMMEM_ram_rw;

    if (xc_hvm_set_mem_type(xc_handle, domid, mem_type, 0xc0, 0x40))
        fprintf(logfile, "unable to change state of ROM memory area!\n");
    else {
        platform_flags = flags & PFFLAG_ROM_LOCK;

        fprintf(logfile, "ROM memory area now %s\n",
                (mem_type == HVMMEM_ram_ro) ? "RO" : "RW");
    }
}

static void log_throttling(const char *path, const char *token, void *opaque)
{
    int len;
    char *throttling = xenstore_dom_read(domid, "log-throttling", &len);
    if (throttling != NULL) {
        throttling_disabled = !(throttling[0] - '0');
        free(throttling);
        fprintf(logfile, "log_throttling %s\n", throttling_disabled ? "disabled" : "enabled");
    }
}

/* We throttle access to dom0 syslog, to avoid DOS attacks.  This is
   modelled as a token bucket, with one token for every byte of log.
   The bucket size is 128KB (->1024 lines of 128 bytes each) and
   refills at 256B/s.  It starts full.  The guest is blocked if no
   tokens are available when it tries to generate a log message. */
#define BUCKET_MAX_SIZE (128*1024)
#define BUCKET_FILL_RATE 256

static void
throttle(unsigned count)
{
    static unsigned available;
    static struct timespec last_refil;
    static int started;
    static int warned;

    struct timespec waiting_for, now;
    double delay;
    struct timespec ts;

    if (throttling_disabled)
        return;

    if (!started) {
        clock_gettime(CLOCK_MONOTONIC, &last_refil);
        available = BUCKET_MAX_SIZE;
        started = 1;
    }

    if (count > BUCKET_MAX_SIZE) {
        fprintf(logfile, "tried to get %d tokens, but bucket size is %d\n",
                BUCKET_MAX_SIZE, count);
        exit(1);
    }

    if (available < count) {
        /* The bucket is empty.  Refil it */

        /* When will it be full enough to handle this request? */
        delay = (double)(count - available) / BUCKET_FILL_RATE;
        waiting_for = last_refil;
        waiting_for.tv_sec += delay;
        waiting_for.tv_nsec += (delay - (int)delay) * 1e9;
        if (waiting_for.tv_nsec >= 1000000000) {
            waiting_for.tv_nsec -= 1000000000;
            waiting_for.tv_sec++;
        }

        /* How long do we have to wait? (might be negative) */
        clock_gettime(CLOCK_MONOTONIC, &now);
        ts.tv_sec = waiting_for.tv_sec - now.tv_sec;
        ts.tv_nsec = waiting_for.tv_nsec - now.tv_nsec;
        if (ts.tv_nsec < 0) {
            ts.tv_sec--;
            ts.tv_nsec += 1000000000;
        }

        /* Wait for it. */
        if (ts.tv_sec > 0 ||
            (ts.tv_sec == 0 && ts.tv_nsec > 0)) {
            if (!warned) {
                fprintf(logfile, "throttling guest access to syslog");
                warned = 1;
            }
            while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
                ;
        }

        /* Refil */
        clock_gettime(CLOCK_MONOTONIC, &now);
        delay = (now.tv_sec - last_refil.tv_sec) +
            (now.tv_nsec - last_refil.tv_nsec) * 1.0e-9;
        available += BUCKET_FILL_RATE * delay;
        if (available > BUCKET_MAX_SIZE)
            available = BUCKET_MAX_SIZE;
        last_refil = now;
    }

    assert(available >= count);

    available -= count;
}

static char log_buffer[4096];
static int log_buffer_off;

static void
write_log(char c)
{
    if (c == '\n' || log_buffer_off == sizeof(log_buffer) - 1) {
        log_buffer[log_buffer_off] = 0;
        throttle(log_buffer_off);
        fprintf(logfile, "%s\n", log_buffer);
        log_buffer_off = 0;
        return;
    }

    if (isspace(c))
        c = ' ';

    if (c == ' ' || isgraph(c))
        log_buffer[log_buffer_off++] = c;
}

static uint8_t unplug_version;
static int unplug_version_isset;

static int drivers_blacklisted;

static uint8_t
get_unplug_version(void)
{
    return unplug_version;
}

static int
set_unplug_version(uint8_t version)
{
    if (unplug_version_isset)
        return 0;

    unplug_version = version;
    unplug_version_isset = 1;

    if (version > 1)
        drivers_blacklisted = 1;

    fprintf(logfile, "UNPLUG: protocol version set to %d "
            "(drivers %sblacklisted)\n", unplug_version,
            (!drivers_blacklisted) ? "not " : "");

    return 1;
}

#define UNPLUG_ALL_IDE_DISKS_BIT    0
#define UNPLUG_ALL_NICS_BIT         1
#define UNPLUG_AUX_IDE_DISKS_BIT    2

static void
version_0_1_unplug(uint16_t mask)
{
    if (drivers_blacklisted)
        return;

    if (mask & (1 << UNPLUG_ALL_IDE_DISKS_BIT)) {
        ide_unplug_all_harddisks();
    }

    if (mask & (1 << UNPLUG_ALL_NICS_BIT)) {
        pci_unplug_all_netifs();
        net_tap_shutdown_all();
    }

    if (mask & (1 << UNPLUG_AUX_IDE_DISKS_BIT)) {
        ide_unplug_aux_harddisks();
    }
}

#define UNPLUG_TYPE_IDE 1
#define UNPLUG_TYPE_NIC 2

static void
version_2_unplug(uint8_t type, uint8_t index)
{
    if (drivers_blacklisted)
        return;

    switch (type) {
    case UNPLUG_TYPE_IDE:
        fprintf(logfile, "UNPLUG: IDE\n");

        ide_unplug_harddisk(index);
        break;
    case UNPLUG_TYPE_NIC: {
        int id;

        fprintf(logfile, "UNPLUG: NIC\n");

        if ((id = pci_unplug_nic((4+index)*8)) >= 0)
            net_tap_shutdown_vlan(id);

        break;
    }
    default:
        fprintf(logfile, "UNPLUG: unrecognized type %02x\n",
                type);
        break;
    }
}

static uint16_t product_id;
static uint32_t build_number;

static void
set_product_id(uint16_t id)
{
    product_id = id;
}

static void
set_build_number(uint32_t number)
{
    if (product_id == 0) {
        fprintf(logfile, "UNPLUG: product_id has not been set\n");
    } else {
        build_number = number;

        fprintf(logfile, "UNPLUG: product_id: %d build_number: %d\n",
                product_id, build_number);

        drivers_blacklisted =
            xenstore_pv_driver_build_blacklisted(product_id,
                                                 build_number);

        fprintf(logfile, "UNPLUG: drivers %sblacklisted\n",
                (!drivers_blacklisted) ? "not " : "");

	if (product_id == 3 && build_number <= 0xff &&
                !drivers_blacklisted)
                xenstore_notify_unplug();
    }

}

static uint32_t
platform_fixed_ioport_read4(void *opaque, uint32_t addr)
{
    return 0xFFFFFFFF;
}

static uint32_t
platform_fixed_ioport_read2(void *opaque, uint32_t addr)
{
    uint16_t val;

    switch (addr) {
    case 0x10:
        val = (!drivers_blacklisted) ? 0x49d2 : 0xd249;
        break;

    default:
        val = 0xFFFF;
        break;
    }

    return (uint32_t)val;
}

static uint32_t
platform_fixed_ioport_read1(void *opaque, uint32_t addr)
{
    uint8_t val;

    switch (addr) {
    case 0x10:
        val = get_platform_flags();
        break;

    case 0x12:
        /*
         * Reading this port implicitly sets the unplug protocol
         * version to at least 1. If the vserion is already been
         * explicitly set then this call has no effect.
         */
        (void) set_unplug_version(1);

        val = get_unplug_version();

        fprintf(logfile, "UNPLUG: protocol %d active\n", val);
        break;

    default:
        val = 0xFF;
    }

    return (uint32_t)val;
}

static void
platform_fixed_ioport_write4(void *opaque, uint32_t addr, uint32_t val)
{
    if (unplug_version != 0)
        set_build_number(val);
}

static void
platform_fixed_ioport_write2(void *opaque, uint32_t addr, uint32_t val)
{
    switch (addr) {
    case 0x10:
        if (unplug_version == 0 ||
            unplug_version == 1) {
            uint16_t mask = (uint16_t)val;

            version_0_1_unplug(mask);
        }
        break;

    case 0x12:
        if (unplug_version != 0)
            set_product_id((uint16_t)val);

        break;
    }
}

static void
platform_fixed_ioport_write1(void *opaque, uint32_t addr, uint32_t val)
{
    static uint8_t unplug_type;

    switch (addr) {
    case 0x10:
        set_platform_flags((uint8_t)val);
        break;

    case 0x11:
        if (unplug_version == 2)
            unplug_type = (uint8_t)val;

        break;

    case 0x12:
        write_log((char)val);
        break;

    case 0x13:
        /*
         * The first write to this port sets the unpluc protocol version.
         * Any subsequent write, providing the protocol is set to 2, will
         * be treated as an unplug index.
         */
        if (!set_unplug_version((uint8_t)val) &&
            unplug_version == 2)
            version_2_unplug(unplug_type, (uint8_t)val);

        break;
    }
}

static void platform_fixed_ioport_save(QEMUFile *f, void *opaque)
{
    uint8_t flags = get_platform_flags();

    qemu_put_8s(f, &flags);
}

static int platform_fixed_ioport_load(QEMUFile *f, void *opaque, int version_id)
{
    uint8_t flags;

    if (version_id > 1)
        return -EINVAL;

    qemu_get_8s(f, &flags);
    set_platform_flags(flags);

    return 0;
}

void platform_fixed_ioport_init(void)
{
    struct stat stbuf;
    int len = 1;

    register_savevm("platform_fixed_ioport", 0, 1, platform_fixed_ioport_save,
                    platform_fixed_ioport_load, NULL);

    register_ioport_write(0x10, 16, 4, platform_fixed_ioport_write4, NULL);
    register_ioport_write(0x10, 16, 2, platform_fixed_ioport_write2, NULL);
    register_ioport_write(0x10, 16, 1, platform_fixed_ioport_write1, NULL);
    register_ioport_read(0x10, 16, 4, platform_fixed_ioport_read4, NULL);
    register_ioport_read(0x10, 16, 2, platform_fixed_ioport_read2, NULL);
    register_ioport_read(0x10, 16, 1, platform_fixed_ioport_read1, NULL);

    set_platform_flags(0);
}

static uint32_t xen_platform_ioport_readb(void *opaque, uint32_t addr)
{
    addr &= 0xff;

    return (addr == 0) ? platform_fixed_ioport_read1(NULL, 0x10) : ~0u;
}

static void xen_platform_ioport_writeb(void *opaque, uint32_t addr, uint32_t val)
{
    addr &= 0xff;
    val  &= 0xff;

    switch (addr) {
    case 0:
        set_platform_flags((uint8_t)val);
        break;
    case 8:
        write_log((char)val);
        break;
    default:
        break;
    }
}

typedef struct PCIXenPlatformState
{
  PCIDevice  pci_dev;
} PCIXenPlatformState;

static void platform_ioport_map(PCIDevice *pci_dev, int region_num, uint32_t addr, uint32_t size, int type)
{
    PCIXenPlatformState *d = (PCIXenPlatformState *)pci_dev;
    register_ioport_write(addr, size, 1, xen_platform_ioport_writeb, d);
    register_ioport_read(addr, size, 1, xen_platform_ioport_readb, d);
}

static uint32_t platform_mmio_read(void *opaque, target_phys_addr_t addr)
{
    static int warnings = 0;
    if (warnings < 5) {
        fprintf(logfile, "Warning: attempted read from physical address "
                "0x%"PRIx64" in xen platform mmio space\n", (uint64_t)addr);
        warnings++;
    }
    return 0;
}

static void platform_mmio_write(void *opaque, target_phys_addr_t addr,
                                uint32_t val)
{
    static int warnings = 0;
    if (warnings < 5) {
        fprintf(logfile, "Warning: attempted write of 0x%x to physical "
                "address 0x%"PRIx64" in xen platform mmio space\n",
                val, (uint64_t)addr);
        warnings++;
    }
    return;
}

static CPUReadMemoryFunc *platform_mmio_read_funcs[3] = {
    platform_mmio_read,
    platform_mmio_read,
    platform_mmio_read,
};

static CPUWriteMemoryFunc *platform_mmio_write_funcs[3] = {
    platform_mmio_write,
    platform_mmio_write,
    platform_mmio_write,
};

static void platform_mmio_map(PCIDevice *d, int region_num,
                              uint32_t addr, uint32_t size, int type)
{
    int mmio_io_addr;

    mmio_io_addr = cpu_register_io_memory(0, platform_mmio_read_funcs,
                                          platform_mmio_write_funcs, NULL);

    cpu_register_physical_memory(addr, 0x1000000, mmio_io_addr);
}

static void xen_pci_save(QEMUFile *f, void *opaque)
{
    PCIXenPlatformState *d = opaque;
    uint64_t t = 0;

    pci_device_save(&d->pci_dev, f);
    qemu_put_be64s(f, &t);
}

static int xen_pci_load(QEMUFile *f, void *opaque, int version_id)
{
    PCIXenPlatformState *d = opaque;
    int ret;

    ret = pci_device_load(&d->pci_dev, f);
    if (ret < 0)
        return ret;

    if (version_id >= 2) {
        if (version_id == 2) {
            uint8_t flags;

            qemu_get_8s(f, &flags);
            set_platform_flags(flags);
        }
        qemu_get_be64(f);
    }

    return 0;
}

void pci_xen_platform_init(PCIBus *bus, int devfn)
{
    PCIXenPlatformState *d;
    struct pci_config_header *pch;

    printf("Register xen platform.\n");
    d = (PCIXenPlatformState *)pci_register_device(
        bus, "xen-platform", sizeof(PCIXenPlatformState), devfn, NULL, NULL);
    pch = (struct pci_config_header *)d->pci_dev.config;

    xenstore_parse_pf_config(pch);

    pch->command = 3; /* IO and memory access */
    pch->api = 0;
    pch->class = 0x1; /* Storage device class */
    pch->subclass = 0x0; /* SCSI subclass */
    pch->header_type = 0;
    pch->interrupt_pin = 3; /* Carefully chosen to avoid interrupt
                               sharing in non-apic systems, which
                               triggers a bug in the Geneva PV
                               drivers. */

    pci_register_io_region(&d->pci_dev, 0, 0x100,
                           PCI_ADDRESS_SPACE_IO, platform_ioport_map);

    /* reserve 16MB mmio address for share memory*/
    pci_register_io_region(&d->pci_dev, 1, 0x1000000,
                           PCI_ADDRESS_SPACE_MEM_PREFETCH, platform_mmio_map);

    register_savevm("platform", 0, 3, xen_pci_save, xen_pci_load, d);
    printf("Done register platform.\n");
    xenstore_dom_watch(domid, "log-throttling", log_throttling, NULL);
}

