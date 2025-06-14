/*
 * compose persistent device path
 *
 * Copyright (C) 2009-2011 Kay Sievers <kay@vrfy.org>
 *
 * Logic based on Hannes Reinecke's shell script.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <getopt.h>

#include "udev.h"

_printf_(2,3)
static int path_prepend(char **path, const char *fmt, ...) {
        va_list va;
        char *pre;
        int err = 0;

        va_start(va, fmt);
        err = vasprintf(&pre, fmt, va);
        va_end(va);
        if (err < 0)
                goto out;

        if (*path != NULL) {
                char *new;

                err = asprintf(&new, "%s-%s", pre, *path);
                free(pre);
                if (err < 0)
                        goto out;
                free(*path);
                *path = new;
        } else {
                *path = pre;
        }
out:
        return err;
}

/*
** Linux only supports 32 bit luns.
** See drivers/scsi/scsi_scan.c::scsilun_to_int() for more details.
*/
static int format_lun_number(struct udev_device *dev, char **path) {
        unsigned long lun = strtoul(udev_device_get_sysnum(dev), NULL, 10);

        /* address method 0, peripheral device addressing with bus id of zero */
        if (lun < 256)
                return path_prepend(path, "lun-%lu", lun);
        /* handle all other lun addressing methods by using a variant of the original lun format */
        return path_prepend(path, "lun-0x%04lx%04lx00000000", lun & 0xffff, (lun >> 16) & 0xffff);
}

static struct udev_device *skip_subsystem(struct udev_device *dev, const char *subsys) {
        struct udev_device *parent = dev;

        while (parent != NULL) {
                const char *subsystem;

                subsystem = udev_device_get_subsystem(parent);
                if (subsystem == NULL || !streq(subsystem, subsys))
                        break;
                dev = parent;
                parent = udev_device_get_parent(parent);
        }
        return dev;
}

static struct udev_device *handle_scsi_fibre_channel(struct udev_device *parent, char **path) {
        struct udev *udev  = udev_device_get_udev(parent);
        struct udev_device *targetdev;
        struct udev_device *fcdev = NULL;
        const char *port;
        char *lun = NULL;

        targetdev = udev_device_get_parent_with_subsystem_devtype(parent, "scsi", "scsi_target");
        if (targetdev == NULL)
                return NULL;

        fcdev = udev_device_new_from_subsystem_sysname(udev, "fc_transport", udev_device_get_sysname(targetdev));
        if (fcdev == NULL)
                return NULL;
        port = udev_device_get_sysattr_value(fcdev, "port_name");
        if (port == NULL) {
                parent = NULL;
                goto out;
        }

        format_lun_number(parent, &lun);
        path_prepend(path, "fc-%s-%s", port, lun);
        if (lun)
                free(lun);
out:
        udev_device_unref(fcdev);
        return parent;
}

static struct udev_device *handle_scsi_sas_wide_port(struct udev_device *parent, char **path) {
        struct udev *udev  = udev_device_get_udev(parent);
        struct udev_device *targetdev;
        struct udev_device *target_parent;
        struct udev_device *sasdev;
        const char *sas_address;
        char *lun = NULL;

        targetdev = udev_device_get_parent_with_subsystem_devtype(parent, "scsi", "scsi_target");
        if (targetdev == NULL)
                return NULL;

        target_parent = udev_device_get_parent(targetdev);
        if (target_parent == NULL)
                return NULL;

        sasdev = udev_device_new_from_subsystem_sysname(udev, "sas_device",
                                udev_device_get_sysname(target_parent));
        if (sasdev == NULL)
                return NULL;

        sas_address = udev_device_get_sysattr_value(sasdev, "sas_address");
        if (sas_address == NULL) {
                parent = NULL;
                goto out;
        }

        format_lun_number(parent, &lun);
        path_prepend(path, "sas-%s-%s", sas_address, lun);
        if (lun)
                free(lun);
out:
        udev_device_unref(sasdev);
        return parent;
}

static struct udev_device *handle_scsi_sas(struct udev_device *parent, char **path)
{
        struct udev *udev  = udev_device_get_udev(parent);
        struct udev_device *targetdev;
        struct udev_device *target_parent;
        struct udev_device *port;
        struct udev_device *expander;
        struct udev_device *target_sasdev = NULL;
        struct udev_device *expander_sasdev = NULL;
        struct udev_device *port_sasdev = NULL;
        const char *sas_address = NULL;
        const char *phy_id;
        const char *phy_count;
        char *lun = NULL;

        targetdev = udev_device_get_parent_with_subsystem_devtype(parent, "scsi", "scsi_target");
        if (targetdev == NULL)
                return NULL;

        target_parent = udev_device_get_parent(targetdev);
        if (target_parent == NULL)
                return NULL;

        /* Get sas device */
        target_sasdev = udev_device_new_from_subsystem_sysname(udev,
                          "sas_device", udev_device_get_sysname(target_parent));
        if (target_sasdev == NULL)
                return NULL;

        /* The next parent is sas port */
        port = udev_device_get_parent(target_parent);
        if (port == NULL) {
                parent = NULL;
                goto out;
        }

        /* Get port device */
        port_sasdev = udev_device_new_from_subsystem_sysname(udev,
                          "sas_port", udev_device_get_sysname(port));

        phy_count = udev_device_get_sysattr_value(port_sasdev, "num_phys");
        if (phy_count == NULL) {
               parent = NULL;
               goto out;
        }

        /* Check if we are simple disk */
        if (strncmp(phy_count, "1", 2) != 0) {
                 parent = handle_scsi_sas_wide_port(parent, path);
                 goto out;
        }

        /* Get connected phy */
        phy_id = udev_device_get_sysattr_value(target_sasdev, "phy_identifier");
        if (phy_id == NULL) {
                parent = NULL;
                goto out;
        }

        /* The port's parent is either hba or expander */
        expander = udev_device_get_parent(port);
        if (expander == NULL) {
                parent = NULL;
                goto out;
        }

        /* Get expander device */
        expander_sasdev = udev_device_new_from_subsystem_sysname(udev,
                          "sas_device", udev_device_get_sysname(expander));
        if (expander_sasdev != NULL) {
                 /* Get expander's address */
                 sas_address = udev_device_get_sysattr_value(expander_sasdev,
                                                    "sas_address");
                 if (sas_address == NULL) {
                        parent = NULL;
                        goto out;
                 }
        }

        format_lun_number(parent, &lun);
        if (sas_address)
                 path_prepend(path, "sas-exp%s-phy%s-%s", sas_address, phy_id, lun);
        else
                 path_prepend(path, "sas-phy%s-%s", phy_id, lun);

        if (lun)
                free(lun);
out:
        udev_device_unref(target_sasdev);
        udev_device_unref(expander_sasdev);
        udev_device_unref(port_sasdev);
        return parent;
}

static struct udev_device *handle_scsi_iscsi(struct udev_device *parent, char **path) {
        struct udev *udev  = udev_device_get_udev(parent);
        struct udev_device *transportdev;
        struct udev_device *sessiondev = NULL;
        const char *target;
        char *connname;
        struct udev_device *conndev = NULL;
        const char *addr;
        const char *port;
        char *lun = NULL;

        /* find iscsi session */
        transportdev = parent;
        for (;;) {
                transportdev = udev_device_get_parent(transportdev);
                if (transportdev == NULL)
                        return NULL;
                if (startswith(udev_device_get_sysname(transportdev), "session"))
                        break;
        }

        /* find iscsi session device */
        sessiondev = udev_device_new_from_subsystem_sysname(udev, "iscsi_session", udev_device_get_sysname(transportdev));
        if (sessiondev == NULL)
                return NULL;
        target = udev_device_get_sysattr_value(sessiondev, "targetname");
        if (target == NULL) {
                parent = NULL;
                goto out;
        }

        if (asprintf(&connname, "connection%s:0", udev_device_get_sysnum(transportdev)) < 0) {
                parent = NULL;
                goto out;
        }
        conndev = udev_device_new_from_subsystem_sysname(udev, "iscsi_connection", connname);
        free(connname);
        if (conndev == NULL) {
                parent = NULL;
                goto out;
        }
        addr = udev_device_get_sysattr_value(conndev, "persistent_address");
        port = udev_device_get_sysattr_value(conndev, "persistent_port");
        if (addr == NULL || port == NULL) {
                parent = NULL;
                goto out;
        }

        format_lun_number(parent, &lun);
        path_prepend(path, "ip-%s:%s-iscsi-%s-%s", addr, port, target, lun);
        if (lun)
                free(lun);
out:
        udev_device_unref(sessiondev);
        udev_device_unref(conndev);
        return parent;
}

static struct udev_device *handle_scsi_default(struct udev_device *parent, char **path) {
        struct udev_device *hostdev;
        int host, bus, target, lun;
        const char *name;
        char *base;
        char *pos;
        DIR *dir;
        struct dirent *dent;
        int basenum;

        hostdev = udev_device_get_parent_with_subsystem_devtype(parent, "scsi", "scsi_host");
        if (hostdev == NULL)
                return NULL;

        name = udev_device_get_sysname(parent);
        if (sscanf(name, "%d:%d:%d:%d", &host, &bus, &target, &lun) != 4)
                return NULL;

        /*
         * Rebase host offset to get the local relative number
         *
         * Note: This is by definition racy, unreliable and too simple.
         * Please do not copy this model anywhere. It's just a left-over
         * from the time we had no idea how things should look like in
         * the end.
         *
         * Making assumptions about a global in-kernel counter and use
         * that to calculate a local offset is a very broken concept. It
         * can only work as long as things are in strict order.
         *
         * The kernel needs to export the instance/port number of a
         * controller directly, without the need for rebase magic like
         * this. Manual driver unbind/bind, parallel hotplug/unplug will
         * get into the way of this "I hope it works" logic.
         */
        basenum = -1;
        base = strdup(udev_device_get_syspath(hostdev));
        if (base == NULL)
                return NULL;
        pos = strrchr(base, '/');
        if (pos == NULL) {
                parent = NULL;
                goto out;
        }
        pos[0] = '\0';
        dir = opendir(base);
        if (dir == NULL) {
                parent = NULL;
                goto out;
        }
        for (dent = readdir(dir); dent != NULL; dent = readdir(dir)) {
                char *rest;
                int i;

                if (dent->d_name[0] == '.')
                        continue;
                if (dent->d_type != DT_DIR && dent->d_type != DT_LNK)
                        continue;
                if (!startswith(dent->d_name, "host"))
                        continue;
                i = strtoul(&dent->d_name[4], &rest, 10);
                if (rest[0] != '\0')
                        continue;
                /*
                 * find the smallest number; the host really needs to export its
                 * own instance number per parent device; relying on the global host
                 * enumeration and plainly rebasing the numbers sounds unreliable
                 */
                if (basenum == -1 || i < basenum)
                        basenum = i;
        }
        closedir(dir);
        if (basenum == -1) {
                parent = NULL;
                goto out;
        }
        host -= basenum;

        path_prepend(path, "scsi-%u:%u:%u:%u", host, bus, target, lun);
out:
        free(base);
        return hostdev;
}

static struct udev_device *handle_scsi_hyperv(struct udev_device *parent, char **path) {
        struct udev_device *hostdev;
        struct udev_device *vmbusdev;
        const char *guid_str;
        char *lun = NULL;
        char guid[38];
        size_t i, k;

        hostdev = udev_device_get_parent_with_subsystem_devtype(parent, "scsi", "scsi_host");
        if (!hostdev)
                return NULL;

        vmbusdev = udev_device_get_parent(hostdev);
        if (!vmbusdev)
                return NULL;

        guid_str = udev_device_get_sysattr_value(vmbusdev, "device_id");
        if (!guid_str)
                return NULL;

        if (strlen(guid_str) < 37 || guid_str[0] != '{' || guid_str[36] != '}')
                return NULL;

        for (i = 1, k = 0; i < 36; i++) {
                if (guid_str[i] == '-')
                        continue;
                guid[k++] = guid_str[i];
        }
        guid[k] = '\0';

        format_lun_number(parent, &lun);
        path_prepend(path, "vmbus-%s-%s", guid, lun);
        free(lun);
        return parent;
}

static struct udev_device *handle_scsi(struct udev_device *parent, char **path, bool *supported_parent) {
        const char *devtype;
        const char *name;
        const char *id;

        devtype = udev_device_get_devtype(parent);
        if (devtype == NULL || !streq(devtype, "scsi_device"))
                return parent;

        /* firewire */
        id = udev_device_get_sysattr_value(parent, "ieee1394_id");
        if (id != NULL) {
                parent = skip_subsystem(parent, "scsi");
                path_prepend(path, "ieee1394-0x%s", id);
                *supported_parent = true;
                goto out;
        }

        /* scsi sysfs does not have a "subsystem" for the transport */
        name = udev_device_get_syspath(parent);

        if (strstr(name, "/rport-") != NULL) {
                parent = handle_scsi_fibre_channel(parent, path);
                *supported_parent = true;
                goto out;
        }

        if (strstr(name, "/end_device-") != NULL) {
                parent = handle_scsi_sas(parent, path);
                *supported_parent = true;
                goto out;
        }

        if (strstr(name, "/session") != NULL) {
                parent = handle_scsi_iscsi(parent, path);
                *supported_parent = true;
                goto out;
        }

        /*
         * We do not support the ATA transport class, it uses global counters
         * to name the ata devices which numbers spread across multiple
         * controllers.
         *
         * The real link numbers are not exported. Also, possible chains of ports
         * behind port multipliers cannot be composed that way.
         *
         * Until all that is solved at the kernel level, there are no by-path/
         * links for ATA devices.
         */
        if (strstr(name, "/ata") != NULL) {
                parent = NULL;
                goto out;
        }

        if (strstr(name, "/vmbus_") != NULL) {
                parent = handle_scsi_hyperv(parent, path);
                goto out;
        }

        parent = handle_scsi_default(parent, path);
out:
        return parent;
}

static struct udev_device *handle_cciss(struct udev_device *parent, char **path) {
        const char *str;
        unsigned int controller, disk;

        str = udev_device_get_sysname(parent);
        if (sscanf(str, "c%ud%u%*s", &controller, &disk) != 2)
                return NULL;

        path_prepend(path, "cciss-disk%u", disk);
        parent = skip_subsystem(parent, "cciss");
        return parent;
}

static void handle_scsi_tape(struct udev_device *dev, char **path) {
        const char *name;

        /* must be the last device in the syspath */
        if (*path != NULL)
                return;

        name = udev_device_get_sysname(dev);
        if (startswith(name, "nst") && strchr("lma", name[3]) != NULL)
                path_prepend(path, "nst%c", name[3]);
        else if (startswith(name, "st") && strchr("lma", name[2]) != NULL)
                path_prepend(path, "st%c", name[2]);
}

static struct udev_device *handle_usb(struct udev_device *parent, char **path) {
        const char *devtype;
        const char *str;
        const char *port;

        devtype = udev_device_get_devtype(parent);
        if (devtype == NULL)
                return parent;
        if (!streq(devtype, "usb_interface") && !streq(devtype, "usb_device"))
                return parent;

        str = udev_device_get_sysname(parent);
        port = strchr(str, '-');
        if (port == NULL)
                return parent;
        port++;

        parent = skip_subsystem(parent, "usb");
        path_prepend(path, "usb-0:%s", port);
        return parent;
}

static struct udev_device *handle_bcma(struct udev_device *parent, char **path) {
        const char *sysname;
        unsigned int core;

        sysname = udev_device_get_sysname(parent);
        if (sscanf(sysname, "bcma%*u:%u", &core) != 1)
                return NULL;

        path_prepend(path, "bcma-%u", core);
        return parent;
}

static struct udev_device *handle_ccw(struct udev_device *parent, struct udev_device *dev, char **path) {
        struct udev_device *scsi_dev;

        scsi_dev = udev_device_get_parent_with_subsystem_devtype(dev, "scsi", "scsi_device");
        if (scsi_dev != NULL) {
                const char *wwpn;
                const char *lun;
                const char *hba_id;

                hba_id = udev_device_get_sysattr_value(scsi_dev, "hba_id");
                wwpn = udev_device_get_sysattr_value(scsi_dev, "wwpn");
                lun = udev_device_get_sysattr_value(scsi_dev, "fcp_lun");
                if (hba_id != NULL && lun != NULL && wwpn != NULL) {
                        path_prepend(path, "ccw-%s-zfcp-%s:%s", hba_id, wwpn, lun);
                        goto out;
                }
        }

        path_prepend(path, "ccw-%s", udev_device_get_sysname(parent));
out:
        parent = skip_subsystem(parent, "ccw");
        return parent;
}

static int find_real_nvme_parent(struct udev_device *dev, struct udev_device **ret) {
        _cleanup_free_ struct udev_device *nvme = NULL;
        const char *sysname, *end;
        struct udev *udev = udev_device_get_udev(dev);

        /* If the device belongs to "nvme-subsystem" (not to be confused with "nvme"), which happens when
         * NVMe multipathing is enabled in the kernel (/sys/module/nvme_core/parameters/multipath is Y),
         * then the syspath is something like the following:
         *   /sys/devices/virtual/nvme-subsystem/nvme-subsys0/nvme0n1
         * Hence, we need to find the 'real parent' in "nvme" subsystem, e.g,
         *   /sys/devices/pci0000:00/0000:00:1c.4/0000:3c:00.0/nvme/nvme0 */

        assert(dev);
        assert(ret);

        sysname = udev_device_get_sysname(dev);
        if (sysname  ==  NULL)
                return -1;

        /* The sysname format of nvme block device is nvme%d[c%d]n%d[p%d], e.g. nvme0n1p2 or nvme0c1n2.
         * (Note, nvme device with 'c' can be ignored, as they are hidden. )
         * The sysname format of nvme subsystem device is nvme%d.
         * See nvme_alloc_ns() and nvme_init_ctrl() in drivers/nvme/host/core.c for more details. */
        end = startswith(sysname, "nvme");
        if (!end)
                return -ENXIO;

        end += strspn(end, "0123456789");
        sysname = strndupa(sysname, end - sysname);

        nvme = udev_device_new_from_subsystem_sysname(udev, "nvme", sysname);
        if (nvme == NULL)
                return -1;

        *ret = TAKE_PTR(nvme);
        return 0;
}

static int builtin_path_id(struct udev_device *dev, int argc __attribute__((unused)), char *argv[] __attribute__((unused)), bool test) {
        struct udev_device *parent;
        char *path = NULL;
        bool supported_transport = false;
        bool supported_parent = false;
        _cleanup_free_ struct udev_device *dev_other_branch = NULL;

        /* S390 ccw bus */
        parent = udev_device_get_parent_with_subsystem_devtype(dev, "ccw", NULL);
        if (parent != NULL) {
                handle_ccw(parent, dev, &path);
                goto out;
        }

        /* walk up the chain of devices and compose path */
        parent = dev;
        while (parent != NULL) {
                const char *subsys;

                subsys = udev_device_get_subsystem(parent);
                if (subsys == NULL) {
                        ;
                } else if (streq(subsys, "scsi_tape")) {
                        handle_scsi_tape(parent, &path);
                } else if (streq(subsys, "scsi")) {
                        parent = handle_scsi(parent, &path, &supported_parent);
                        supported_transport = true;
                } else if (streq(subsys, "cciss")) {
                        parent = handle_cciss(parent, &path);
                        supported_transport = true;
                } else if (streq(subsys, "usb")) {
                        parent = handle_usb(parent, &path);
                        supported_transport = true;
                } else if (streq(subsys, "bcma")) {
                        parent = handle_bcma(parent, &path);
                        supported_transport = true;
                } else if (streq(subsys, "serio")) {
                        path_prepend(&path, "serio-%s", udev_device_get_sysnum(parent));
                        parent = skip_subsystem(parent, "serio");
                } else if (streq(subsys, "pci")) {
                        path_prepend(&path, "pci-%s", udev_device_get_sysname(parent));
                        parent = skip_subsystem(parent, "pci");
                        supported_parent = true;
                } else if (streq(subsys, "platform")) {
                        path_prepend(&path, "platform-%s", udev_device_get_sysname(parent));
                        parent = skip_subsystem(parent, "platform");
                        supported_transport = true;
                        supported_parent = true;
                } else if (streq(subsys, "acpi")) {
                        path_prepend(&path, "acpi-%s", udev_device_get_sysname(parent));
                        parent = skip_subsystem(parent, "acpi");
                        supported_parent = true;
                } else if (streq(subsys, "xen")) {
                        path_prepend(&path, "xen-%s", udev_device_get_sysname(parent));
                        parent = skip_subsystem(parent, "xen");
                        supported_parent = true;
                } else if (streq(subsys, "scm")) {
                        path_prepend(&path, "scm-%s", udev_device_get_sysname(parent));
                        parent = skip_subsystem(parent, "scm");
                        supported_transport = true;
                        supported_parent = true;
                } else if (streq(subsys, "nvme") || streq(subsys, "nvme-subsystem")) {
                        const char *nsid;
                        int r;
                        
                        nsid = udev_device_get_sysattr_value(dev, "nsid");
                        if (nsid != NULL) {
                                path_prepend(&path, "nvme-%s", nsid);
                                if (compat_path)
                                        path_prepend(&path, "nvme-%s", nsid);

                                if (streq(subsys, "nvme-subsystem")) {
                                        r = find_real_nvme_parent(dev, &dev_other_branch);
                                        if (r < 0)
                                                return r;

                                        parent = dev_other_branch;
                                }

                                parent = skip_subsystem(parent, "nvme");
                                supported_parent = true;
                                supported_transport = true;
                        }

                parent = udev_device_get_parent(parent);
        }

        /*
         * Do not return devices with an unknown parent device type. They
         * might produce conflicting IDs if the parent does not provide a
         * unique and predictable name.
         */
        if (!supported_parent) {
                free(path);
                path = NULL;
        }

        /*
         * Do not return block devices without a well-known transport. Some
         * devices do not expose their buses and do not provide a unique
         * and predictable name that way.
         */
        if (streq(udev_device_get_subsystem(dev), "block") && !supported_transport) {
                free(path);
                path = NULL;
        }

out:
        if (path != NULL) {
                char tag[UTIL_NAME_SIZE];
                size_t i;
                const char *p;

                /* compose valid udev tag name */
                for (p = path, i = 0; *p; p++) {
                        if ((*p >= '0' && *p <= '9') ||
                            (*p >= 'A' && *p <= 'Z') ||
                            (*p >= 'a' && *p <= 'z') ||
                            *p == '-') {
                                tag[i++] = *p;
                                continue;
                        }

                        /* skip all leading '_' */
                        if (i == 0)
                                continue;

                        /* avoid second '_' */
                        if (tag[i-1] == '_')
                                continue;

                        tag[i++] = '_';
                }
                /* strip trailing '_' */
                while (i > 0 && tag[i-1] == '_')
                        i--;
                tag[i] = '\0';

                udev_builtin_add_property(dev, test, "ID_PATH", path);
                udev_builtin_add_property(dev, test, "ID_PATH_TAG", tag);
                free(path);
                return EXIT_SUCCESS;
        }
        return EXIT_FAILURE;
}

const struct udev_builtin udev_builtin_path_id = {
        .name = "path_id",
        .cmd = builtin_path_id,
        .help = "Compose persistent device path",
        .run_once = true,
};
