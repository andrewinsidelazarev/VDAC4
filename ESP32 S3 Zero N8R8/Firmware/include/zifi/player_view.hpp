#pragma once

// Отрисовка экрана проигрывателя в холст RGB565 640×360 (player_overlay: на выводе — ×2, весь
// кадр 1280×720). Дизайн — Codex, утверждён пользователем 2026-10-21; исходники — в
// Sound/player_ui (player_ui.h, player_ui.cpp, шрифт fonts/pui_font.cpp), прошивка берёт их без
// копии (main/CMakeLists.txt). Вход и выход: player_view::begin / end / render, kCoverSide —
// сторона обложки, которую просит отрисовка.
#include "player_ui.h"
