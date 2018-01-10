QEMU_ROOT ?= .
include $(QEMU_ROOT)/Rules.mk

ifdef CONFIG_STUBDOM
export TARGET_DIRS=i386-stubdom
else
export TARGET_DIRS=i386-dm
endif

SUBDIR_RULES=subdir-$(TARGET_DIRS)
subdir-$(TARGET_DIRS): libqemu_common.a

include $(QEMU_ROOT)/xen-hooks.mak
