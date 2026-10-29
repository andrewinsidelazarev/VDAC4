// Codex: подтверждённый возврат M_SEL в slot0; состояние занимает одно слово RAM.
#pragma once
#include <atomic>
#include <cstdint>
namespace zifi {
class FtDetachGuard {
    // Codex: младшие два бита — открыт/был выбран; время округлено вниз до 4 мс.
    // Переполнение millis обрабатывается без знака, как в остальных таймерах ядра.
    std::atomic<uint32_t> state_{0};
public:
    void opened(bool selected=false){state_.fetch_or(selected?3u:1u,std::memory_order_release);}
    bool due(uint32_t now){
        uint32_t s=state_.load(std::memory_order_acquire);
        for(;;){
            if(!(s&1)||uint32_t(now-(s&~3u))<100)return false;
            // Codex: не потерять выборку, замеченную задачей приложения при ft_open.
            if(state_.compare_exchange_weak(s,(s&3u)|(now&~3u),std::memory_order_acq_rel,std::memory_order_acquire))return true;
        }
    }
    bool observe(uint32_t id,uint32_t version,uint32_t status){
        if(id!=0x56444334u||((version>>16)&255)!=1)return false;
        const unsigned personality=version>>24;const bool selected=(status&(1u<<12))!=0;
        if(personality==2&&selected)state_.fetch_or(2,std::memory_order_release);
        return (state_.load(std::memory_order_acquire)&2)&&personality==1&&!selected;
    }
    void reset(){state_.store(0,std::memory_order_release);}
};
static_assert(sizeof(FtDetachGuard)==4,"FT detach guard RAM budget");
}
