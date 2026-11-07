#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include "zm_app.h"
#include "zifi/memory_retry.hpp"
#define ZIFI_HAS_SPI_LINK 1
using BaseType_t=int;using UBaseType_t=unsigned;using TaskHandle_t=void*;
constexpr int pdPASS=1,pdFAIL=0,tskNO_AFFINITY=-1;
constexpr unsigned MALLOC_CAP_INTERNAL=1,MALLOC_CAP_8BIT=2,MALLOC_CAP_SPIRAM=4;
#define pdMS_TO_TICKS(n) (n)
static unsigned mode,attempts,waits,reclaims,psramAttempts,lastDepth,lastPriority,lastCaps;
static int lastCore;static unsigned largest;static bool psramWorks=true;
static size_t heap_caps_get_free_size(unsigned){return 42000;}
static size_t heap_caps_get_largest_free_block(unsigned){return largest;}
static void vTaskDelay(unsigned){++waits;if(mode==1)largest=32768;}
static void bringupLog(const char*,const char*,...){}
static const char *kTag="test";
static BaseType_t xTaskCreatePinnedToCore(void(*)(void*),const char*,unsigned depth,void*,unsigned priority,void** out,int cpu){
 ++attempts;lastDepth=depth;lastPriority=priority;lastCore=cpu;
 if(depth>largest){*out=nullptr;return pdFAIL;}*out=(void*)uintptr_t(1);return pdPASS;
}
static BaseType_t xTaskCreatePinnedToCoreWithCaps(void(*)(void*),const char*,unsigned depth,void*,unsigned,void**out,int,unsigned caps){
 ++psramAttempts;lastDepth=depth;lastCaps=caps;*out=psramWorks?(void*)uintptr_t(2):nullptr;return psramWorks?pdPASS:pdFAIL;
}
namespace zifi {
namespace player_overlay {void releaseMemory(){++reclaims;}}
class ModuleApp {public:
 struct TaskSlot{TaskHandle_t handle{};zm_task_fn fn{};void*arg{};volatile bool running{};bool psram{};};
 bool appStarted_=false;TaskSlot tasks_[2]{};static ModuleApp*instance_;
 static void taskTrampoline(void*){}
 static int coreTaskStart(zm_task_fn,void*,uint32_t,uint8_t,int8_t);
};
ModuleApp* ModuleApp::instance_=nullptr;
