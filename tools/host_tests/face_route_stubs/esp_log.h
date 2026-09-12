#pragma once
template<class... Args> inline void RouteTestLog(Args...) {}
#define ESP_LOGW(...) RouteTestLog(__VA_ARGS__)
#define ESP_LOGE(...) RouteTestLog(__VA_ARGS__)
