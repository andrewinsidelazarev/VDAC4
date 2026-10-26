#include "zifi/apps/core_builtin.hpp"

#include <string.h>

namespace zifi {
namespace {

constexpr uint32_t kSilenceMs = 1500;

const zm_core_t* g_core = nullptr;
void* g_lock = nullptr;
void* g_wake = nullptr;
volatile int g_state = 0;
volatile bool g_running = false;
volatile bool g_exit = false;
volatile bool g_done = true;
char g_name[16] = "";
char g_error[64] = "";
uint32_t g_version = 0;
uint32_t g_input_ms = 0;

void fail(const char* text) {
  strncpy(g_error, text, sizeof g_error - 1);
  g_error[sizeof g_error - 1] = 0;
  g_core->log("core: %s", text);
  g_state = 255;
}

void worker(void*) {
  while (g_running) {
    if (g_state != 1) {
      g_core->signal_wait(g_wake, 50);
      continue;
    }
    g_core->log("core: loading %s", g_name);
    const int r = g_core->core_load(g_name, &g_version);
    if (r == ZM_NOT_FOUND) {
      fail("core not found in the BL616 cache nor on the Tang SD");
      continue;
    }
    if (r != ZM_OK) {
      fail("core did not start (Tang is back in TS-Config)");
      continue;
    }
    g_core->mutex_lock(g_lock);
    g_input_ms = g_core->millis();
    g_core->mutex_unlock(g_lock);
    g_state = 2;
    g_core->log("core: %s runs, VERSION %08X", g_name, static_cast<unsigned>(g_version));
    while (g_running && g_state == 2) {
      g_core->mutex_lock(g_lock);
      const uint32_t quiet = g_core->millis() - g_input_ms;
      g_core->mutex_unlock(g_lock);
      if (g_exit || quiet > kSilenceMs) {
        g_core->log(g_exit ? "core: exit — back to slot 0" : "core: the plugin is silent — back to slot 0");
        // «вернулись» (3) — только когда TS-Config снова на LINK: иначе плагин ушёл бы в WC, а Evo
        // осталась без экрана (разбор Codex, № 10); не вышло — ошибка, а задача LINK повторит BOOT
        if (g_core->core_back(10000) == ZM_OK) {
          g_state = 3;
        } else {
          fail("could not return the Tang to TS-Config (slot 0)");
        }
        break;
      }
      g_core->signal_wait(g_wake, 50);
    }
  }
  g_done = true;
}

int command(uint8_t cmd, const uint8_t* p, uint16_t n) {
  uint8_t ok = 0;
  if (cmd == 0x70) {
    const int idle = g_state == 0 || g_state == 3 || g_state == 255;
    if (idle && n >= 2 && n <= 16 && p[n - 1] == 0) {
      memcpy(g_name, p, n);
      g_error[0] = 0;
      g_exit = false;
      g_state = 1;
      ok = 1;
    }
    g_core->reply(0xF0, &ok, 1);
    if (ok) {
      g_core->signal_give(g_wake);
    }
  } else if (cmd == 0x71) {
    g_core->mutex_lock(g_lock);
    g_input_ms = g_core->millis();
    g_core->mutex_unlock(g_lock);
    ok = 1;
    g_core->reply(0xF1, &ok, 1);
  } else if (cmd == 0x72) {
    uint8_t st[5 + sizeof g_error + 1];
    st[0] = static_cast<uint8_t>(g_state);
    for (int i = 0; i < 4; ++i) {
      st[1 + i] = static_cast<uint8_t>(g_version >> (8 * i));
    }
    const size_t len = strlen(g_error);
    memcpy(st + 5, g_error, len + 1);
    g_core->reply(0xF2, st, static_cast<uint16_t>(5 + len + 1));
  } else if (cmd == 0x73) {
    g_exit = true;
    if (g_state == 0) {
      g_state = 3;
    }
    ok = 1;
    g_core->reply(0xF3, &ok, 1);
    g_core->signal_give(g_wake);
  } else {
    return ZM_ERROR;
  }
  return ZM_OK;
}

int start(const zm_core_t* core) {
  g_core = core;
  g_lock = core->mutex_create();
  g_wake = core->signal_create();
  if (g_lock == nullptr || g_wake == nullptr) {
    return ZM_NO_MEMORY;
  }
  g_state = 0;
  g_exit = false;
  g_error[0] = 0;
  g_version = 0;
  g_running = true;
  g_done = false;
  const int r = core->task_start(worker, nullptr, 4096, 3, 1);
  if (r != ZM_OK) {
    g_done = true;
    g_running = false;
  }
  return r;
}

void stop() {
  g_running = false;
  if (g_wake != nullptr) {
    g_core->signal_give(g_wake);
  }
  // core_load может идти до ~15 с: дать ему кончиться, прежде чем ядро ESP снимет задачу
  for (int i = 0; i < 2000 && !g_done; ++i) {
    g_core->delay(10);
  }
}

const zm_app_t kApp = {sizeof(zm_app_t), ZM_APP_ABI_MAJOR, ZM_APP_ABI_MINOR, "core", "0.1",
                       0x70, 0x73, start, stop, command, nullptr, nullptr, nullptr};

}  // namespace

const zm_app_t* coreBuiltinApp() { return &kApp; }

}  // namespace zifi
