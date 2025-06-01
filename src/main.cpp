#include <Arduino.h>
#include <TinyShell.h>
#include <TableLinker/TableLinker.h>

uint8_t test_function(int d, uint8_t f, String s) {
    Serial.printf("%d %.2f %s\n", d, f, s.c_str());
    return 0;
}

uint8_t real_function() {
    Serial.println("here");
    return 0;
}

function_manager fm(2);
void* args[3];

void setup() {
    Serial.begin(921600);

    fm.add(0, test_function, "teste", "testa a função");
    fm.add(1, real_function, "real", "função real");
}

void loop() {
    int a = 1;
    int b = 2.0;
    String text = "here";
    args[0] = &a;
    args[1] = &b;
    args[2] = &text;

    Serial.println(fm.call("teste", args));
    Serial.println(fm.call("real"));
    delay(1000);
}