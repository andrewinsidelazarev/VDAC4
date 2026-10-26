// Флаг «карта Tang у WC» (tang_sd::holdForWc) — отдельно от tang_sd.cpp. Его проверяет смена ядра
// (core_store, ft_host, tang_link, консоль), а сборки со своей копией tang_sd.cpp без захвата карты
// (слои nes-vdac4) должны собираться и с ним: флаг у них просто всегда false.
#include "zifi/tang_sd.hpp"

#if ZIFI_HAS_SPI_LINK

#include <atomic>

namespace zifi {
namespace tang_sd {
namespace {

std::atomic<bool> g_held{false};

}  // namespace

bool heldByWc() { return g_held.load(); }
void setHeldByWc(bool held) { g_held = held; }

}  // namespace tang_sd
}  // namespace zifi

#endif  // ZIFI_HAS_SPI_LINK
