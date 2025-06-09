#include <Arduino.h>
#include <TinyShell.h>

TinyShell ts;

uint8_t wrapper_h() {
    Serial.println(ts.get_help("").c_str());
    return 0;
}

uint8_t wrapper_l(string module = "") {
    Serial.println(ts.get_help(module).c_str());
    return 0;
}

uint8_t wrapper_e() {
    // explain the command usage
    Serial.println("Usage: <module> -<command> [args]");
    Serial.println("Example: teste -t1 1, 2, 3");  
    return 0;
}

// Example function to be added to the shell
uint8_t teste_1(int a, int b, uint8_t c) {
    Serial.print("Teste 1 called with args: ");
    Serial.print(a);
    Serial.print(", ");
    Serial.print(b);
    Serial.print(", ");
    Serial.print(c);
    Serial.println();
    return 0;
}

uint8_t teste_2(int a, int b, uint8_t c) {
    Serial.print("Teste 2 called with args: ");
    Serial.print(a);
    Serial.print(", ");
    Serial.print(b);
    Serial.print(", ");
    Serial.print(c);
    Serial.println();
    return 1;
}

void setup() {
    Serial.begin(921600);
    delay(1000);

    ts.create_module("teste", "Funcoes de teste com texto");
    ts.create_module("help", "ajuda e informacoes");
    ts.add(teste_1, "t1", "Teste de funcao com 3 parametros", "teste");
    ts.add(teste_2, "t2", "Teste de funcao com 3 parametros", "teste");
    ts.add(wrapper_h, "h", "Lista os modulos", "help");
    ts.add(wrapper_l, "l", "Lista as funcoes de um modulo", "help");
    ts.add(wrapper_e, "e", "Explica o uso do comando", "help");
}

String commandBuffer = "";

void loop() {
    while (Serial.available()) {
        char c = Serial.read();

        // Detecção de backspace/delete
        if (c == 8 || c == 127) {  // backspace/delete
            if (!commandBuffer.isEmpty()) {
                commandBuffer.remove(commandBuffer.length() - 1);

                // Apaga no terminal: volta cursor, escreve espaço, volta cursor
                Serial.print("\b \b");
            }
            continue;
        }

        // Ecoa caractere normalmente
        Serial.write(c);

        // Fim do comando
        if (c == '\n') {
            commandBuffer.trim();  // Remove \r e espaços
            Serial.println();      // Nova linha visual

            //Serial.println("Received command: " + commandBuffer);
            string response = ts.run_line_command(commandBuffer.c_str());
            delay(100);  // Aguarda a serial estar pronta
            Serial.println(String(response.c_str()));

            commandBuffer = "";
        } else {
            commandBuffer += c;
        }
    }

    delay(10);
}