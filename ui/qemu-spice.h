/*
 * Copyright (C) 2010 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 or
 * (at your option) version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef QEMU_SPICE_H
#define QEMU_SPICE_H

#ifdef CONFIG_SPICE

#include <spice.h>

#include "qemu-option.h"
#include "qemu-config.h"

extern int using_spice;

extern bool spice_used; // spice option is set in xm conf file.
extern int spice_port;
extern int spice_tls_port;
extern char *psh;

extern char *pspwd;
extern int spice_disable_ticketing;

extern char *psic;
extern char *pssv;
extern char *psjwc;
extern char *pszgwc;
extern int spiceplayback;

extern int spiceagent_mouse;

extern int qxl_num;
extern int qxl_ram;

void qemu_spice_init(void);
void qemu_spice_input_init(void);
void qemu_spice_audio_init(void);
void qemu_spice_display_init(DisplayState *ds);
int qemu_spice_add_interface(SpiceBaseInstance *sin);

int mon_spice_migrate(Monitor *mon, const QDict *qdict, QObject **ret_data);

#else  /* CONFIG_SPICE */

#define using_spice 0

#endif /* CONFIG_SPICE */

#endif /* QEMU_SPICE_H */
