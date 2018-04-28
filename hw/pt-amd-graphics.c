/*
 * AMD SRIOV Graphics pass through support for VFs
 */

#include "pass-through.h"
#include "pci/header.h"
#include "pci/pci.h"

#include <unistd.h>
#include <sys/ioctl.h>
#include <assert.h>

#include <sys/mman.h>

#include "gim_ioctl.h"
#include "qemu-char.h"

#define BLOCK_MMIO 		1
#define DO_NOT_BLOCK_MMIO	2

static int amd_default_mmio_behavior = BLOCK_MMIO;
//#define MMIO_LOGGING
//#define MMIO_LIST

#ifdef MMIO_LOGGING
static int MMIO_count = 0;
#endif

static int good_MMIO_count = 0 ;
static int bad_MMIO_count = 0;

static size_t		pt_amd_mmio_size = 0;
static void		*pt_amd_mmio_ptr = (void *) 0;
static PCIDevice	*ati_pci_gfx_dev = NULL;
static uint32_t		pt_amd_mmio_bar_num;
static uint32_t 	pt_amd_guest_mmio_bar;

static char gim_file_name[] = "/dev/gim";			/* Location of the GIM IOCTL device	*/
static char sysfs_dir[] = "/sys/devices/virtual/sriov/gim/";	/* GIM will create a sysfs file in this folder with my PID */
static char gim_sysfs_pipe[64];					/* Needs to hold "sysfs_dir/qemu-<pid>" */
static int  pt_trap_needed = 1;					/* Default to MMIO trapping needed	*/

/* Structure to maintain a list of MMIO registers and whether they are white or black listed */
struct emulated_mmio {
	uint32_t	offset;
	uint32_t	valid;					/* Can be split to read and write	*/
} *amd_emulated_mmio = NULL;					/* Array of valid passthru MMIO addresses */
static uint32_t amd_num_emulated_mmio = 0;			/* Number of entries in the array	*/
static uint32_t amd_emulated_mmio_size = 0;			/* Size of the array in uint32_t entries*/

static int pt_amd_sysfs_fd;

#define MMIO_SIZE_INCREMENT 32

static int  amd_mmio_is_xen_mapped = 0;				/* Indicates if Xen Mapping (pass through) is in effect		*/
static uint32_t pt_amd_mem_handle = 0;
static void pt_disable_mmio (void);
static void pt_enable_mmio (void);
static int  pt_amd_alloc_vf (uint8_t bus, uint8_t dev, uint8_t fn);
static void pt_amd_free_vf (void);
void pt_iomem_ati_mmio_cleanup (void *unused);
int unregister_amd_vf_region(struct pt_dev *real_device);

struct mmio_counter {
	uint32_t	offset;
	uint32_t	read_count;
	uint32_t	write_count;
};

static int max_bad_mmios = 0;
#define BAD_MMIO_INC	32

static struct mmio_counter *bad_mmios;
static int bad_mmio_count = 0;

/* prototypes */
int register_amd_vf_region (struct pt_dev *real_device);

static int can_access_mmio (uint32_t offset, int is_write);
static void pt_munmap(void* virt_addr, size_t length);


/*
 * Map a Host physical address to a virtual address
 */
static void *pt_mmap(uint64_t phys_addr, size_t length)
{
	int fd;
	size_t		map_size;
	__off64_t	page_aligned_map_offset;
	unsigned int 	offset_in_page;
	void *		virt_addr;

	if ((fd = open( "/dev/mem", O_RDWR )) < 0 ) {
		PT_LOG ("Serious ERROR: Failed to open /dev/mem\n");
		return NULL;
	}
	/*
	 * Make sure that the map_size encompasses the entire range.
	 * ie might start in middle of a page and might end in the middle of a page
	 */
	offset_in_page = phys_addr & (TARGET_PAGE_SIZE - 1);
	map_size = length + offset_in_page;
	map_size = ((map_size + TARGET_PAGE_SIZE - 1) >> TARGET_PAGE_BITS) << TARGET_PAGE_BITS;


	page_aligned_map_offset = phys_addr & ~(TARGET_PAGE_SIZE - 1);
	virt_addr = mmap64(0, map_size,
			   PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
			   fd, page_aligned_map_offset);

	close (fd);

	virt_addr = (char *) virt_addr + offset_in_page;

	return (virt_addr);
}

static void pt_munmap(void* virt_addr, size_t length)
{
	unsigned int 	offset_in_page;
	size_t	   	map_size;
	

	offset_in_page = (unsigned int)((unsigned long) virt_addr & (TARGET_PAGE_SIZE - 1));
	map_size = length + offset_in_page;
	map_size = ((map_size + TARGET_PAGE_SIZE - 1) >> TARGET_PAGE_BITS) << TARGET_PAGE_BITS;

	virt_addr = (char *)virt_addr - offset_in_page;

	munmap(virt_addr, map_size);
}

static bool pt_ati_get_mmio_bar_index (struct pci_dev *pci_dev, unsigned int *bar_index)
{
	bool	found = false;
	unsigned	int i;

	/*
	 * Find the MMIO BAR.  The MMIO has the attributes of MEMORY and non-prefetch.
	 *
	 * PCI_NUM_REGIONS is the 6 BARs plus the ROM expansion BAR (==7)
	 */
	for (i=0; i<PCI_NUM_REGIONS-1 && !found; ++i) {
		if (pci_dev->base_addr[i] != 0) {
			PT_LOG ("BAR%d [0x%08lx], Size = 0x%08lx %s, %s, %s\n", i, pci_dev->base_addr[i], pci_dev->size[i],
			pci_dev->base_addr[i] & 1 ? "I/O" : "Memory",
			pci_dev->base_addr[i] & 6 ? 
				(pci_dev->base_addr[i] & 4 ? "locatable to any 64 bit address" : "must be < 1MB")
				 	: "locatable to any 32 bit address",
			pci_dev->base_addr[i] & 8 ? "prefetchable" : "non-prefetchable");

			/*
			 * Look for a non-prefetch BAR in memory space
			 */
			if (((pci_dev->base_addr[i] & PCI_BASE_ADDRESS_SPACE) == PCI_BASE_ADDRESS_SPACE_MEMORY) &&
			   (!(pci_dev->base_addr[i] & PCI_BASE_ADDRESS_MEM_PREFETCH))) {
				*bar_index = i;

				/* Is it 32 bit or 64 bit? */
				if ((pci_dev->base_addr[i] & PCI_BASE_ADDRESS_MEM_TYPE_MASK) == PCI_BASE_ADDRESS_MEM_TYPE_64) {
					PT_LOG("64 bit address at offsets [%d] and [%d], attr = 0x%lx\n", 
						i, i+1, pci_dev->base_addr[i] & 0xf);
					found = false; 	/* MMIO can't be 64 bit BAR. */
				} else {
					PT_LOG ("32 bit address at BAR%d\n", i);
					found = true;
				}
			}
		}
	}

	return (found ? 0 : -1);
}

static uint32_t pt_graphics_mmio_readb(void *opaque, target_phys_addr_t addr)
{
	fprintf (stderr, "NOT_SUPPORTED: Read byte from MMIO 0x%08lx\n", addr);
	return 0;
}

static uint32_t pt_graphics_mmio_readw(void *opaque, target_phys_addr_t addr)
{
	fprintf (stderr, "NOT_SUPPORTED: Read word from MMIO 0x%08lx\n", addr);
	return 0;
}

static uint32_t pt_graphics_mmio_readl(void *opaque, target_phys_addr_t addr)
{
	uint32_t *mmio;
	uint64_t	offset;
	uint32_t	val;


	offset = (uint64_t) addr - (uint64_t) pt_amd_guest_mmio_bar;

	mmio = (uint32_t *) (((uint64_t)pt_amd_mmio_ptr + offset));

#ifdef MMIO_LOGGING
	++MMIO_count;
	val = *mmio;
	fprintf (stderr, "[%6d] MMIO_read:  0x%08x from offset 0x%04lx\n", MMIO_count, val, offset);
#else
#ifdef MMIO_LIST
	can_access_mmio (offset, 0);	// Call to log the access but allow it anyway
	val = *mmio;
#else
	/* Normal mode */
	if (can_access_mmio (offset, 0)) {
		val = *mmio;
	} else {
		//fprintf (stderr, "MMIO_Read: Invalid READ access of MMIO offset 0x%04lx\n", offset);
		val = 0xFFFFFFFF;
	}
#endif
#endif
	return (val);
}

static void pt_graphics_mmio_writeb(void *opaque, target_phys_addr_t addr, uint32_t val)
{
	fprintf (stderr, "NOT SUPPORTED: Write byte 0x%02x to MMIO 0x%08lx\n", val, addr);
}
static void pt_graphics_mmio_writew(void *opaque, target_phys_addr_t addr, uint32_t val)
{
	fprintf (stderr, "NOT_SUPPORTED: Write word 0x%04x to MMIO 0x%08lx\n", val, addr);
}

static void pt_graphics_mmio_writel(void *opaque, target_phys_addr_t addr, uint32_t val)
{
	uint32_t *mmio;
	uint64_t	offset;

	offset = (uint64_t) addr - (uint64_t) pt_amd_guest_mmio_bar;
	mmio = (uint32_t *) (((uint64_t)pt_amd_mmio_ptr + offset));

#ifdef MMIO_LOGGING
	++MMIO_count; 
	PT_LOG ("[%6d] MMIO_write: 0x%08x to 0x%04lx\n", MMIO_count, val, offset);
	*mmio = val;
#else
#ifdef MMIO_LIST
	can_access_mmio (offset, 1);
	*mmio = val;

#else

	if (can_access_mmio (offset, 1)) {
		*mmio = val;
	} //else {
	//	fprintf (stderr, "MMIO_Write: Invalid WRITE attempt of 0x%08x to MMIO offset 0x%04lx\n", val, offset);
	//}
#endif
#endif
	return;
}

static CPUReadMemoryFunc *pt_graphics_mmio_read[] = {
	&pt_graphics_mmio_readb,
	&pt_graphics_mmio_readw,
	&pt_graphics_mmio_readl,
};

static CPUWriteMemoryFunc *pt_graphics_mmio_write[] = {
	&pt_graphics_mmio_writeb,
	&pt_graphics_mmio_writew,
	&pt_graphics_mmio_writel,
};

void pt_iomem_ati_mmio_cleanup (void *unused)
{
	PT_LOG ("Unregister the region (?)\n");
	PT_LOG ("Clean up the mmio mapping\n");
	PT_LOG ("Unmap the virtual address space\n");
	pt_disable_mmio ();	/* If Xen mapped, then remove it */
	pt_amd_free_vf ();

	if (ati_pci_gfx_dev != NULL) {
		pci_unregister_device(ati_pci_gfx_dev);  // This is a neat fn, check out the unregister callback
		ati_pci_gfx_dev = NULL;
	}
	if (pt_amd_mmio_size != 0)
	{
		pt_munmap (pt_amd_mmio_ptr, pt_amd_mmio_size);
		pt_amd_mmio_size = 0;
	}
}
/* This callback function is called each time a mmio region has been updated */

/*
 * Register the read/write functions that will be called when the guest 
 * accesses a register in the MMIO range
 */
static void pt_iomem_amd_mmio_map(PCIDevice *d, int i, uint32_t e_phys, uint32_t e_size,
			 int type)
{
	/* Index = BAR num */

	struct pt_dev *assigned_device  = (struct pt_dev *)d;
	int first_map = ( assigned_device->bases[i].e_size == 0 );
	int ret = 0;
	int mem_handle;

	/*
	 * e_phys - emulated physical address.  Guest thinks this is the physical address
	 * maddr  - real machine physical address accessed via this host virtul address.
	 */
	PT_LOG("ati - e_phys=%08x maddr=%lx type=%d len=%d index=%d first_map=%d\n",
	e_phys, (unsigned long)assigned_device->bases[i].access.maddr,
	type, e_size, i, first_map);

	if ( e_size == 0 ) {
		PT_LOG ("e_size == 0, don't do anything\n");
		return;
	}

	if ( !first_map && assigned_device->bases[i].e_physbase != -1 ) {
		PT_LOG ("Remove mapping for graphics BAR%d (0x%08lx)\n", i, assigned_device->bases[i].e_physbase);
		PT_LOG ("Callback wants to remove Xen Mapping\n");
		pt_disable_mmio ();
	}

	assigned_device->bases[i].e_physbase = e_phys;
	assigned_device->bases[i].e_size= e_size;

	/* map only valid guest address */
	if (e_phys != -1) {
		PT_LOG ("Trap guest physical addr 0x%08x access on BAR%d.  Mapped to local ptr 0x%08lx in domain %d\n",
			e_phys, i, (uint64_t) pt_amd_mmio_ptr, domid);
		PT_LOG ("Callback wants to set up pass through mapping\n");
		pt_enable_mmio ();
	}
}

/*
 * Caller has already tested that the device is an AMD
 * Graphics device and it is a Virtual Function (VF)
 */
int register_amd_vf_region (struct pt_dev *real_device)
{
	struct pci_dev  *pci_dev = real_device->pci_dev;
	uint64_t		mmio_phys_addr = 0l;

	PT_LOG ("Register callback function MMIO BAR changing\n");

	if (!real_device->is_virtfn)
		return -1;

	/* Find MMIO BAR */

	if (pt_ati_get_mmio_bar_index (pci_dev, &pt_amd_mmio_bar_num)) {
		PT_LOG ("Could not find MMIO BAR for mapping/trapping\n");
		return -1;
	}

	PT_LOG ("MMIO is at BAR%d\n", pt_amd_mmio_bar_num);

	mmio_phys_addr += pci_dev->base_addr[pt_amd_mmio_bar_num] & 0xFFFFFFF0;
	pt_amd_mmio_size = pci_dev->size[pt_amd_mmio_bar_num]; 

	PT_LOG ("MMIO is at host physical address 0x%08lx, size = 0x%08lx\n", mmio_phys_addr, pt_amd_mmio_size);

	/* Get a local pointer to the MMIO for Trapping emulation */
	pt_amd_mmio_ptr = pt_mmap (mmio_phys_addr, pt_amd_mmio_size);

	PT_LOG ("Map physical MMIO space 0x%08lx to local ptr 0x%08lx\n", 
			mmio_phys_addr, (long unsigned int) pt_amd_mmio_ptr);

	/*
 	 * Hijack the callback when the MMIO BAR changes
	 * This has been previoulsy set to pt_iomem_map in passthrough.c
	 * but we need to over ride it.
	 *
	 * Whenever the guest writes to the MMIO BAR the callback
	 * will get called.  We need to track the guest address in the 
	 * MMIO BAR.
	 */
	pci_register_io_region((PCIDevice *)real_device, pt_amd_mmio_bar_num,
		(uint32_t)pt_amd_mmio_size, PCI_ADDRESS_SPACE_MEM,
		pt_iomem_amd_mmio_map);


	ati_pci_gfx_dev = (PCIDevice *)real_device;
	qemu_register_exit(pt_iomem_ati_mmio_cleanup, NULL);

	/* Tell GIM that we are ready to get started (via IOCTL) by allocating a VF*/
	pt_amd_alloc_vf ( pci_dev->bus, pci_dev->dev, pci_dev->func);

	return 0;
}

int unregister_amd_vf_region(struct pt_dev *real_device)
{
	int ret = 0;
	PCIDevice   *dev = (PCIDevice *)&real_device->dev;
	struct pci_dev  *pci_dev = real_device->pci_dev;


	PT_LOG ("NOT YET SUPPORTED: Need to untrap the BAR accesses\n");
	PT_LOG ("Unmap the virtual address space\n");
	pt_munmap (pt_amd_mmio_ptr, pt_amd_mmio_size);

	return ret;
}

static void dump_bad_mmio(void)
{
	int 	i;

	printf ("%d bad MMIO accesses detected\n", bad_mmio_count);	

	for (i=0; i<bad_mmio_count; ++i) {
		printf ("MMIO offset 0x%08x.  %d bad READs, %d bad WRITEs\n", 
			bad_mmios[i].offset, bad_mmios[i].read_count, bad_mmios[i].write_count);
	}
}

static int can_access_mmio (uint32_t offset, int is_write)
{
	int i;
	void *ptr;

	/*
	 * Check if the MMIO offset is emulated or not.
	 * If it is emulated then check if it is valid or not valid
	 * If it is valid then return 0x1 to indicate that the MMIO 
	 * access is permitted
	 */
	for (i=0; i<amd_num_emulated_mmio; ++i) {
		if (amd_emulated_mmio[i].offset == offset) {
			if (amd_emulated_mmio[i].valid) {
				++good_MMIO_count;
			return 1;
			} else {
				++bad_MMIO_count;
				return 0;
			}
		}
	}

	/*
	 * The MMIO offset is not emulated.  Check if the default
	 * behavior is to pass through or to block.  If it is passthrough
	 * then return 0x1
	 */
	if (amd_default_mmio_behavior == DO_NOT_BLOCK_MMIO) {
		++good_MMIO_count;
		return 1;
	}

	/*
	 * The MMIO access is not valid.  This function will return 0x0
	 * The remainder of this function is for tracking purposes only.
	 * this will log an entry for the bad MMIO access that can be reported
	 * in the log file.
	 */

	/* Debug tracking summary of Bad MMIO accesses */

	/*
	 * Check if the MMIO has previously had a hit.  If an entry already exists
	 * then just increment either the invalid read or write attempt.
	 */
	for (i=0; i<bad_mmio_count; ++i) {
		if (bad_mmios[i].offset == offset) {
			if (is_write)
				++bad_mmios[i].write_count;
			else
				++bad_mmios[i].read_count;
			return (0);
		}
	}

	/*
	 * Check if need to make a new entry in the list.  If the list is not large enough
	 * then need to reallocate the list larger by BAD_MMIO_INC number of elements
	 */
	if (i >= max_bad_mmios) {
		ptr = realloc (bad_mmios, sizeof (struct mmio_counter) * (max_bad_mmios + BAD_MMIO_INC));
		if (ptr != NULL) {
			bad_mmios = ptr;
			max_bad_mmios += BAD_MMIO_INC;
		} else {
			PT_LOG ("Failed to enlarge the bad MMIO list.  Offset 0x%04x not added\n", offset);
		}
	}

	/*
 	 * If the realloc worked or the list was already large enough, use another entry
	 */
	if (i < max_bad_mmios) {
		bad_mmios[i].offset = offset;
		if (is_write) {
			bad_mmios[i].write_count = 1;;
			bad_mmios[i].read_count = 0;
		} else {
			bad_mmios[i].write_count = 0;;
			bad_mmios[i].read_count = 1;
		}

		bad_mmio_count = i+1;
	} else {
		printf ("No room for more bad MMIOs.  Increase Array size\n");
	}
	
	return 0;
}

#define MAX_PASSTHROUGH_RANGES 16

struct passthrough_range {
	uint32_t	ebase;
	uint32_t	esize;
} amd_passthrough_ranges [MAX_PASSTHROUGH_RANGES];

static void add_passthrough_range (uint32_t offset, uint32_t len)
{
	int 	i;

	for (i=0; i<MAX_PASSTHROUGH_RANGES && amd_passthrough_ranges[i].esize != 0; ++i); 

	if (i == MAX_PASSTHROUGH_RANGES) {
		fprintf (stderr, "Out of entries in amd_passthrough_ranges[]\n");
		return;
	}
	
	amd_passthrough_ranges[i].ebase = offset;
	amd_passthrough_ranges[i].esize = len;
	
	PT_LOG ("Create new range entry 0x%04x to 0x%04x\n",
		amd_passthrough_ranges[i].ebase,
		amd_passthrough_ranges[i].ebase +  amd_passthrough_ranges[i].esize);
	pt_trap_needed = 0;
}

static void clear_passthrough_ranges (void)
{
	int	i;

	pt_trap_needed = 1;
	for (i=0; i < MAX_PASSTHROUGH_RANGES; ++i) {
		amd_passthrough_ranges[i].esize = 0;
		amd_passthrough_ranges[i].ebase = 0;
	}
}

static void pt_block_mmio (char *);

static void remove_from_ranges (uint32_t offset)
{
	int 	i;
	uint32_t	page_offset;
	uint32_t	first_page, last_page;
	char		range [32];

	PT_LOG ("Check if 0x%04x falls in an existing range\n", offset);

	/* Check if the offset falls into a pre-defined range */
	for (i=0; i<MAX_PASSTHROUGH_RANGES && amd_passthrough_ranges[i].esize != 0; ++i) {
		if (offset >= amd_passthrough_ranges[i].ebase && offset < amd_passthrough_ranges[i].ebase+amd_passthrough_ranges[i].esize) {
			PT_LOG ("Offset 0x%04x falls in range from 0x%04x to 0x%04x\n",
				offset,
				amd_passthrough_ranges[i].ebase,
				amd_passthrough_ranges[i].ebase+amd_passthrough_ranges[i].esize-1);
			page_offset = offset / XC_PAGE_SIZE;
			if (amd_passthrough_ranges[i].esize == XC_PAGE_SIZE) {
				PT_LOG ("Range is only a single page.  Need to shuffle other pages up\n");
				/* Shift all entries up by one */
				amd_passthrough_ranges[i].esize = 0;		/* In case it is last entry	*/
				for (; (i+1)<MAX_PASSTHROUGH_RANGES && amd_passthrough_ranges[i+1].esize != 0; ++i) {
					amd_passthrough_ranges[i].esize = amd_passthrough_ranges[i+1].esize;	
					amd_passthrough_ranges[i].ebase = amd_passthrough_ranges[i+1].ebase;
				}
				amd_passthrough_ranges[i].esize = 0;	/* Clear the last entry	*/
			} else {
				first_page = amd_passthrough_ranges[i].ebase / XC_PAGE_SIZE;
				last_page = (amd_passthrough_ranges[i].ebase + amd_passthrough_ranges[i].esize-1)/ XC_PAGE_SIZE;
				PT_LOG ("Offset is on page 0x%d in a range covering pages 0x%0x to 0x%0x\n", 
					page_offset, first_page, last_page);
				if (page_offset == first_page) {
					PT_LOG ("offset is on first page.  Move base up\n");
					amd_passthrough_ranges[i].ebase += XC_PAGE_SIZE;
					amd_passthrough_ranges[i].esize -= XC_PAGE_SIZE;
					PT_LOG ("Range entry becomes 0x%04x to 0x%04x\n",
						amd_passthrough_ranges[i].ebase,
						amd_passthrough_ranges[i].ebase +  amd_passthrough_ranges[i].esize);
				} else if (page_offset == last_page) {
					PT_LOG ("offset is one last page, make the range smaller\n");
					amd_passthrough_ranges[i].esize -= XC_PAGE_SIZE;
					PT_LOG ("Range entry becomes 0x%04x to 0x%04x\n",
						amd_passthrough_ranges[i].ebase,
						amd_passthrough_ranges[i].ebase +  amd_passthrough_ranges[i].esize);
				} else {
					PT_LOG ("Offset is in the middle of a range, need to split it.\n");
					amd_passthrough_ranges[i].esize = (page_offset - first_page) * XC_PAGE_SIZE;
					PT_LOG ("Range entry becomes 0x%04x to 0x%04x\n",
						amd_passthrough_ranges[i].ebase,
						amd_passthrough_ranges[i].ebase +  amd_passthrough_ranges[i].esize);
					add_passthrough_range ((page_offset+1) * XC_PAGE_SIZE, (last_page-(page_offset+1))* XC_PAGE_SIZE);
				}
			}
			break;	
		}
	}
}

static void _pt_amd_set_single_mapping (uint32_t e_base, uint64_t maddr, uint32_t e_size, int op)
{
	PT_LOG ("%s mapping for Base 0x%08x and size 0x%04x\n", op == DPCI_REMOVE_MAPPING ? "REMOVE" : "ADD", e_base, e_size);
	xc_domain_memory_mapping(xc_handle, domid,
		e_base >> XC_PAGE_SHIFT,
		maddr >> XC_PAGE_SHIFT,
		(e_size + XC_PAGE_SIZE - 1) >> XC_PAGE_SHIFT, op);
}
static void pt_amd_set_mapping (int op)
{
	int	i;
	struct pt_dev 	*assigned_device;
	uint32_t	e_phys, e_size;
	uint64_t	maddr;
	uint32_t	gaddr;
	uint32_t	base_delta;
	uint32_t	size;

	PT_LOG ("Update the Xen Mapping\n");

	assigned_device = (struct pt_dev *)ati_pci_gfx_dev;
	e_phys = assigned_device->bases[pt_amd_mmio_bar_num].e_physbase;
	e_size = assigned_device->bases[pt_amd_mmio_bar_num].e_size;

	for (i=0; i < MAX_PASSTHROUGH_RANGES &&  amd_passthrough_ranges[i].esize != 0; ++i) {
		base_delta = amd_passthrough_ranges[i].ebase;
		size = amd_passthrough_ranges[i].esize;
		if (size + base_delta > e_size )
			size = e_size - base_delta;

		maddr =	assigned_device->bases[pt_amd_mmio_bar_num].access.maddr + base_delta;
		gaddr = e_phys + base_delta;
		_pt_amd_set_single_mapping (gaddr, maddr, size, op);
	}
	if (i == 0) {
		PT_LOG ("WARNING: No ranges defined for op = %s\n", op == DPCI_REMOVE_MAPPING ? "REMOVE" : "ADD");
	}
	PT_LOG ("Xen Mapping complete\n");
}

static void pt_amd_vf_trap_mmio (void)
{
	struct pt_dev 	*assigned_device;
	uint32_t	e_phys, e_size;

	assigned_device = (struct pt_dev *)ati_pci_gfx_dev;
	e_phys = assigned_device->bases[pt_amd_mmio_bar_num].e_physbase;
	e_size = assigned_device->bases[pt_amd_mmio_bar_num].e_size;

	PT_LOG ("Received a request to start MMIO TRAPPING (%p) - needed=%d, mmio_is_xen_mapped=%d\n", 
		ati_pci_gfx_dev, pt_trap_needed, amd_mmio_is_xen_mapped);

	if (pt_amd_mem_handle == 0) {
		PT_LOG ("Create a pt_amd_mem_handle for the readl/writel callback functions\n");
		pt_amd_mem_handle = cpu_register_io_memory (0, pt_graphics_mmio_read, pt_graphics_mmio_write, assigned_device); 
		PT_LOG ("pt_amd_mem_handle set to %d\n", pt_amd_mem_handle);
	}
	if (e_size == 0) {
		PT_LOG ("Can't enable mapping because e_size == 0\n");
		return;
	}

	if (e_phys == -1) {
		PT_LOG ("0x%08x has been written to MMIO BAR to determine its size.  Don't map it!\n", e_phys);
		return;
	}

	PT_LOG ("Register physical memory 0x%08x for size 0x%08x with handle %d\n", e_phys, e_size, pt_amd_mem_handle);
	cpu_register_physical_memory (e_phys, e_size, pt_amd_mem_handle);
	pt_amd_guest_mmio_bar = e_phys;		/* Save for readl/writel */

	if (amd_mmio_is_xen_mapped) {
		PT_LOG ("Remove Xen mapping so that readl/writel are called\n");
		PT_LOG ("Trap all MMIO accesses to readl() and writel()\n");
	 	PT_LOG ("Trap guest physical addr 0x%08x access on BAR%d.  Mapped to local ptr 0x%08lx in domain %d\n",
			e_phys, pt_amd_mmio_bar_num,  (uint64_t) pt_amd_mmio_ptr, domid);
		amd_mmio_is_xen_mapped = 0;
		pt_amd_set_mapping (DPCI_REMOVE_MAPPING);
	} else {
		PT_LOG ("MMIO Trapping is already enabled. Therefore it was not enabled again\n");
	}
}

static void pt_amd_vf_passthru_mmio(void)
{
	struct pt_dev	*assigned_device;
	uint32_t	e_phys;
	uint32_t	e_size;

	assigned_device = (struct pt_dev *)ati_pci_gfx_dev;
	e_phys = assigned_device->bases[pt_amd_mmio_bar_num].e_physbase;
	e_size = assigned_device->bases[pt_amd_mmio_bar_num].e_size;

	PT_LOG ("Received a request to stop MMIO TRAPPING (%p) - needed=%d, mmio_is_mapped=%d\n",
		ati_pci_gfx_dev, pt_trap_needed, amd_mmio_is_xen_mapped);

	if (e_size == 0) {
		PT_LOG ("Can't enable mapping because e_size == 0\n");
		return;
	}

	if (e_phys == -1) {
		PT_LOG ("0x%08x has been written to MMIO BAR to determine its size.  Don't map it!\n", e_phys);
		return;
	}

	pt_amd_guest_mmio_bar = e_phys;		/* Save for readl/writel */

	if ((!pt_trap_needed && !amd_mmio_is_xen_mapped)) {
		PT_LOG ("Allow striaght pass through of guest accessing MMIO\n");
		amd_mmio_is_xen_mapped = 1;
		pt_amd_set_mapping (DPCI_ADD_MAPPING);
	} else {
		PT_LOG ("Trapping was not enabled therefore nothing to unregister\n");
	}
}

static void pt_enable_mmio (void)
{
	PT_LOG ("MMIO BAR is valid\n");

	if (pt_trap_needed) {
		PT_LOG ("QEMU trapping is needed, enable readl/writel\n");
		pt_amd_vf_trap_mmio ();
	} else {
		PT_LOG ("Direct passthrough to MMIO without trapping\n");
		pt_amd_vf_passthru_mmio ();
	}
}

extern int _pt_iomem_helper(struct pt_dev *assigned_device, int i,
                            unsigned long e_base, unsigned long e_size,
                            int op);

static void pt_disable_mmio (void)
{
	struct pt_dev	*assigned_device;
	uint32_t	e_phys;
	uint32_t	e_size;

	assigned_device = (struct pt_dev *)ati_pci_gfx_dev;
	e_phys = assigned_device->bases[pt_amd_mmio_bar_num].e_physbase;
	e_size = assigned_device->bases[pt_amd_mmio_bar_num].e_size;

	PT_LOG ("MMIO BAR is not valid\n");

	if (amd_mmio_is_xen_mapped) {
		PT_LOG ("MMIO is Xen mapped as passthrough.  Need to remove the mapping\n");
		amd_mmio_is_xen_mapped = 0;
		_pt_iomem_helper(assigned_device, pt_amd_mmio_bar_num, e_phys, e_size,
				   DPCI_REMOVE_MAPPING);
		
	}
}


static void notify_gim (int msg)
{
        int ioctl_fd;

        ioctl_fd = open(gim_file_name, O_RDWR);
        if (ioctl_fd == -1) {
                fprintf (stderr, "Failed to open %s\n", gim_file_name);
                return;
        }
        PT_LOG ("Opened device %s\n", gim_file_name);

        if (ioctl(ioctl_fd, msg) == -1) {
                fprintf (stderr, "IOCTL call failed\n");
                return;
        }
        PT_LOG ("IOCTL was successful\n");

        close (ioctl_fd);
}

/*
 * Block a range of MMIO offsets
 * Currently only support blocking the entire MMIO BAR range
 * Blocking of sub-ranges not require therefore not supported.
 *
 * Currently the only valid range can be specified as "BA"
 */
static void pt_block_mmio (char *range)
{
	void	*ptr;
	char	*range_ptr;
	uint32_t val;
	int	num_mmio = sizeof (uint32_t);
	int	i;

	PT_LOG ("Request from GIM to BLOCK MMIO access \"%s\"\n", range);

	/* If BLOCK ALL then set the default behaviour */
	if (range[0] == 'A') {
		pt_amd_vf_trap_mmio ();
		amd_num_emulated_mmio = 0;
		bad_mmio_count = 0;
		clear_passthrough_ranges ();
		amd_default_mmio_behavior = BLOCK_MMIO;
	} else {
		/* Individual MMIO range was specified */
		val = strtoul (range, &range_ptr, 0);

		if (*range_ptr == '/') {        /* Range specified      */
			num_mmio = strtoul (&range_ptr[1], NULL, 0);
		}
		/* If the range is a multiple of an MMIO page then add the entire PAGE */
		num_mmio /=  sizeof (uint32_t);

		PT_LOG ("Remove %d consecutive MMIO offsets from the valid emulated MMIO list\n", num_mmio);

		while (num_mmio) {
			/* Check if the emulated MMIO list is large enough	*/
			if ((amd_num_emulated_mmio) >= amd_emulated_mmio_size) {
				ptr = realloc (amd_emulated_mmio, 
					sizeof (struct emulated_mmio) * (amd_emulated_mmio_size + MMIO_SIZE_INCREMENT));
				if (ptr != NULL) {
					amd_emulated_mmio = ptr;
					amd_emulated_mmio_size += MMIO_SIZE_INCREMENT;
				} else {
					PT_LOG ("Cannot add %s to the valid MMIO list\n", range);
					PT_LOG ("Failed to increase the size of the list from %d entries to %d entries\n",
						amd_emulated_mmio_size, amd_emulated_mmio_size + MMIO_SIZE_INCREMENT);
					break;
				}
			}
			remove_from_ranges (val);
			amd_emulated_mmio[amd_num_emulated_mmio].offset = val;
			amd_emulated_mmio[amd_num_emulated_mmio].valid = 0;
			++amd_num_emulated_mmio;
			--num_mmio;
			val += 4;       /* Next MMIO offset  */
		}
	}
}

/*
 * Unblock an MMIO range
 *
 * Valid syntax is <offset>[/<range>]
 * where offset is an MMIO offset and range is the offset number of bytes to include in the range
 *
 * Range and offset can be either hex (0x...) format or decimal format.
 *
 * for example 0x5100/40 specifies a starting offset of 0x5100 for a length of 40 bytes (or 10 DWORDS)
 */
static void pt_unblock_mmio (char *range)
{
	void	*ptr;
	char	*range_ptr;
	uint32_t val;
	int	num_mmio = sizeof (uint32_t);
	int	i;

	struct pt_dev   *assigned_device;
        uint32_t        e_size;


        PT_LOG ("Request from GIM to UNBLOCK MMIO access \"%s\"\n", range);

	if (range[0] == 'A') {
		PT_LOG ("Unblock ALL MMIO range\n");
		clear_passthrough_ranges ();		/* Starting fresh */
        	assigned_device = (struct pt_dev *)ati_pci_gfx_dev;
		e_size = assigned_device->bases[pt_amd_mmio_bar_num].e_size;
		add_passthrough_range (0, e_size);	/* Add the entire range as passthrough */
		amd_default_mmio_behavior = DO_NOT_BLOCK_MMIO;
	} else {
		val = strtoul (range, &range_ptr, 0);

		if (*range_ptr == '/') {	/* Range specified	*/
			num_mmio = strtoul (&range_ptr[1], NULL, 0);
		}

		num_mmio /=  sizeof (uint32_t);

		PT_LOG ("Add %d consecutive MMIO offsets to the valid emulated MMIO list\n", num_mmio);

		while (num_mmio) {
			if ((amd_num_emulated_mmio) >= amd_emulated_mmio_size) {
				ptr = realloc (amd_emulated_mmio, sizeof (struct emulated_mmio) * (amd_emulated_mmio_size + MMIO_SIZE_INCREMENT));
				if (ptr != NULL) {
					amd_emulated_mmio = ptr;
					amd_emulated_mmio_size += MMIO_SIZE_INCREMENT;
				} else {
					PT_LOG ("Cannot add %s to the valid MMIO list\n", range);
					PT_LOG ("Failed to increase the size of the list from %d entries to %d entries\n",
						amd_emulated_mmio_size, amd_emulated_mmio_size + MMIO_SIZE_INCREMENT);
					break;
				}
			}
			amd_emulated_mmio[amd_num_emulated_mmio].offset = val;
			amd_emulated_mmio[amd_num_emulated_mmio].valid = 1;
			++amd_num_emulated_mmio;	
			--num_mmio;
			val += 4;	/* Next MMIO offset  */
		}
	}
}

static void remove_spaces (char *cmd)
{
	int in, out;

	in = 0;
	out = 0;

 	for (in=0; cmd[in] != '\0'; ++in) {
		if (!isspace((unsigned char) cmd[in])) {
			cmd[out] = cmd[in];
			++out;
		}
	}
	cmd[out] = '\0';
}

static void pt_execute_token (char *cmd)
{
	if (!strncmp (cmd, "B", 1)) {		/* 'B' for BLOCK MMIO */
		pt_block_mmio (&cmd[1]);
	} else if (!strncmp (cmd, "U", 1)) {	/* 'U' for UNBLOCK MMIO */
		pt_unblock_mmio (&cmd[1]);
	} else {
		fprintf (stderr, "gim_execute_token() Unknown command \"%s\"\n", cmd);
	}
}

static void pt_execute (char *cmd)
{
	int 	i;
	char *token;
	char *save_ptr;

	remove_spaces (cmd);
	PT_LOG ("GIM command = \"%s\"\n", cmd);

	token = strtok_r (cmd, ",", &save_ptr);

	while (token != NULL) {
		pt_execute_token (token);
	
		token = strtok_r (NULL, ",", &save_ptr);
	}
	if (amd_passthrough_ranges[0].esize != 0) {
		PT_LOG ("There is a MMIO PASSTHRU range defined\n");
#ifndef MMIO_LOGGING
		pt_amd_vf_passthru_mmio ();
#endif
		notify_gim (GIM_IOCTL_MMIO_IS_PASS_THROUGH);
	} else {
		PT_LOG ("MMIO should be blocked and emulated\n");
		notify_gim (GIM_IOCTL_MMIO_IS_BLOCKED);
	}

	PT_LOG ("%d good MMIOs, %d bad MMIOs\n", good_MMIO_count, bad_MMIO_count);
#ifdef DEBUGGING
	PT_LOG ("%d entries in the MMIO list\n", amd_num_emulated_mmio);
	for (i=0; i<amd_num_emulated_mmio; ++i) {
		PT_LOG ("[%3d] - 0x%04x\n", i, amd_emulated_mmio[i]);
	}
#endif
}
/*
 * GIM writes to sysfs file will land in this callback function
 * opaque contains the file descriptor to read
 */

#define MAX_SYSFS_READ 4096
static 	char sysfs_buf[MAX_SYSFS_READ+1];

volatile int stop_and_wait = 1;

static void pt_exception (void *opaque)
{
	int i;
	int fd;
	int rc;


	PT_LOG ("pt_exception: received an exception. Data = %d, My buffer is %d MAX size\n", (int)(long)opaque, MAX_SYSFS_READ);

	fd = (int)(long)opaque;
	memset (sysfs_buf, 0, MAX_SYSFS_READ+1);
	rc = read (fd, sysfs_buf, MAX_SYSFS_READ);

	if (rc > MAX_SYSFS_READ) {
		PT_LOG ("Message from GIM is too large for %d sized buffer\n", MAX_SYSFS_READ);
	}

	if (rc < 0)	/* Error with pipe or no data */
		return;

	if (rc == 0) {
		/* There is data in the pipe but I need to close and reopen */

		close (fd);

		fd = open (gim_sysfs_pipe, O_RDONLY);
		PT_LOG ("Reopening \"%s\" returns fd = %d\n", gim_sysfs_pipe, fd);

		memset (sysfs_buf, 0, MAX_SYSFS_READ+1);

		i = 0;
		do {
			rc = read (fd, &sysfs_buf-i, MAX_SYSFS_READ-i);
			PT_LOG ("Read returns %d bytes\n", rc);
			if (rc > 0)
				i += rc;
		} while (rc > 0);

		PT_LOG ("GIM sent me \"%s\" with last rc = %d and datalen = %d\n", sysfs_buf, rc, i);
	}

	dump_bad_mmio ();
	pt_execute (sysfs_buf);

	if (fd != (int)(long) opaque) {
		PT_LOG ("File descriptor has changed.  Need to re-register with QEMU select handler\n");
		pt_amd_sysfs_fd = fd;

		qemu_set_fd_handler3 ((int)(long) opaque, NULL, NULL, NULL, NULL, NULL);
		qemu_set_fd_handler3 (pt_amd_sysfs_fd, NULL, NULL, NULL, pt_exception, (void *)(long)pt_amd_sysfs_fd);
	}
}

int pt_amd_alloc_vf (uint8_t bus, uint8_t dev, uint8_t fn)
{
	struct gim_ioctl_alloc_vf vf;				/* Interface structure for IOCTL */
	int ioctl_fd;
	uint32_t bdf;
	int	rc;

	bdf = (bus << 8) + (dev << 3) + fn;

	PT_LOG ("Ask GIM to allocate a VF for BDF = %02x:%02x.%x (0x%02x)\n", bus, dev, fn, bdf);
	ioctl_fd = open(gim_file_name, O_RDWR);
	if (ioctl_fd == -1) {
		fprintf (stderr, "Failed to open %s\n", gim_file_name);
		return 2;
	}
	PT_LOG ("Opened device %s\n", gim_file_name);

	memset(&vf, 0, sizeof(vf));
	vf.bdf = bdf;

	if ((rc=ioctl(ioctl_fd, GIM_IOCTL_ALLOC_VF, &vf)) == -1) {
		fprintf (stderr, "IOCTL: GIM_IOCTL_ALLOC_VF failed\n");
	} else {
		PT_LOG ("IOCTL GIM_IOCTL_ALLOC_VF was successful\n");
	}

	if (errno == EAGAIN) {
		PT_LOG ("FB was not cleared but can still continue on\n");
	} else {
		PT_LOG ("ALLOC_VF failed with rc %d.  Should follow up\n", errno);
	}

	close (ioctl_fd);

	sprintf (gim_sysfs_pipe, "%sqemu-%d", sysfs_dir, getpid());
	PT_LOG ("Using sys file %s\n", gim_sysfs_pipe);

	pt_amd_sysfs_fd = open(gim_sysfs_pipe, O_RDONLY);   // Opening a file generates a poll event.  Need to read to clear it
	if (pt_amd_sysfs_fd == -1) {
		fprintf (stderr, "Failed to open %s\n", gim_sysfs_pipe);
		return 2;
	}

	qemu_set_fd_handler3 (pt_amd_sysfs_fd, NULL, NULL, NULL, pt_exception, (void *)(long)pt_amd_sysfs_fd);
	PT_LOG ("Sysfs \"%s\" is open and waiting for GIM\n", gim_sysfs_pipe);

	return 0;
}

static void pt_amd_free_vf (void)
{
	int ioctl_fd;

	PT_LOG ("Tell GIM to free the VF\n");

	qemu_set_fd_handler3 (pt_amd_sysfs_fd, NULL, NULL, NULL, NULL, NULL);

	ioctl_fd = open(gim_file_name, O_RDWR);
	if (ioctl_fd == -1) {
		fprintf (stderr, "Failed to open %s\n", gim_file_name);
		return;
	}
	PT_LOG ("Opened device %s\n", gim_file_name);

	if (ioctl(ioctl_fd, GIM_IOCTL_FREE_VF) == -1) {
		fprintf (stderr, "IOCTL call failed\n");
		return;
	}
	PT_LOG ("FREE_VF IOCTL was successful\n");

	close (ioctl_fd);
}
