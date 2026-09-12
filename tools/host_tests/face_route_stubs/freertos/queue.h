#pragma once
#include "FreeRTOS.h"
using QueueHandle_t = void*;
// This test does not emulate a scheduler. Unexpected worker use fails closed.
inline QueueHandle_t xQueueCreate(UBaseType_t, UBaseType_t) { std::abort(); }
inline BaseType_t xQueueReceive(QueueHandle_t, void*, TickType_t) { std::abort(); }
inline BaseType_t xQueueSend(QueueHandle_t, const void*, TickType_t) { std::abort(); }
