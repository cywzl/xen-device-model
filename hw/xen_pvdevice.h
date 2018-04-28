#ifndef XEN_PVDEVICE_H
#define XEN_PVDEVICE_H

#include "pci.h"

void xen_pvdevice_init(PCIBus *bus, int devfn);

#endif
