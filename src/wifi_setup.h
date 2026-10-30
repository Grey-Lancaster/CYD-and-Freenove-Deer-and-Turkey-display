#pragma once
#include <Arduino.h>
#include <functional>
void wifiSetupBegin(const char *hostname, std::function<void(const String &ip)> onConnected);
void wifiSetupLoop();
void wifiForgetAll();        // forget every saved network (the picker shows again after a restart)
bool wifiLangEs();                // the on-screen language is Spanish (chosen on the WiFi screen or the web page)
void wifiSetLangEs(bool spanish);
