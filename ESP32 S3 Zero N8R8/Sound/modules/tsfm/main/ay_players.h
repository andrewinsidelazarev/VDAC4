// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "zm_source.h"
namespace tsfm {
struct AyPlayer;
AyPlayer* ay_open(const uint8_t* data, size_t size, const char* type, const zm_host_t* host);
void ay_close(AyPlayer* player);
bool ay_reset(AyPlayer* player);
bool ay_tick(AyPlayer* player, uint8_t regs[2][14], uint16_t masks[2]);
uint32_t ay_ticks(const AyPlayer* player);
unsigned ay_chips(const AyPlayer* player);
const char* ay_title(const AyPlayer* player);
const char* ay_author(const AyPlayer* player);
}
