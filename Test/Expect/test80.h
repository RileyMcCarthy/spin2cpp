#ifndef test80_Class_Defined__
#define test80_Class_Defined__

#include <stdint.h>
#include "FullDuplexSerial.h"

class test80 {
public:
  FullDuplexSerial	Fds;
  void	Init(void);
private:
  char	Namebuffer[14];
};

#endif
