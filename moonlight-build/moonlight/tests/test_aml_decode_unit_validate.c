#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
typedef struct ENTRY {
  struct ENTRY *next;
  char *data;
  int length;
} ENTRY, *PLENTRY;
typedef struct UNIT {
  PLENTRY bufferList;
  int fullLength;
} UNIT, *PDECODE_UNIT;
#include "../src/video/aml_decode_unit_validate.h"
int main(void) {
  char bytes[16] = {0};
  ENTRY tail = {.next=NULL, .data=bytes, .length=3};
  ENTRY head = {.next=&tail, .data=bytes, .length=4};
  UNIT u = {.bufferList=&head, .fullLength=7};
  assert(aml_decode_unit_valid(&u));
  u.fullLength=4;
  assert(!aml_decode_unit_valid(&u)); /* previously copied 7 into 4 */
  u.fullLength=10;
  assert(!aml_decode_unit_valid(&u)); /* truncated chain */
  u.fullLength=7; tail.length=0;
  assert(!aml_decode_unit_valid(&u));
  tail.length=3; head.data=NULL;
  assert(!aml_decode_unit_valid(&u));
  head.data=bytes; u.fullLength=0;
  assert(!aml_decode_unit_valid(&u));
  u.fullLength=AML_MAX_DECODE_UNIT_BYTES+1;
  assert(!aml_decode_unit_valid(&u));
  assert(!aml_decode_unit_valid(NULL));
  puts("PASS: AML decode unit fragment bounds and total length");
  return 0;
}
