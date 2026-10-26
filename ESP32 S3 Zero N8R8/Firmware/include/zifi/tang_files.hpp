#pragma once
// Файловые операции без открытых дескрипторов на стороне ELF. Вызывающая
// прошивка держит tang_sd::Session на всю операцию, включая mount.
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include "zm_app.h"

namespace zifi { namespace tang_files {
using Clock = uint32_t (*)(void);
using Delay = void (*)(uint32_t);

inline bool pathOf(const char* in, char out[528]) {
  if (!in || in[0] != '/' || strnlen(in, 512) >= 512) return false;
  const char* part = in + 1;
  for (const char* p = part;; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (c && (c < 32 || c == '\\' || c == ':')) return false;
    if (!c || c == '/') {
      const size_t len = p - part;
      if ((len == 1 && part[0] == '.') || (len == 2 && part[0] == '.' && part[1] == '.') ||
          (len == 0 && c)) return false;
      if (!c) break;
      part = p + 1;
    }
  }
  snprintf(out, 528, "/tangsd%s", in);
  return true;
}

inline bool alive(const volatile int* active, Clock now, uint32_t started) {
  return (!active || *active) && now() - started < 15000;
}

inline int list(const char* path, const char* suffix, uint32_t skip,
                zm_sd_entry_t* entries, uint32_t capacity, uint32_t* count, int* more,
                const volatile int* active, Clock now, Delay pause) {
  if (count) *count = 0;
  if (more) *more = 0;
  char folder[528];
  if (!entries || !count || !more || capacity < 1 || capacity > 16 || !suffix ||
      strnlen(suffix, 16) >= 16 || !pathOf(path, folder)) return ZM_ERROR;
  const uint32_t started = now();
  DIR* dir = opendir(folder);
  if (!dir) return ZM_NOT_FOUND;
  uint32_t found = 0;
  int result = ZM_OK;
  for (;;) {
    errno = 0;
    dirent* item = readdir(dir);
    if (!item) { if (errno) result = ZM_ERROR; break; }
    if (!alive(active, now, started)) { result = ZM_TIMEOUT; break; }
    const size_t len = strlen(item->d_name), ext = strlen(suffix);
    if (!len || len > 255 || !strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")) continue;
    char name[800];
    snprintf(name, sizeof name, "%s/%s", folder, item->d_name);
    struct stat st = {};
    if (stat(name, &st)) { result = ZM_ERROR; break; }
    const bool directory = S_ISDIR(st.st_mode);
    if (!directory && (!S_ISREG(st.st_mode) || len < ext || strcasecmp(item->d_name + len - ext, suffix))) continue;
    if (found++ < skip) { pause(1); continue; }
    if (*count == capacity) { *more = 1; break; }
    zm_sd_entry_t& e = entries[(*count)++];
    memset(&e, 0, sizeof e);
    memcpy(e.name, item->d_name, len);
    e.flags = directory ? 1 : 0;
    e.size = st.st_size < 0 || static_cast<uint64_t>(st.st_size) > UINT32_MAX ? UINT32_MAX : st.st_size;
    pause(1);
  }
  closedir(dir);
  if (result != ZM_OK) { *count = 0; *more = 0; }
  return result;
}

inline int read(const char* path, uint8_t** data, size_t* size, size_t maximum,
                const volatile int* active, void* (*alloc)(size_t,uint32_t), void (*release)(void*),
                Clock now, Delay pause) {
  if (data) *data = nullptr;
  if (size) *size = 0;
  char file[528];
  if (!data || !size || !maximum || maximum > 7u*1024u*1024u || !pathOf(path,file)) return ZM_ERROR;
  struct stat st = {};
  if (stat(file, &st)) return errno == ENOENT ? ZM_NOT_FOUND : ZM_ERROR;
  if (!S_ISREG(st.st_mode)) return ZM_ERROR;
  if (st.st_size <= 0 || static_cast<uint64_t>(st.st_size) > maximum) return ZM_ERROR;
  const size_t length = static_cast<size_t>(st.st_size);
  FILE* f = fopen(file, "rb");
  if (!f) return errno == ENOENT ? ZM_NOT_FOUND : ZM_ERROR;
  auto buffer = static_cast<uint8_t*>(alloc(length, ZM_MEM_PSRAM));
  if (!buffer) { fclose(f); return ZM_NO_MEMORY; }
  const uint32_t started = now();
  size_t offset = 0;
  int result = ZM_OK;
  while (offset < length) {
    if (!alive(active, now, started)) { result = ZM_TIMEOUT; break; }
    const size_t n = length - offset > 8192 ? 8192 : length - offset;
    if (fread(buffer + offset, 1, n, f) != n) { result = ZM_ERROR; break; }
    offset += n;
    pause(1);
  }
  if (result == ZM_OK && (fgetc(f) != EOF || ferror(f))) result = ZM_ERROR;
  if (fclose(f) && result == ZM_OK) result = ZM_ERROR;
  if (result != ZM_OK) { release(buffer); return result; }
  *data = buffer;
  *size = length;
  return ZM_OK;
}

// Транспорт не разбирает содержимое и не знает ядер/картриджей/настроек.
// Файл заменяется после закрытия и побайтового чтения обратно.
// Имена path.tmp/path.bak зарезервированы для временного файла и предыдущей копии.
inline int write(const char* path, const uint8_t* data, size_t size,
                 const volatile int* active, Clock now, Delay pause) {
  char file[528], temp[536], backup[536];
  if (!data || !size || size > 7u*1024u*1024u || !pathOf(path,file)) return ZM_ERROR;
  snprintf(temp,sizeof temp,"%s.tmp",file);
  snprintf(backup,sizeof backup,"%s.bak",file);
  const uint32_t started = now();
  FILE* f=fopen(temp,"wb");
  if (!f) return ZM_ERROR;
  int result=ZM_OK;
  for(size_t at=0;at<size;) {
    if(!alive(active,now,started)) { result=ZM_TIMEOUT; break; }
    const size_t count=size-at>4096 ? 4096 : size-at;
    if(fwrite(data+at,1,count,f)!=count) { result=ZM_ERROR; break; }
    at+=count; pause(1);
  }
  if(result==ZM_OK && (fflush(f) || fsync(fileno(f)))) result=ZM_ERROR;
  if(fclose(f)) result=ZM_ERROR;
  if(result==ZM_OK) {
    f=fopen(temp,"rb");
    if(!f) result=ZM_ERROR;
    else {
      uint8_t buffer[512];
      for(size_t at=0;at<size && result==ZM_OK;) {
        const size_t count=size-at>sizeof buffer ? sizeof buffer : size-at;
        if(!alive(active,now,started)) result=ZM_TIMEOUT;
        else if(fread(buffer,1,count,f)!=count || memcmp(buffer,data+at,count)) result=ZM_ERROR;
        at+=count; pause(1);
      }
      if(result==ZM_OK && (fgetc(f)!=EOF || ferror(f))) result=ZM_ERROR;
      if(fclose(f)) result=ZM_ERROR;
    }
  }
  if(result!=ZM_OK) { remove(temp); return result; }
  struct stat st={}; bool hadOld=false;
  if(stat(file,&st)==0) {
    if(remove(backup) && errno!=ENOENT) return ZM_ERROR;
    if(rename(file,backup)) return ZM_ERROR;
    hadOld=true;
  } else if(errno!=ENOENT) return ZM_ERROR;
  if(rename(temp,file)) {
    if(hadOld) rename(backup,file);
    return ZM_ERROR;
  }
  return ZM_OK;
}
} }
