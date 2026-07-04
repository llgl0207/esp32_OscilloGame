#pragma once
#include <Arduino.h>

void initWebServer();
void suspendWebServer();
void resumeWebServer();
void updateWebUIStatus(String status);

