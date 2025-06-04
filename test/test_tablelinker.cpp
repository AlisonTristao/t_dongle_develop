#include <Arduino.h>
#include <TinyShell.h>
#include <TableLinker/TableLinker.h>

uint8_t test_function_0(int d, float f, string s) {
    Serial.printf("teste_1: %d %.2f %s\n", d, f, s.c_str());
    return 0;
}

uint8_t test_function_1(int d, float f, string s) {
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

TableLinker tb;

int d = 10;
float f = 2.5;
string s = "testando";
void* args[3] = {&d, &f, &s};

void setup() {
    Serial.begin(921600);
    delay(1000);

    tb.create_module("funcoes_texte", "funcoes para testar");
    tb.create_module("funcoes_reais", "funcoes que funcionam");

    tb.add_func_to_module("funcoes_texte", test_function_0, "teste_1", "testa primeiro");
    tb.add_func_to_module("funcoes_texte", test_function_1, "teste_2", "testa segundo");

    tb.add_func_to_module("funcoes_reais", real_function_0, "here_1", "funcao que funciona");
    tb.add_func_to_module("funcoes_reais", real_function_1, "here_2", "funcao que funciona de novo");

    Serial.println(tb.get_all().c_str());
    Serial.println(tb.get_all_module("funcoes_texte").c_str());
    Serial.println(tb.get_all_module("funcoes_reais").c_str());
}

void loop() {
    //Serial.println(tb.get_all().c_str());
    //Serial.println(tb.get_all_module("funcoes_texte").c_str());
    //Serial.println("-> " + String(tb.call("funcoes_reais", "here_1")));
    //Serial.println("-> " + String(tb.call("funcoes_texte", "teste_1", args)));
    delay(1000);
}