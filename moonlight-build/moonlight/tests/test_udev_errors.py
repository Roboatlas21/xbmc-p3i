"""Run production udev initialization with an injected libudev backend."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
UDEV_H = r'''
#pragma once
struct udev;
struct udev_monitor;
struct udev_enumerate;
struct udev_device;
struct udev_list_entry;
struct udev *udev_new(void);
void udev_unref(struct udev *);
struct udev_enumerate *udev_enumerate_new(struct udev *);
void udev_enumerate_unref(struct udev_enumerate *);
int udev_enumerate_add_match_subsystem(struct udev_enumerate *, const char *);
int udev_enumerate_scan_devices(struct udev_enumerate *);
struct udev_list_entry *udev_enumerate_get_list_entry(struct udev_enumerate *);
const char *udev_list_entry_get_name(struct udev_list_entry *);
#define udev_list_entry_foreach(entry, first) for ((entry)=(first); (entry); (entry)=0)
struct udev_device *udev_device_new_from_syspath(struct udev *, const char *);
const char *udev_device_get_devnode(struct udev_device *);
const char *udev_device_get_action(struct udev_device *);
void udev_device_unref(struct udev_device *);
struct udev_monitor *udev_monitor_new_from_netlink(struct udev *, const char *);
int udev_monitor_filter_add_match_subsystem_devtype(struct udev_monitor *, const char *, const char *);
int udev_monitor_enable_receiving(struct udev_monitor *);
int udev_monitor_get_fd(struct udev_monitor *);
struct udev_device *udev_monitor_receive_device(struct udev_monitor *);
void udev_monitor_unref(struct udev_monitor *);
'''
LOOP_H = '''#pragma once
#define LOOP_OK 0
typedef int (*FdHandler)(int);
void loop_add_fd(int fd, FdHandler handler, int events);
void loop_remove_fd(int fd);
'''
EVDEV_H = '''#pragma once
#include <stdbool.h>
struct mapping;
void evdev_create(const char *dev, struct mapping *m, bool verbose, int rotate);
'''
HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <stdbool.h>
#include <poll.h>
struct udev {int value;};
struct udev_monitor {int value;};
struct udev_enumerate {int value;};
struct udev_device {int value;};
struct udev_list_entry {int value;};
static struct udev ctx;
static struct udev_monitor monitor;
static struct udev_enumerate enumerator;
static struct udev_device device;
static int fail_stage, registered, removed, refs, monitor_refs, event_refs, enumerations, enumeration_refs, monitoring;
#include "input/udev.c"
struct udev *udev_new(void) { return fail_stage == 1 ? NULL : &ctx; }
void udev_unref(struct udev *p) { assert(p == &ctx); ++refs; }
struct udev_enumerate *udev_enumerate_new(struct udev *p) { (void)p; if (fail_stage == 0) assert(monitoring); ++enumerations; return &enumerator; }
void udev_enumerate_unref(struct udev_enumerate *p) { assert(p == &enumerator); ++enumeration_refs; }
int udev_enumerate_add_match_subsystem(struct udev_enumerate *p, const char *s) { (void)p; (void)s; return 0; }
int udev_enumerate_scan_devices(struct udev_enumerate *p) { (void)p; return 0; }
struct udev_list_entry *udev_enumerate_get_list_entry(struct udev_enumerate *p) { (void)p; return NULL; }
const char *udev_list_entry_get_name(struct udev_list_entry *p) { (void)p; return NULL; }
struct udev_device *udev_device_new_from_syspath(struct udev *p, const char *s) { (void)p; (void)s; return &device; }
const char *udev_device_get_devnode(struct udev_device *p) { (void)p; return NULL; }
const char *udev_device_get_action(struct udev_device *p) { (void)p; return NULL; }
void udev_device_unref(struct udev_device *p) { assert(p == &device); ++event_refs; }
struct udev_monitor *udev_monitor_new_from_netlink(struct udev *p, const char *s) {
  (void)p; (void)s; return fail_stage == 2 ? NULL : &monitor;
}
int udev_monitor_filter_add_match_subsystem_devtype(struct udev_monitor *p, const char *s, const char *type) {
  (void)p; (void)s; (void)type; return fail_stage == 3 ? -5 : 0;
}
int udev_monitor_enable_receiving(struct udev_monitor *p) {
  (void)p; if (fail_stage == 4) return -6; monitoring = 1; return 0;
}
int udev_monitor_get_fd(struct udev_monitor *p) {
  (void)p; return fail_stage == 5 ? -1 : 21;
}
struct udev_device *udev_monitor_receive_device(struct udev_monitor *p) {
  (void)p; return fail_stage == 7 ? NULL : &device;
}
void udev_monitor_unref(struct udev_monitor *p) { assert(p == &monitor); ++monitor_refs; }
void loop_add_fd(int fd, FdHandler callback, int events) {
  assert(fd == 21 && callback && events == POLLIN); ++registered;
}
void loop_remove_fd(int fd) { assert(fd == 21); ++removed; }
void evdev_create(const char *dev, struct mapping *m, bool verbose, int rotate) {
  (void)dev; (void)m; (void)verbose; (void)rotate; abort();
}
int main(void) {
  unsetenv("MOONLIGHT_INPUT_NAMES_HEX");
  for (int stage = 1; stage <= 5; ++stage) {
    fail_stage = stage;
    registered = removed = refs = monitor_refs = event_refs = enumerations = enumeration_refs = monitoring = 0;
    udev_init(false, NULL, false, 0);
    assert(registered == 0);
    udev_destroy();
    assert(removed == 0);
    assert(enumerations == 0 && enumeration_refs == 0);
    if (stage != 1) assert(refs == 1);
    if (stage >= 3) assert(monitor_refs == 1);
  }
  fail_stage = 0;
  registered = removed = event_refs = 0;
  enumerations = enumeration_refs = monitoring = 0;
  udev_init(false, NULL, false, 0);
  assert(enumerations == 0 && enumeration_refs == 0);
  assert(registered == 1 && monitor_fd == 21);
  assert(udev_handle(21) == LOOP_OK && event_refs == 1);
  fail_stage = 7;
  assert(udev_handle(21) == LOOP_OK && event_refs == 1);
  udev_destroy();
  udev_destroy();
  assert(removed == 1);

  /* An explicit selection enables hotplug monitoring but never admits
   * an unrelated device name. The normal automatic mode stays unchanged. */
  setenv("MOONLIGHT_INPUT_NAMES_HEX", "58626f7820506164", 1);
  fail_stage = 0;
  registered = removed = enumerations = enumeration_refs = monitoring = 0;
  udev_init(false, NULL, false, 0);
  assert(enumerations == 1 && enumeration_refs == 1 && monitoring == 1); /* Whitelisted startup sweep, not only add events. */
  assert(registered == 1 && whitelist_active);
  assert(udev_allowed_name("Xbox Pad"));
  assert(!udev_allowed_name("Unselected Keyboard"));
  udev_destroy();
  unsetenv("MOONLIGHT_INPUT_NAMES_HEX");
  assert(removed == 1);

  setenv("MOONLIGHT_INPUT_SELECTORS_V2", "58626f7820506164:N:", 1);
  registered = removed = enumerations = enumeration_refs = monitoring = 0;
  udev_init(false, NULL, false, 0);
  assert(enumerations == 1 && enumeration_refs == 1 && monitoring == 1 && whitelist_active);
  assert(udev_allowed_name("Xbox Pad"));
  assert(!udev_allowed_name("Keyboard"));
  udev_destroy();
  unsetenv("MOONLIGHT_INPUT_SELECTORS_V2");
  return 0;
}
'''


class UdevErrorTests(unittest.TestCase):
    def test_lifecycle_on_device_monitor_failures(self):
        if not shutil.which('cc'):
            self.skipTest('C compiler missing')
        with tempfile.TemporaryDirectory() as td:
            root = Path(td) / 'src'
            (root / 'input').mkdir(parents=True)
            (root / 'libudev.h').write_text(UDEV_H)
            (root / 'loop.h').write_text(LOOP_H)
            (root / 'input/udev.h').write_text('#pragma once\n#include <stdbool.h>\nstruct mapping;\n')
            (root / 'input/evdev.h').write_text(EVDEV_H)
            shutil.copyfile(ROOT / 'src/input/udev.c', root / 'input/udev.c')
            shutil.copyfile(ROOT / 'src/input/selected_names.h',
                            root / 'input/selected_names.h')
            (root / 'harness.c').write_text(
                '#include <stdlib.h>\n' + HARNESS)
            result = subprocess.run(
                ['cc', '-std=gnu99', '-Wall', '-Wextra', '-Werror',
                 '-I', str(root), str(root / 'harness.c'), '-o', str(root / 'test')],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(root / 'test')], check=True, capture_output=True)


if __name__ == '__main__':
    unittest.main()
