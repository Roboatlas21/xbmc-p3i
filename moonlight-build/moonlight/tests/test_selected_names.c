/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/input/selected_names.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
  const char *names = "58626f7820576972656c65737320436f6e74726f6c6c6572,426c7565746f6f746820506164";
  assert(moonlight_selected_input(names, "Xbox Wireless Controller"));
  assert(moonlight_selected_input(names, "Bluetooth Pad"));
  assert(!moonlight_selected_input(names, "Keyboard"));
  assert(!moonlight_selected_input(names, "Xbox Wireless Controller 2"));
  assert(!moonlight_selected_input("", "Bluetooth Pad"));
  assert(!moonlight_selected_input("zz", "Bluetooth Pad"));
  assert(!moonlight_selected_input("4241x4", "BAD"));
  assert(!moonlight_selected_input("42:4f", "BO"));
  assert(moonlight_selected_input("4e616d652c506164", "Name,Pad"));
  assert(!moonlight_selected_input("4e616d652c506164", "Name"));
  /* Two controllers with the same model name have distinct identities. */
  const char *unique = "58626f7820506164:U:42542d41";
  assert(moonlight_selected_identity(unique, NULL, "Xbox Pad", "BT-A", ""));
  assert(!moonlight_selected_identity(unique, NULL, "Xbox Pad", "BT-B", ""));
  assert(!moonlight_selected_identity(unique, names, "Keyboard", "BT-A", ""));
  const char *physical = "58626f7820506164:P:7573622d312f696e70757430";
  assert(moonlight_selected_identity(physical, NULL, "Xbox Pad", "",
                                     "usb-1/input0"));
  assert(!moonlight_selected_identity(physical, NULL, "Xbox Pad", "",
                                      "usb-2/input0"));
  assert(moonlight_selected_identity("58626f7820506164:N:", NULL,
                                     "Xbox Pad", "", ""));
  assert(!moonlight_selected_identity("58626f7820506164:U:", NULL,
                                      "Xbox Pad", "", ""));
  assert(moonlight_selected_identity(NULL, names,
                                     "Xbox Wireless Controller", "", ""));
  assert(!moonlight_selected_identity("invalid", names,
                                      "Xbox Wireless Controller", "", ""));
  /* A generic serial alone is not enough when a second same-model device
   * appears later. A composite U/Phys selection must bind both values. */
  const char *specific = "58626f7820506164:U:30303030:7573622d312f696e70757430";
  assert(moonlight_selected_identity(specific, NULL,
                                     "Xbox Pad", "0000", "usb-1/input0"));
  assert(!moonlight_selected_identity(specific, NULL,
                                      "Xbox Pad", "0000", "usb-2/input0"));
  assert(!moonlight_selected_identity(specific, NULL,
                                      "Xbox Pad", "1111", "usb-1/input0"));
  assert(!moonlight_selected_identity(specific, NULL,
                                      "Xbox Pad", "0000", ""));
  assert(!moonlight_selected_identity("58626f7820506164:N::00", NULL,
                                      "Xbox Pad", "", ""));
  /* Previously stored three-part unique selectors remain readable. */
  assert(moonlight_selected_identity("58626f7820506164:U:30303030", NULL,
                                     "Xbox Pad", "0000", "usb-2/input0"));
  /* Strong unique identities should survive physical USB port changes.
   * Generic IDs must instead be encoded as a port (P) selector by Luna. */
  assert(moonlight_selected_identity("58626f7820506164:U:414231322d58593938",
                                     NULL, "Xbox Pad", "AB12-XY98",
                                     "usb-1/input0"));
  assert(moonlight_selected_identity("58626f7820506164:U:414231322d58593938",
                                     NULL, "Xbox Pad", "AB12-XY98",
                                     "usb-2/input0"));
  assert(!moonlight_selected_identity(
      "58626f7820506164:P:7573622d312f696e70757430",
      NULL, "Xbox Pad", "0000", "usb-2/input0"));
  puts("PASS: exact selected input names");
  return 0;
}
