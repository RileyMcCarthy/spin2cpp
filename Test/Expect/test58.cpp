#include <propeller.h>
#include "test58.h"

uint8_t test58::dat[] = {
  0xdb, 0x0f, 0xc9, 0x3f, 
};
int32_t test58::Getval(void)
{
  return ((int32_t *)&dat[0])[0];
}

