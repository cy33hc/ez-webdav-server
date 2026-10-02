#ifndef __SCE_SYSTEM_SERVICE_H__
#define __SCE_SYSTEM_SERVICE_H__

#include <stdint.h>

typedef struct {
    char padding[8];
    char s_version[10]; // e.g. " 6.720.001"
    char unk[18]; // zeros
    uint32_t i_version; // e.g. 0x06720001
} KernelSwVersion;

#define SCE_SYSTEM_SERVICE_EVENT_ON_RESUME  0x10000000u
#define SCE_SYSTEM_SERVICE_EVENT_BEFORE_SLEEP 0x10000001u

typedef struct SceSystemServiceEvent {
    uint32_t eventType;
    uint8_t  data[60];
} SceSystemServiceEvent;

extern "C"
{
    void sceSystemServicePowerTick();
    int sceSystemServiceReceiveEvent(SceSystemServiceEvent *event);
}
    
#endif