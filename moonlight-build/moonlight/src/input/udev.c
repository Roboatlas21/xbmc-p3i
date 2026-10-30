/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015-2017 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#include "../loop.h"

#include "udev.h"
#include "evdev.h"
#include "selected_names.h"

#include <libudev.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/input.h>

static bool autoadd, debug;
static bool whitelist_active;
static char *selected_names;
static char *selected_selectors;
static bool selectors_v2_active;

static bool udev_allowed_name(const char *name) {
  return !whitelist_active ||
         ((selected_selectors || selected_names) &&
          (!selectors_v2_active || selected_selectors) &&
          moonlight_selected_identity(selected_selectors, selected_names,
                                      name, "", ""));
}

/* Query the same evdev device name that Luna displays in its picker.
 * Do not open an unselected hotplug node for write/input capture. */
static bool udev_allowed_node(const char *devnode) {
  if (!whitelist_active) return true;
  if (!devnode || (!selected_selectors && !selected_names) ||
      (selectors_v2_active && !selected_selectors))
    return false;
  int fd = open(devnode, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) return false;
  char name[256] = {0}, uniq[256] = {0}, phys[256] = {0};
  int length = ioctl(fd, EVIOCGNAME(sizeof(name)), name);
  if (length <= 0 || length >= (int)sizeof(name)) {
    close(fd);
    return false;
  }
  name[sizeof(name) - 1] = 0;
  if (!selectors_v2_active && !udev_allowed_name(name)) {
    close(fd);
    return false;
  }
  length = ioctl(fd, EVIOCGUNIQ(sizeof(uniq)), uniq);
  if (length <= 0 || length >= (int)sizeof(uniq)) uniq[0] = 0;
  length = ioctl(fd, EVIOCGPHYS(sizeof(phys)), phys);
  if (length <= 0 || length >= (int)sizeof(phys)) phys[0] = 0;
  uniq[sizeof(uniq) - 1] = 0;
  phys[sizeof(phys) - 1] = 0;
  close(fd);
  return moonlight_selected_identity(selected_selectors, selected_names,
                                     name, uniq, phys);
}
static struct mapping* defaultMappings;

static struct udev *udev;
static struct udev_monitor *udev_mon;
static int inputRotate;
static int monitor_fd = -1;

static int udev_handle(int fd) {
  (void)fd;
  if (!udev_mon) return LOOP_OK;
  struct udev_device *dev = udev_monitor_receive_device(udev_mon);
  if (!dev) return LOOP_OK; /* Nonblocking monitor can return EAGAIN. */
  const char *action = udev_device_get_action(dev);
  if ((autoadd || whitelist_active) && action &&
      strcmp("add", action) == 0) {
    const char *devnode = udev_device_get_devnode(dev);
    int id;
    char extra;
    if (devnode && sscanf(devnode, "/dev/input/event%d%c", &id, &extra) == 1 &&
        id >= 0 && udev_allowed_node(devnode))
      evdev_create(devnode, defaultMappings, debug, inputRotate);
  }
  udev_device_unref(dev); /* Also release devices with no action. */
  return LOOP_OK;
}

void udev_init(bool autoload, struct mapping* mappings, bool verbose, int rotate) {
  debug = verbose;
  autoadd = autoload;
  const char *requested = getenv("MOONLIGHT_INPUT_NAMES_HEX");
  const char *v2 = getenv("MOONLIGHT_INPUT_SELECTORS_V2");
  whitelist_active = requested != NULL || v2 != NULL;
  selectors_v2_active = v2 != NULL;
  free(selected_names);
  free(selected_selectors);
  selected_names = NULL;
  selected_selectors = NULL;
  if (requested && strlen(requested) <= 16384)
    selected_names = strdup(requested);
  if (v2 && strlen(v2) <= 16384)
    selected_selectors = strdup(v2);
  defaultMappings = mappings;
  inputRotate = rotate;
  udev = udev_new();
  if (!udev) {
    fprintf(stderr, "Input: cannot create udev context; hotplug unavailable\n");
    return;
  }

  /* Activate the monitor BEFORE scanning. Events arriving during the
   * enumeration stay in its queue; evdev_create deduplicates devices that
   * appear in both the snapshot and the queued notifications. */
  udev_mon = udev_monitor_new_from_netlink(udev, "udev");
  if (!udev_mon) {
    fprintf(stderr, "Input: udev monitor unavailable; hotplug disabled\n");
  } else {
    int rc = udev_monitor_filter_add_match_subsystem_devtype(udev_mon, "input", NULL);
    if (rc >= 0) rc = udev_monitor_enable_receiving(udev_mon);
    if (rc >= 0) monitor_fd = udev_monitor_get_fd(udev_mon);
    if (rc < 0 || monitor_fd < 0) {
      fprintf(stderr, "Input: unable to activate udev hotplug monitor (%d)\n",
              rc < 0 ? rc : monitor_fd);
      udev_monitor_unref(udev_mon);
      udev_mon = NULL;
      monitor_fd = -1;
    } else {
      loop_add_fd(monitor_fd, &udev_handle, POLLIN);
    }
  }

  /* Explicit selectors still enumerate already connected nodes, including
   * controllers whose eventN changed after Luna wrote its session snapshot. */
  if (autoload || whitelist_active) {
    struct udev_enumerate *enumerate = udev_enumerate_new(udev);
    if (!enumerate) {
      fprintf(stderr, "Input: cannot allocate udev enumeration\n");
    } else {
      int filter = udev_enumerate_add_match_subsystem(enumerate, "input");
      int scanned = filter < 0 ? filter : udev_enumerate_scan_devices(enumerate);
      if (scanned < 0) {
        fprintf(stderr, "Input: udev enumeration failed: %d\n", scanned);
      } else {
        struct udev_list_entry *entry;
        udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(enumerate)) {
          const char *path = udev_list_entry_get_name(entry);
          struct udev_device *dev = path ?
              udev_device_new_from_syspath(udev, path) : NULL;
          if (!dev) continue;
          const char *devnode = udev_device_get_devnode(dev);
          int id;
          char extra;
          if (devnode && sscanf(devnode, "/dev/input/event%d%c", &id, &extra) == 1 &&
              id >= 0 && udev_allowed_node(devnode))
            evdev_create(devnode, mappings, verbose, rotate);
          udev_device_unref(dev);
        }
      }
      udev_enumerate_unref(enumerate);
    }
  }
}

void udev_destroy() {
  if (monitor_fd >= 0) loop_remove_fd(monitor_fd);
  monitor_fd = -1;
  if (udev_mon) udev_monitor_unref(udev_mon);
  udev_mon = NULL;
  if (udev) udev_unref(udev);
  udev = NULL;
  free(selected_names);
  free(selected_selectors);
  selected_names = NULL;
  selected_selectors = NULL;
  selectors_v2_active = false;
  whitelist_active = false;
}
