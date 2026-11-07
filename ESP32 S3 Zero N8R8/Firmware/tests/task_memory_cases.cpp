
} // namespace zifi
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static void worker(void*){}
int main(){
 using zifi::ModuleApp;ModuleApp app;
 auto reset=[&](unsigned m,unsigned room){app=ModuleApp{};ModuleApp::instance_=&app;mode=m;largest=room;attempts=waits=reclaims=psramAttempts=0;psramWorks=true;};
 reset(0,32768);CHECK(ModuleApp::coreTaskStart(worker,(void*)3,16384,4,0)==ZM_OK);CHECK(attempts==1&&waits==0&&reclaims==0);CHECK(app.tasks_[0].running&&!app.tasks_[0].psram&&app.tasks_[0].arg==(void*)3);
 reset(1,12800);CHECK(ModuleApp::coreTaskStart(worker,nullptr,16384,4,0)==ZM_OK);CHECK(attempts==2&&waits==1&&reclaims==1&&psramAttempts==0);
 reset(1,12800);app.appStarted_=true;CHECK(ModuleApp::coreTaskStart(worker,nullptr,16384,4,0)==ZM_OK);CHECK(attempts==2&&waits==1&&reclaims==0);
 reset(0,12800);CHECK(ModuleApp::coreTaskStart(worker,nullptr,16384,4,0)==ZM_NO_MEMORY);CHECK(attempts==26&&waits==25&&reclaims==1&&!app.tasks_[0].running&&app.tasks_[0].handle==nullptr);
 reset(0,12800);CHECK(ModuleApp::coreTaskStart(worker,nullptr,16384|ZM_TASK_STACK_PSRAM,4,0)==ZM_OK);CHECK(attempts==3&&waits==2&&reclaims==1&&psramAttempts==1);CHECK(lastDepth==16384&&lastCaps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)&&app.tasks_[0].psram);
 reset(0,12800);psramWorks=false;CHECK(ModuleApp::coreTaskStart(worker,nullptr,16384|ZM_TASK_STACK_PSRAM,4,0)==ZM_NO_MEMORY);CHECK(!app.tasks_[0].running&&app.tasks_[0].handle==nullptr);
 reset(0,32768);app.tasks_[0].running=app.tasks_[1].running=true;CHECK(ModuleApp::coreTaskStart(worker,nullptr,4096,4,0)==ZM_BUSY);CHECK(attempts==0);
 reset(0,32768);CHECK(ModuleApp::coreTaskStart(nullptr,nullptr,4096,4,0)==ZM_ERROR);CHECK(attempts==0);
 reset(0,32768);CHECK(ModuleApp::coreTaskStart(worker,nullptr,0,99,-1)==ZM_OK);CHECK(lastDepth==2048&&lastPriority==20&&lastCore==tskNO_AFFINITY);
 std::puts("PASS actual coreTaskStart: immediate allocation, idle cleanup retry, bounded failure, opt-in PSRAM fallback, slot cleanup, busy/invalid input and parameter bounds");
}
