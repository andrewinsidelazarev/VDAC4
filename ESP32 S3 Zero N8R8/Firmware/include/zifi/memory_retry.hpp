#pragma once
#include <stdint.h>
namespace zifi {
// Reclamation may finish on an idle task. Yield before each bounded retry.
// Callers keep ownership of any still-active app's resources.
template<class Allocate,class Reclaim,class Wait>
bool allocateAfterCleanup(Allocate allocate,Reclaim reclaim,Wait wait,
                          uint32_t retries,uint32_t stepMs) {
  if(allocate())return true;
  if(retries==0)return false;
  reclaim();
  for(uint32_t i=0;i<retries;++i) {
    wait(stepMs);
    if(allocate())return true;
  }
  return false;
}
} // namespace zifi
