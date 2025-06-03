#include <Arduino.h>
#include <TinyShell.h>
#include <TableLinker/TableLinker.h>

uint8_t test_function_0(int d, uint8_t f, String s) {
    Serial.printf("teste_1: %d %.2f %s\n", d, f, s.c_str());
    return 0;
}

uint8_t test_function_1(int d, uint8_t f, String s) {
    Serial.printf("teste_2: %d %.2f %s\n", d, f, s.c_str());
    return 0;
}

uint8_t real_function_0() {
    Serial.println("here_1");
    return 0;
}

uint8_t real_function_1() {
    Serial.println("here_2");
    return 0;
}

TableLinker tb(2);

void setup() {
    Serial.begin(921600);

    tb.create_module(0, 2, "funcoes_texte", "funcoes para testar");
    tb.create_module(1, 2, "funcoes_reais", "funcoes que funcionam");

    tb.add_func_to_module(0, test_function_0, 0, "teste_1", "testa primeiro");
    tb.add_func_to_module(0, test_function_1, 1, "teste_2", "testa segundo");
}

void loop() {
    Serial.print(tb.get_all_module(0));
    delay(1000);
}