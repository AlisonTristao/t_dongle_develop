#include <Arduino.h>
#include <AppRuntime.h>

namespace {
AppRuntime g_app;
}

void setup() {
    g_app.begin();
}

void loop() {
    g_app.tick();
}
