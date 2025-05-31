#include <Arduino.h>
#include <TinyShell.h>
#include <TableLinker/TableLinker.h>

uint8_t test_function(int d, uint8_t f, String s) {
    Serial.printf("%d %.2f %s\n", d, f, s.c_str());
    return 0;
}

function_manager fm(1);
void* args[3];

void setup() {
    Serial.begin(921600);

    fm.add(0, test_function);
}

void loop() {
    int a = 1;
    int b = 2.0;
    String text = "here";
    args[0] = &a;
    args[1] = &b;
    args[2] = &text;

    Serial.println(fm.call(0, args));
    Serial.println(fm.get_expected_types_string(0));
    delay(1000);
}