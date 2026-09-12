#pragma once
#include "FreeRTOS.h"
using TaskHandle_t = void*;
inline BaseType_t xTaskCreatePinnedToCore(void (*)(void*), const char*, uint32_t,
                                        void*, UBaseType_t, TaskHandle_t*, BaseType_t) {
    std::abort();
}
