#pragma once

#include "zifi/board.hpp"

#if ZIFI_HAS_SPI_LINK

#include "zifi/tang_vfs.hpp"

namespace zifi {
namespace tang_sd {

// Источник моста VFS для шары SMB «TANG» (TangVfs над этой картой); один на прошивку.
TangVfs& vfs();
// Шара есть: Tang на связи, карта вставлена (монтируется) и не отдана WC (SD2).
bool shareReady();

}  // namespace tang_sd
}  // namespace zifi

#endif  // ZIFI_HAS_SPI_LINK
