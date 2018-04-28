/*
 * QEMU vGT/XenGT Legacy VGA support
 *
 * Copyright (c) 2003 Fabrice Bellard
 * Copyright (c) Citrix Systems, Inc
 * Copyright (c) Intel
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
#include "console.h"
#include "pc.h"
#include "pci.h"
#include "xen.h"
typedef uint32_t pci_addr_t;
#include "pci_host.h"
#include "vga_int.h"
#include "pixel_ops.h"
#include "qemu-timer.h"
#include "vga-xengt.h"
#include "qemu-log.h"
#include "pass-through.h"
#include "assert.h"

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <libdrm/drm.h>
#include <libdrm/i915_drm.h>
#include <xf86drm.h>
#include <sys/time.h>


#include <libdrm/intel_bufmgr.h>
#include <libdrm/drm_fourcc.h>
#include "intel-tools/intel_batchbuffer.h"
#include "intel-tools/intel_chipset.h"
#include "host-utils.h"
/*  #include "intel-tools/intel_gpu_tools.h" */


#if 0
typedef struct vgt_vga_state {
    PCIDevice dev;
    //struct VGACommonState state;
    int num_displays;
    struct pci_dev host_dev;
    bool instance_created;
} vgt_vga_state_t;
#endif

#define EDID_SIZE 128
#define MAX_INPUT_NUM 3
#define MAX_FILE_NAME_LENGTH 128

/* port definition must align with gvt-g driver */
enum vgt_port {
    PORT_A = 0,
    PORT_B,
    PORT_C,
    PORT_D,
    PORT_E,
    MAX_PORTS
};

typedef struct vgt_monitor_info {
    unsigned char port_type:4;
    unsigned char port_is_dp:4;  /* 0 = HDMI PORT, 1 = DP port, only valid for PORT_B/C/D */
    unsigned char port_override;
    unsigned char edid[EDID_SIZE];
}vgt_monitor_info_t;

/* These are the default values */
int vgt_low_gm_sz = 128; /* in MB */
int vgt_high_gm_sz = 448; /* in MB */
int vgt_fence_sz = 4;
int vgt_primary = 1; /* -1 means "not specified */
int vgt_cap = 0;
const char *vgt_monitor_config_file = "/etc/gvt-g-monitor.conf";

int vgt_legacy_vga_ram_size;
void vga_map(PCIDevice *pci_dev, int region_num,
                    uint32_t addr, uint32_t size, int type);


static inline unsigned int port_info_to_type(unsigned char port_is_dp, int port)
{
    /* port type definition must align with gvt-g driver */
    enum vgt_port_type {
        VGT_CRT = 0,
        VGT_DP_A,
        VGT_DP_B,
        VGT_DP_C,
        VGT_DP_D,
        VGT_HDMI_B,
        VGT_HDMI_C,
        VGT_HDMI_D,
        VGT_PORT_TYPE_MAX
    } ret;

    switch (port) {
        case PORT_A:
            ret = VGT_DP_A;
            break;
        case PORT_B:
            ret = (port_is_dp) ? VGT_DP_B : VGT_HDMI_B;
            break;
        case PORT_C:
            ret = (port_is_dp) ? VGT_DP_C : VGT_HDMI_C;
            break;
        case PORT_D:
            ret = (port_is_dp) ? VGT_DP_D : VGT_HDMI_D;
            break;
	case PORT_E:
            ret = VGT_CRT;
            break;
        default:
            ret = VGT_PORT_TYPE_MAX;
            break;
    }

    return ret;
}

static bool validate_monitor_configs(vgt_monitor_info_t *config)
{
    if (config->port_type >= MAX_PORTS) {
        qemu_log("vGT: %s failed because the invalid port_type input: %d!\n",
            __func__, config->port_type);
        return false;
    }
    if (config->port_override >= MAX_PORTS) {
        qemu_log("vGT: %s failed due to the invalid port_override input: %d!\n",
            __func__, config->port_override);
        return false;
    }
    if (config->edid[126] != 0) {
        qemu_log("vGT: %s failed because there is extended block in EDID! "
            "(EDID[126] is not zero)\n", __func__);
        return false;
    }

    return true;
}

static void config_hvm_monitors(vgt_monitor_info_t *config)
{
    const char *path_prefix = "/sys/kernel/vgt/vm";
    FILE *fp;
    char file_name[MAX_FILE_NAME_LENGTH];
    int ret;

    // override
    snprintf(file_name, MAX_FILE_NAME_LENGTH, "%s%d/PORT_%c/port_override",
        path_prefix, xen_domid, 'A' + config->port_type);
    if ((fp = fopen(file_name, "w")) == NULL) {
        qemu_log("vGT: %s failed to open file %s! errno = %d\n",
            __func__, file_name, errno);
        return;
    }
    fprintf(fp, "PORT_%c", 'A' + config->port_override);
    if (fclose(fp) != 0) {
        qemu_log("vGT: %s failed to close file: errno = %d\n", __func__, errno);
    }

    // type
    snprintf(file_name, MAX_FILE_NAME_LENGTH, "%s%d/PORT_%c/type",
        path_prefix, xen_domid, 'A' + config->port_type);
    if ((fp = fopen(file_name, "w")) == NULL) {
        qemu_log("vGT: %s failed to open file %s! errno = %d\n",
            __func__, file_name, errno);
        return;
    }
    fprintf(fp, "%d", port_info_to_type(config->port_is_dp, config->port_type));
    if (fclose(fp) != 0) {
        qemu_log("vGT: %s failed to close file: errno = %d\n", __func__, errno);
    }

    // edid
    snprintf(file_name, MAX_FILE_NAME_LENGTH, "%s%d/PORT_%c/edid",
        path_prefix, xen_domid, 'A' + config->port_type);
    if ((fp = fopen(file_name, "w")) == NULL) {
        qemu_log("vGT: %s failed to open file %s! errno = %d\n",
            __func__, file_name, errno);
        return;
    }
    ret = fwrite(config->edid, 1, EDID_SIZE, fp);
    if (ret != EDID_SIZE) {
        qemu_log("vGT: %s failed to write EDID with returned size %d: "
            "errno = %d\n", __func__, ret, errno);
    }
    if (fclose(fp) != 0) {
        qemu_log("vGT: %s failed to close file: errno = %d\n", __func__, errno);
    }

    // flush result to port structure
    snprintf(file_name, MAX_FILE_NAME_LENGTH, "%s%d/PORT_%c/connection",
        path_prefix, xen_domid, 'A' + config->port_type);
    if ((fp = fopen(file_name, "w")) == NULL) {
        qemu_log("vGT: %s failed to open file %s! errno = %d\n",
            __func__, file_name, errno);
        return;
    }
    fprintf(fp, "flush");
    if (fclose(fp) != 0) {
        qemu_log("vGT: %s failed to close file: errno = %d\n", __func__, errno);
    }
}

#define CTOI(chr) \
    (chr >= '0' && chr <= '9' ? chr - '0' : \
    (chr >= 'a' && chr <= 'f' ? chr - 'a' + 10 :\
    (chr >= 'A' && chr <= 'F' ? chr - 'A' + 10 : -1)))

static int get_byte_from_txt_file(FILE *file, const char *file_name)
{
    int i;
    int val[2];

    for (i = 0; i < 2; ++ i) {
        do {
            unsigned char buf;
            if (fread(&buf, 1, 1, file) != 1) {
                qemu_log("vGT: %s failed to get byte from text file %s with errno: %d!\n",
                    __func__, file_name, errno);
                return -1;
            }

            if (buf == '#') {
                // ignore comments
                int ret;
                while (((ret = fread(&buf, 1, 1, file)) == 1) && (buf != '\n')) ;
                if (ret != 1) {
                    qemu_log("vGT: %s failed to proceed after comment string "
                            "from text file %s with errno: %d!\n",
                            __func__, file_name, errno);
                    return -1;
                }
            }

            val[i] = CTOI(buf);
        } while (val[i] == -1);
    }

    return ((val[0] << 4) | val[1]);
}

static int get_config_header(unsigned char *buf, FILE *file, const char *file_name)
{
    int ret;
    unsigned char chr;

    if (fread(&chr, 1, 1, file) != 1) {
        qemu_log("vGT: %s failed to get byte from text file %s with errno: %d!\n",
            __func__, file_name, errno);
        return -1;
    }

    if (chr == '#') {
        // it is text format input.
        while (((ret = fread(&chr, 1, 1, file)) == 1) && (chr != '\n')) ;
        if (ret != 1) {
            qemu_log("vGT: %s failed to proceed after comment string "
                "from file %s with errno: %d!\n",
                __func__, file_name, errno);
            return -1;
        }
        ret = get_byte_from_txt_file(file, file_name);
        buf[0] = 1;
        buf[1] = (ret & 0xf);
    } else {
        if ((ret = fread(&buf[0], 1, 2, file)) != 2) {
            qemu_log("vGT: %s failed to read file %s! "
                "Expect to read %d bytes but only got %d bytes! errno: %d\n",
                __func__, file_name, 2, ret, errno);
            return -1;
        }

        if (buf[0] != 0) {
            // it is text format input.
            buf[1] -= '0';
        }
    }

    return 0;
}

static void config_vgt_guest_monitors(void)
{
    FILE *monitor_config_f;
    unsigned char buf[4];
    vgt_monitor_info_t monitor_configs[MAX_INPUT_NUM];
    bool text_mode;
    int input_items;
    int ret, i;

    if (!vgt_monitor_config_file) {
        return;
    }

    if ((monitor_config_f = fopen(vgt_monitor_config_file, "r")) == NULL) {
        qemu_log("vGT: %s failed to open file %s! errno = %d\n",
            __func__, vgt_monitor_config_file, errno);
        return;
    }

    if (get_config_header(buf, monitor_config_f, vgt_monitor_config_file) != 0) {
        goto finish_config;
    }

    text_mode = !!buf[0];
    input_items = buf[1];

    if (input_items <= 0 || input_items > MAX_INPUT_NUM) {
        qemu_log("vGT: %s, Out of range input of the number of items! "
            "Should be [1 - 3] but input is %d\n", __func__, input_items);
        goto finish_config;
    }

    if (text_mode) {
        unsigned int total = sizeof(vgt_monitor_info_t) * input_items;
        unsigned char *p = (unsigned char *)monitor_configs;
        for (i = 0; i < total; ++i, ++p) {
            unsigned int val = get_byte_from_txt_file(monitor_config_f,
                vgt_monitor_config_file);
            if (val == -1) {
                break;
            } else {
                *p = val;
            }
        }
        if (i < total) {
            goto finish_config;
        }
    } else {
        unsigned int total = sizeof(vgt_monitor_info_t) * input_items;
        ret = fread(monitor_configs, sizeof(vgt_monitor_info_t), input_items,
                    monitor_config_f);
        if (ret != total) {
            qemu_log("vGT: %s failed to read file %s! "
                "Expect to read %d bytes but only got %d bytes! errno: %d\n",
                 __func__, vgt_monitor_config_file, total, ret, errno);
            goto finish_config;
        }
    }

    for (i = 0; i < input_items; ++ i) {
        if (validate_monitor_configs(&monitor_configs[i]) == false) {
            qemu_log("vGT: %s the monitor config[%d] input from %s is not valid!\n",
                __func__, i, vgt_monitor_config_file);
            goto finish_config;
        }
    }
    for (i = 0; i < input_items; ++ i) {
        config_hvm_monitors(&monitor_configs[i]);
    }

finish_config:
    if (fclose(monitor_config_f) != 0) {
        qemu_log("vGT: %s failed to close file %s: errno = %d\n", __func__,
            vgt_monitor_config_file, errno);
    }
    return;
}

int drm_fd;

static int xengt_enabled;

int xengt_is_enabled(void)
{
    struct drm_i915_gem_vgtbuffer gem_vgtbuffer;
    int rc;

    if (xengt_enabled)
        goto done;

    memset(&gem_vgtbuffer, 0, sizeof (gem_vgtbuffer));

    gem_vgtbuffer.plane_id = I915_VGT_PLANE_PRIMARY;
    gem_vgtbuffer.vmid = xen_domid;
    gem_vgtbuffer.pipe_id = 0;
    gem_vgtbuffer.flags = I915_VGTBUFFER_QUERY_ONLY;

    rc = drmIoctl(drm_fd, DRM_IOCTL_I915_GEM_VGTBUFFER, &gem_vgtbuffer);
    if (rc < 0)
        goto done;

    xengt_enabled = !!gem_vgtbuffer.start;

    if (xengt_enabled)
        qemu_log("vGT: enabled\n");

done:
    return xengt_enabled;
}

static drm_intel_bufmgr *gem_vgt_bufmgr;
struct intel_batchbuffer *gem_vgt_batchbuffer;

typedef struct xengt_surface {
    DisplayState *ds;
    drm_intel_bo *bo;
} xengt_surface_t;

static xengt_surface_t xengt_surface;

#define	P2ROUNDUP(_x, _a) -(-(_x) & -(_a))

static void xengt_destroy_display_surface(void)
{
    xengt_surface_t *surface = &xengt_surface;
    DisplayState *ds = surface->ds;
    int width;
    int height;

    if (surface->ds == NULL)
        return;

    qemu_log("vGT: %s\n", __func__);

    width = ds_get_width(ds);
    height = ds_get_height(ds);

    qemu_free_displaysurface(surface->ds);
    ds->surface = qemu_create_displaysurface(ds, width, height);

    surface->ds = NULL;

    drm_intel_bo_unmap(surface->bo);
    drm_intel_bo_unreference(surface->bo);
    surface->bo = NULL;
}

static void xengt_create_display_surface(DisplayState *ds,
                                         struct drm_i915_gem_vgtbuffer *gem_vgtbuffer,
                                         PixelFormat pf)
{
    xengt_surface_t *surface = &xengt_surface;
    uint32_t width = P2ROUNDUP(gem_vgtbuffer->width, 16);
    uint32_t linesize = width * gem_vgtbuffer->bpp / 8;

    surface->bo = drm_intel_bo_alloc(gem_vgt_bufmgr, "vnc",
                                     P2ROUNDUP(gem_vgtbuffer->height * linesize,
                                               4096),
                                     4096);
    if (surface->bo == NULL) {
        qemu_log("vGT: %s: failed to allocate buffer", __func__);
        return;
    }
        
    drm_intel_bo_map(surface->bo, 1);

    qemu_log("vGT: %s: w %d h %d, bbp %d , stride %d, fmt %x\n", __func__, width,
             gem_vgtbuffer->height,
             gem_vgtbuffer->bpp,
             linesize,
             gem_vgtbuffer->drm_format);

    qemu_free_displaysurface(ds);
    ds->surface = qemu_create_displaysurface_from(width,
                                                  gem_vgtbuffer->height,
                                                  gem_vgtbuffer->bpp,
                                                  linesize,
                                                  surface->bo->virtual);
    ds->surface->pf = pf;

    surface->ds = ds;
}

typedef struct xengt_fb {
    int64_t created;
    int64_t used;
    uint64_t epoch;
    struct drm_i915_gem_vgtbuffer gem_vgtbuffer;
    drm_intel_bo *bo;
} xengt_fb_t;

#define XENGT_NR_FB 16

static xengt_fb_t xengt_fb[XENGT_NR_FB];

static void xengt_close_object(uint32_t handle)
{
    struct drm_gem_close gem_close;

    memset(&gem_close, 0, sizeof (gem_close));
    gem_close.handle = handle;

    (void) drmIoctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &gem_close);
}

static unsigned int fb_count = 0;

static void xengt_release_fb(unsigned int i, const char *reason)
{
    xengt_fb_t *fb = &xengt_fb[i];

    if (fb->gem_vgtbuffer.handle == 0)
        return;

    qemu_log("vGT: %s %u (%s)\n", __func__, i, reason);

    if(fb->bo)
         drm_intel_bo_unreference(fb->bo);

    xengt_close_object(fb->gem_vgtbuffer.handle);

    memset(fb, 0, sizeof(*fb));
    --fb_count;

    if (fb_count == 0)
      xengt_destroy_display_surface();
}

QEMUTimer *drm_timer;

#define XENGT_TIMER_PERIOD 1000 /* ms */

/* Timeout to release a vgtbuffer object after last use */
#define XENGT_VGTBUFFER_EXPIRE 5000 /* ms */

static void xengt_timer(void *opaque)
{
    int64_t now;
    int i;

    now = qemu_get_clock(rt_clock);

    for (i = 0; i < XENGT_NR_FB; i++) {
        xengt_fb_t *fb = &xengt_fb[i];
        int64_t delta;

        if (fb->gem_vgtbuffer.handle == 0)
            continue;

        if ((now - fb->used) > XENGT_VGTBUFFER_EXPIRE)
            xengt_release_fb(i, "unused");
    }   

    qemu_mod_timer(drm_timer, now + XENGT_TIMER_PERIOD);
}

static void xengt_drm_init(void)
{
    drm_fd = open("/dev/dri/card0", O_RDWR);
    if (drm_fd < 0) {
        qemu_log("vGT: %s failed: errno=%d\n", __func__, errno);
        exit(-1);
    }

    qemu_log("vGT: %s opened drm\n", __func__);

    gem_vgt_bufmgr = drm_intel_bufmgr_gem_init(drm_fd, 4096);
    if (gem_vgt_bufmgr == NULL) {
        qemu_log("vGT: %s: drm_intel_bufmgr_gem_init failed\n", __func__);
        exit(-1);
    }

    drm_intel_bufmgr_gem_enable_reuse(gem_vgt_bufmgr);

    qemu_log("vGT: %s initialized bufmgr\n", __func__);

    gem_vgt_batchbuffer = intel_batchbuffer_alloc(gem_vgt_bufmgr, intel_get_drm_devid(drm_fd));
    if (gem_vgt_batchbuffer == NULL) {
        qemu_log("vGT: %s: intel_batchbuffer_alloc failed\n", __func__);
        exit(-1);
    }

    qemu_log("vGT: %s initialized batchbuffer\n", __func__);

    drm_timer = qemu_new_timer(rt_clock, xengt_timer, NULL);
    qemu_mod_timer(drm_timer, qemu_get_clock(rt_clock) + XENGT_TIMER_PERIOD);

    qemu_log("vGT: %s created timer\n", __func__);
}

static int gem_bo_globalize(uint32_t fd, uint32_t handle,  uint32_t* ghandle)
{
    int ret;
    struct drm_gem_flink flink;

    memset(&flink, 0, sizeof(flink));
    flink.handle = handle;

    ret = drmIoctl(fd, DRM_IOCTL_GEM_FLINK, &flink);
    if (ret != 0)
        return -errno;

    *ghandle = flink.name;
    return 0;
}

static xengt_fb_t *xengt_new_fb(struct drm_i915_gem_vgtbuffer *gem_vgtbuffer)
{
    xengt_fb_t *fb;
    static uint64_t epoch = 1;
    uint64_t oldest_epoch;
    unsigned int i, oldest;
    int rc;
    int global_handle = 0;

    oldest_epoch = epoch;
    oldest = XENGT_NR_FB;

    for (i = 0; i < XENGT_NR_FB; i++) {
        fb = &xengt_fb[i];

        if (fb->epoch < oldest_epoch) {
            oldest_epoch = fb->epoch;
            oldest = i;
        }
    }
    assert(oldest < XENGT_NR_FB);

    i = oldest;
    fb = &xengt_fb[i];

    xengt_release_fb(i, "spill");

    fb->used = fb->created = qemu_get_clock(rt_clock);
    fb->epoch = epoch++;
    fb->gem_vgtbuffer = *gem_vgtbuffer;

    rc = gem_bo_globalize(drm_fd, gem_vgtbuffer->handle, &global_handle);
    if (rc) {
        qemu_log("vGT: %s: Failed to link from handle %x!\n", __func__, gem_vgtbuffer->handle);
        return NULL;
    }

    fb->bo = drm_intel_bo_gem_create_from_name(gem_vgt_bufmgr, "src", global_handle);
    if (!fb->bo) {
         qemu_log("vGT: %s: Failed to create bo from handle %x!\n", __func__, global_handle);
         return NULL;
    }

    qemu_log("vGT: %s %u: Created bo, with size %ld, handle %d\n", __func__, i,
             fb->bo->size ,fb->bo->handle);

    fb_count++;
    return fb;
}

static xengt_fb_t *xengt_lookup_fb(struct drm_i915_gem_vgtbuffer *gem_vgtbuffer)
{
    int i;

    for (i = 0; i < XENGT_NR_FB; i++) {
        xengt_fb_t *fb = &xengt_fb[i];

        if (memcmp(&fb->gem_vgtbuffer,
                   gem_vgtbuffer,
                   offsetof(struct drm_i915_gem_vgtbuffer, handle)) == 0) {
            fb->used = qemu_get_clock(rt_clock);
            return fb;
        }
    }

    return NULL;
}

static void xengt_disable(void)
{
    int i;

    for (i = 0; i < XENGT_NR_FB; i++)
        xengt_release_fb(i, "disable");

    xengt_enabled = 0;
    qemu_log("vGT: disabled\n");
}

static xengt_fb_t *xengt_get_fb(void)
{
    struct drm_i915_gem_vgtbuffer gem_vgtbuffer;
    xengt_fb_t *fb = NULL;
    int rc;

    memset(&gem_vgtbuffer, 0, sizeof (gem_vgtbuffer));
    gem_vgtbuffer.plane_id = I915_VGT_PLANE_PRIMARY;
    gem_vgtbuffer.vmid = xen_domid;
    gem_vgtbuffer.pipe_id = 0;
    gem_vgtbuffer.flags = I915_VGTBUFFER_QUERY_ONLY;

    rc = drmIoctl(drm_fd, DRM_IOCTL_I915_GEM_VGTBUFFER, &gem_vgtbuffer);
    if (rc < 0) {
        xengt_disable();
        goto done;
    }

    if ((fb = xengt_lookup_fb(&gem_vgtbuffer)) != NULL)
        goto done;

    gem_vgtbuffer.flags = 0;

    rc = drmIoctl(drm_fd, DRM_IOCTL_I915_GEM_VGTBUFFER, &gem_vgtbuffer);
    if (rc < 0)
        goto done;

    if (unlikely((fb = xengt_lookup_fb(&gem_vgtbuffer)) != NULL)) {
        /* We don't need the new object so close it */
        xengt_close_object(gem_vgtbuffer.handle);
        goto done;
    }

    if ((fb = xengt_new_fb(&gem_vgtbuffer)) == NULL) {
        /* We can't use the new object so close it */
        xengt_close_object(gem_vgtbuffer.handle);
    }

done:
    return fb;
}

static int qemu_set_pixelformat(uint32_t drm_format, PixelFormat *pf)
{
    uint32_t red;
    uint32_t green;
    uint32_t blue;
    uint32_t alpha;
    int rc = 0;

    switch (drm_format) {
    case DRM_FORMAT_XRGB8888:
        red = 0xFF0000;
        green = 0xFF00;
        blue = 0xFF;
        alpha = 0xFF000000;
        break;

    case DRM_FORMAT_XBGR8888:
        red = 0xFF;
        green = 0xFF00;
        blue = 0xFF0000;
        alpha = 0xFF000000;
        break;

    case DRM_FORMAT_XBGR2101010:
        red = 0x3FF;
        green = 0xFFC00;
        blue = 0x3FF00000;
        alpha = 0xC0000000;
        break;

    case DRM_FORMAT_XRGB2101010:
        red = 0x3FF00000;
        green = 0xFFC00;
        blue = 0x3FF;
        alpha = 0xC0000000;
        break;

    default:
        rc = -1;
        break;
    }

    if (rc < 0)
        return rc;

    memset(pf, 0x00, sizeof(PixelFormat));

    pf->rmask = red;
    pf->gmask = green;
    pf->bmask = blue;

    pf->rbits = ctpop32(red);
    pf->gbits = ctpop32(green);
    pf->bbits = ctpop32(blue);
    pf->abits = ctpop32(alpha);

    pf->depth = pf->rbits + pf->gbits + pf->bbits;
    pf->bits_per_pixel = pf->depth + pf->abits;
    pf->bytes_per_pixel = pf->bits_per_pixel / 8;

    pf->rmax = (1 << pf->rbits) -1;
    pf->gmax = (1 << pf->gbits) -1;
    pf->bmax = (1 << pf->bbits) -1;
    pf->amax = (1 << pf->abits) -1;

    pf->rshift = ffs(red) - 1;
    pf->gshift = ffs(green) - 1;
    pf->bshift = ffs(blue) -1;
    pf->ashift = ffs(alpha) -1;

    return 0;
}

void xengt_draw_primary(DisplayState *ds, int full_update)
{
    xengt_surface_t *surface = &xengt_surface;
    xengt_fb_t *fb;
    struct drm_i915_gem_vgtbuffer *gem_vgtbuffer;
    PixelFormat pf;
    int rc;

    if (fb_count == 0)
        full_update = 1;

    if ((fb = xengt_get_fb()) == NULL || (fb->bo == NULL)) {
        if (xengt_enabled)
            qemu_log("vGT: %s: no frame buffer", __func__);
        return;
    }

    gem_vgtbuffer = &fb->gem_vgtbuffer;

    rc = qemu_set_pixelformat(gem_vgtbuffer->drm_format, &pf);
    if (rc < 0) {
        qemu_log("vGT: %s: unknown format (%08x)", __func__, gem_vgtbuffer->drm_format);
        return;
    }

    if (full_update ||
	surface->ds != ds ||
        ds_get_width(ds) != gem_vgtbuffer->width ||
        ds_get_height(ds) != gem_vgtbuffer->height ||
	memcmp(&ds->surface->pf, &pf, sizeof(PixelFormat)) != 0) {

        xengt_destroy_display_surface();
        
        xengt_create_display_surface(ds, gem_vgtbuffer, pf);

        if (ds->surface != NULL)
            dpy_resize(ds);
    }

    if (ds->surface != NULL) {
        drm_intel_bo_unmap(surface->bo);

        if (fb->bo)
            intel_blt_copy(gem_vgt_batchbuffer,
                           fb->bo, 0, 0, gem_vgtbuffer->stride,
                           surface->bo, 0, 0, ds_get_linesize(ds),
                           gem_vgtbuffer->width, 
                           gem_vgtbuffer->height, 
                           gem_vgtbuffer->bpp);

        drm_intel_bo_map(surface->bo, 1);
    }

    dpy_update(ds, 0, 0, gem_vgtbuffer->width, gem_vgtbuffer->height);
    return;
}

/*
 *  Inform vGT driver to create a vGT instance
 */
static void create_vgt_instance(void)
{
    /* FIXME: this should be substituded as a environment variable */
    const char *path = "/sys/kernel/vgt/control/create_vgt_instance";
    FILE *vgt_file;
    int err = 0;

    qemu_log("vGT: %s: domid=%d, low_gm_sz=%dMB, high_gm_sz=%dMB, "
             "fence_sz=%d, vga_sz=%dMB, vgt_primary=%d vgt_cap=%d\n",
             __func__, xen_domid, vgt_low_gm_sz, vgt_high_gm_sz,
             vgt_fence_sz, 8, vgt_primary, vgt_cap);
    if (vgt_low_gm_sz <= 0 || vgt_high_gm_sz <=0 ||
        vgt_cap < 0 || vgt_cap > 100 ||
        vgt_primary < -1 || vgt_primary > 1 ||
        vgt_fence_sz <=0) {
        qemu_log("vGT: %s failed: invalid parameters!\n", __func__);
        abort();
    }

    if ((vgt_file = fopen(path, "w")) == NULL) {
        err = errno;
        qemu_log("vGT: open %s failed\n", path);
    }
    /* The format of the string is:
     * domid,aperture_size,gm_size,fence_size. This means we want the vgt
     * driver to create a vgt instanc for Domain domid with the required
     * parameters. NOTE: aperture_size and gm_size are in MB.
     */
    if (!err && 
        fprintf(vgt_file, "%d,%u,%u,%u,%d,%d\n", xen_domid,
                vgt_low_gm_sz, vgt_high_gm_sz, vgt_fence_sz, vgt_primary,
                vgt_cap) < 0) {
        err = errno;
    }

    if (!err && fclose(vgt_file) != 0) {
        err = errno;
    }

    if (err) {
        qemu_log("vGT: %s failed: errno=%d\n", __func__, err);
        exit(-1);
    }

    config_vgt_guest_monitors();
    xengt_drm_init();
}

/*
 *  Inform vGT driver to close a vGT instance
 */
void destroy_vgt_instance(void)
{
    const char *path = "/sys/kernel/vgt/control/create_vgt_instance";
    FILE *vgt_file;
    int err = 0;

    qemu_log("vGT: %s: domid=%d\n", __func__, xen_domid);

    if ((vgt_file = fopen(path, "w")) == NULL) {
        qemu_log("vGT: open %s failed\n", path);
        err = errno;
    }

    /* -domid means we want the vgt driver to free the vgt instance
     * of Domain domid.
     * */
    if (!err && fprintf(vgt_file, "%d\n", -xen_domid) < 0) {
        err = errno;
    }

    if (!err && fclose(vgt_file) != 0) {
        err = errno;
    }

    if (err) {
        qemu_log("vGT: %s: failed: errno=%d\n", __func__, err);
        exit(-1);
    }
}

static int pch_map_irq(PCIDevice *pci_dev, int irq_num)
{
    return irq_num;
}

void vgt_bridge_pci_write(PCIDevice *dev, uint32_t addr, uint32_t val, int len)
{
#if 0
    vgt_vga_state_t *o = DO_UPCAST(vgt_vga_state_t, dev, dev);
#endif
    //assert(dev->devfn == 0x00);

    PT_LOG_DEV(dev, "vGT Config Write: addr=%x len=%x val=%x\n", addr, len, val);

    switch (addr) {
#if 0
        case 0x58:        // PAVPC Offset
            xen_host_pci_set_block(o->host_dev, addr, val, len);
            break;
#endif
	/*case 0xf8:
    		qemu_log("mapping VGA RAM at 0x%x\n",val);
            vga_map(pci_find_device(0,2,0), 0, val, vgt_legacy_vga_ram_size, 0);
            break;*/
        default:
            pci_default_write_config(dev, addr, val, len);
    }
}

static void vgt_bridge_pci_conf_init_from_host(PCIDevice *dev,
        uint32_t addr, int len)
{
    struct pci_dev *host_dev;

    if (len > 4) {
        PT_LOG_DEV(dev, "WARNIGN: length %x too large for config addr %x, ignore init\n",
                len, addr);
        return;
    }

    /* FIXME: need a better scheme to grab the root complex. This
     * only for a single VM scenario.
    */
    if ( !(host_dev = pt_pci_get_dev(0, 0, 0))) {
        qemu_log(" Error, failed to get host PCI device\n");
    }

    *((u32*)(dev->config + addr)) = pt_pci_host_read(host_dev, addr, len);
}

static void vgt_host_bridge_cap_init(PCIDevice *dev)
{
    assert(dev->devfn == 0x00);
    u32 cap_ptr = 0;
    struct pci_dev *host_dev;

    host_dev = pt_pci_get_dev(0, 0, 0);
    cap_ptr = pt_pci_host_read(host_dev, 0x34, 1);

    while (cap_ptr !=0) {
        vgt_bridge_pci_conf_init_from_host(dev, cap_ptr, 4); /* capability */
        vgt_bridge_pci_conf_init_from_host(dev, cap_ptr + 4, 4); /* capability */
        vgt_bridge_pci_conf_init_from_host(dev, cap_ptr + 8, 4); /* capability */
        vgt_bridge_pci_conf_init_from_host(dev, cap_ptr + 12, 4); /* capability */
        //PT_LOG_DEV(pci_dev, "Add vgt host bridge capability: offset=0x%x, cap=0x%x\n", cap_ptr,
        //    pt_pci_host_read(0, PCI_SLOT(pci_dev->devfn), 0, cap_ptr, 1) & 0xFF );
        cap_ptr = pt_pci_host_read(host_dev, cap_ptr +1, 1);
    }

}


static void vgt_vga_map(PCIDevice *pci_dev, int region_num,
                    uint32_t addr, uint32_t size, int type)
{
    vga_map(pci_find_device(0,2,0), region_num, addr, size, type);
}
void vgt_bridge_pci_conf_init(PCIDevice *pci_dev)
{
    qemu_log("vgt_bridge_pci_conf_init\n");
    qemu_log("vendor id: %x\n", *(uint16_t *)((char *)pci_dev->config + 0x00));
    vgt_bridge_pci_conf_init_from_host(pci_dev, 0x00, 2); /* vendor id */
    qemu_log("vendor id: %x\n", *(uint16_t *)((char *)pci_dev->config + 0x00));
    qemu_log("device id: %x\n", *(uint16_t *)((char *)pci_dev->config + 0x02));
    vgt_bridge_pci_conf_init_from_host(pci_dev, 0x02, 2); /* device id */
    qemu_log("device id: %x\n", *(uint16_t *)((char *)pci_dev->config + 0x02));
    vgt_bridge_pci_conf_init_from_host(pci_dev, 0x06, 2); /* status */
    vgt_bridge_pci_conf_init_from_host(pci_dev, 0x08, 2); /* revision id */
    vgt_bridge_pci_conf_init_from_host(pci_dev, 0x34, 1); /* capability */
    //vgt_host_bridge_cap_init(pci_dev);
    vgt_bridge_pci_conf_init_from_host(pci_dev, 0x50, 2); /* SNB: processor graphics control register */
    vgt_bridge_pci_conf_init_from_host(pci_dev, 0x52, 2); /* processor graphics control register */
    vgt_bridge_pci_conf_init_from_host(pci_dev, 0xa, 2); /* class code */

}

uint32_t vgt_bridge_pci_read(PCIDevice *pci_dev, uint32_t config_addr, int len)
{
    uint32_t val;

    val = pci_default_read_config(pci_dev, config_addr, len);

    return val;
}

/*static void vgt_cleanupfn(PCIDevice *dev)
{
    vgt_vga_state_t *d = DO_UPCAST(vgt_vga_state_t, dev, dev);

    if (d->instance_created) {
        destroy_vgt_instance();
    }
    }*/

static void vgt_cleanupfn2(void *unused)
{
    destroy_vgt_instance();
}

/*static int vgt_initfn(PCIDevice *dev)
{
    vgt_vga_state_t *d = DO_UPCAST(vgt_vga_state_t, dev, dev);

    qemu_log("vgt_initfn\n");
    d->instance_created = FALSE;

    create_vgt_instance();
    return 0;
}*/

void xengt_vga_init(PCIBus *pci_bus, ram_addr_t vga_ram_addr, int vga_ram_size)
{
    int ret;
    struct pci_dev *host_dev;
    uint16_t vid, did;
    uint8_t  rid;

    if (!(host_dev = pt_pci_get_dev(0, 0x1f, 0))) {
        qemu_log(" Error, failed to get host PCI device\n");
        return;
    }

    vid = pt_pci_host_read(host_dev, PCI_VENDOR_ID, 2);
    did = pt_pci_host_read(host_dev, PCI_DEVICE_ID, 2);
    rid = pt_pci_host_read(host_dev, PCI_REVISION, 1);
    if (vid != PCI_VENDOR_ID_INTEL) {
        qemu_log(" Error, vga-xengt is only supported on Intel GPUs\n");
        return;
    }

    pci_isa_bridge_init(pci_bus, PCI_DEVFN(0x1f, 0), vid,did,rid,
                                   pch_map_irq, "xengt-isa");
    pci_register_io_region(pci_find_device(0,0x1f,0), 0, vga_ram_size,
                           PCI_ADDRESS_SPACE_MEM_PREFETCH, vgt_vga_map);

    /* Note I have not set the class code of the bridge! */
    qemu_log("Create xengt ISA bridge successfully\n");
    ret = pci_vga_init(pci_bus, PCI_DEVFN(0x2,0),
			 phys_ram_base + vga_ram_addr,
                         vga_ram_addr, vga_ram_size, 0, 0);

    vgt_legacy_vga_ram_size = vga_ram_size;
    if (ret) {
        qemu_log("Warning: vga-xengt not available\n");
        return;
    }

    create_vgt_instance();
    qemu_register_exit(vgt_cleanupfn2, NULL);
    qemu_log("Create xengt VGA successfully\n");
    return;
}
/*
static void vgt_class_initfn(ObjectClass *klass, void *data)
{
    qemu_log("vgt_class_initfn\n");
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *ic = PCI_DEVICE_CLASS(klass);
    ic->init = vgt_initfn;
    dc->reset = vgt_reset;
    ic->exit = vgt_cleanupfn;
    dc->vmsd = &vmstate_vga_common;
}

static TypeInfo vgt_info = {
    .name          = "xengt-vga",
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(vgt_vga_state_t),
    .class_init    = vgt_class_initfn,
};

static TypeInfo isa_info = {
    .name          = "xengt-isa",
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(vgt_vga_state_t),
};

static void vgt_register_types(void)
{
    type_register_static(&vgt_info);
    type_register_static(&isa_info);
}

type_init(vgt_register_types)
*/
