#ifndef __XENGT_H__
#define __XENGT_H__

int xengt_is_enabled(void);
void xengt_draw_primary(DisplayState *ds, int full_update);
void xengt_vga_init(PCIBus *pci_bus, ram_addr_t vga_ram_addr, int
		vga_ram_size);
void vgt_bridge_pci_conf_init(PCIDevice *dev);
void vgt_bridge_pci_write(PCIDevice *dev, uint32_t addr, uint32_t val, int len);
uint32_t vgt_bridge_pci_read(PCIDevice *pci_dev, uint32_t config_addr, int len);
void destroy_vgt_instance(void);
/* Convert from a base type to a parent type, with compile time checking.  */
#ifdef __GNUC__
#define DO_UPCAST(type, field, dev) ( __extension__ ( { \
    char __attribute__((unused)) offset_must_be_zero[ \
        -offsetof(type, field)]; \
    container_of(dev, type, field);}))
#else
#define DO_UPCAST(type, field, dev) container_of(dev, type, field)
#endif
#endif
