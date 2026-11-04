/* Настройки приложения ядра: соседний .cfg на карте Tang.
 * Включается только в ELF ядра, не в общую прошивку ESP.
 * SDK используется как файловый транспорт. Все значения/правила — у вызывающего ядра.
 */
#ifndef VDAC4_CORE_CFG_H
#define VDAC4_CORE_CFG_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "zm_app.h"

#define CORE_CFG_MAX 2048
typedef struct {
    char path[80];
    char text[CORE_CFG_MAX+1];
    size_t size;
    int loaded, dirty;
} core_cfg_t;

/* Сейчас загрузчик берёт все BIN из /CORES. Имя наследуется от выбранного BIN. */
static int core_cfg_path(core_cfg_t *cfg, const char *name)
{
    size_t n=0;
    if (!name) return 0;
    memcpy(cfg->path,"/CORES/",7);
    while (name[n]) {
        unsigned char c=(unsigned char)name[n];
        if (n>=15 || !((c>='A'&&c<='Z')||(c>='a'&&c<='z')||
                       (c>='0'&&c<='9')||c=='_'||c=='-')) return 0;
        cfg->path[7+n]=(char)(c>='A'&&c<='Z' ? c+32 : c); ++n;
    }
    if (!n) return 0;
    memcpy(cfg->path+7+n,".cfg",5);
    return 1;
}

/* Ищет целую строку key=value. Дубликаты/мусор после числа не принимаются.
 * Неизвестные строки остаются нетронутыми при обновлении знакомого ключа. */
static int core_cfg_integer(const core_cfg_t *cfg, const char *key,
                            unsigned max, unsigned *value, size_t *begin, size_t *end)
{
    size_t keylen=strlen(key),at=0; int found=0;
    while (at<cfg->size) {
        size_t a=at,b=at;
        while (b<cfg->size && cfg->text[b]!='\n') ++b;
        at=b<cfg->size ? b+1 : b;
        size_t finish=b;
        if (finish>a && cfg->text[finish-1]=='\r') --finish;
        if (finish>a+keylen && !memcmp(cfg->text+a,key,keylen) && cfg->text[a+keylen]=='=') {
            if (found) return -1;
            size_t p=a+keylen+1; unsigned v=0;
            if (p==finish) return -1;
            for (;p<finish;++p) {
                unsigned char c=(unsigned char)cfg->text[p];
                if (c<'0'||c>'9' || v>max/10 || (v==max/10 && (unsigned)(c-'0')>max%10)) return -1;
                v=v*10+(unsigned)(c-'0');
            }
            *value=v; if(begin)*begin=a; if(end)*end=at; found=1;
        }
    }
    return found;
}

static int core_cfg_load(core_cfg_t *cfg, const zm_core_t *api,
                         const char *name, const volatile int *active)
{
    memset(cfg,0,sizeof *cfg);
    if (!core_cfg_path(cfg,name) || api->size<offsetof(zm_core_t,sd_write)+sizeof api->sd_write ||
        api->abi_major!=1 || api->abi_minor<9 || !api->sd_read || !api->sd_write) return ZM_ERROR;
    uint8_t *bytes=NULL; size_t size=0;
    int result=api->sd_read(cfg->path,&bytes,&size,CORE_CFG_MAX,active);
    if (result==ZM_NOT_FOUND) {
        char backup[88]; snprintf(backup,sizeof backup,"%s.bak",cfg->path);
        result=api->sd_read(backup,&bytes,&size,CORE_CFG_MAX,active);
        if (result==ZM_OK) cfg->dirty=1; // восстановить основное имя при следующем выходе
    }
    if (result==ZM_NOT_FOUND) {
        strcpy(cfg->text,"# VDAC4 core configuration\nversion=1\n");
        cfg->size=strlen(cfg->text); cfg->loaded=1; return ZM_OK;
    }
    if (result!=ZM_OK) { if(bytes)api->free(bytes); return result; }
    if (!bytes || !size || size>CORE_CFG_MAX || memchr(bytes,0,size)) {
        if(bytes) api->free(bytes);
        return ZM_ERROR;
    }
    memcpy(cfg->text,bytes,size); cfg->text[size]=0; cfg->size=size; api->free(bytes);
    unsigned version=0;
    if (core_cfg_integer(cfg,"version",1,&version,NULL,NULL)!=1 || version!=1) return ZM_ERROR;
    cfg->loaded=1;
    return ZM_OK;
}

static int core_cfg_set(core_cfg_t *cfg, const char *key, unsigned value, unsigned max)
{
    if (!cfg->loaded || value>max) return 0;
    unsigned previous=0; size_t a=cfg->size,b=cfg->size;
    int found=core_cfg_integer(cfg,key,max,&previous,&a,&b);
    if (found<0) return 0;
    if (found && previous==value) return 1;
    char line[80];
    int n=snprintf(line,sizeof line,"%s%s=%u\n",
        !found && a && cfg->text[a-1]!='\n' ? "\n" : "",key,value);
    if (n<0 || (size_t)n>=sizeof line || cfg->size-(b-a)+(size_t)n>CORE_CFG_MAX) return 0;
    memmove(cfg->text+a+n,cfg->text+b,cfg->size-b+1);
    memcpy(cfg->text+a,line,(size_t)n);
    cfg->size=cfg->size-(b-a)+(size_t)n; cfg->dirty=1;
    return 1;
}

static int core_cfg_save(core_cfg_t *cfg, const zm_core_t *api, const volatile int *active)
{
    if (!cfg->loaded) return ZM_ERROR;
    if (!cfg->dirty) return ZM_OK;
    int result=api->sd_write(cfg->path,(const uint8_t*)cfg->text,cfg->size,active);
    if (result==ZM_OK) cfg->dirty=0;
    return result;
}
#endif
