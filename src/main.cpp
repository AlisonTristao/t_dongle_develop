#include <Arduino.h>
#include <TinyShell.h>

// create your shell instance
TinyShell ts;

    /*
    * the sintaxe to send commands to the shell is:
    * <module> -<command> [args]
    * 
    * where:
    * <module> is the name of the module
    * -command is the name of the command
    * [args] are the arguments for the command
    * 
    * example:
    * teste -t1 1, 2, 3
    * 
    * module: teste
    * command: t1
    * args: 1, 2, 3

    * if your function returns uint8_t, it will be printed in the shell
    * if your function returns other types, you need to wrap your function
    * 
    * the return uint8_t (byte) represents the status of the command:
    * 0 = success
    * 255 = error
    * 
    * you can create types if you want to use them in your functions
    * the definition of the types is done in the header file TableLinker.h
    */

// wrapper functions to be used in the shell
uint8_t wrapper_h() {
    Serial.println(ts.get_help("").c_str());
    return RESULT_OK;  // return 0 to indicate success
}

uint8_t wrapper_l(string module = "") {
    Serial.println(ts.get_help(module).c_str());
    return RESULT_OK;  // return 0 to indicate success
}

uint8_t wrapper_e() {
    // explain the command usage
    Serial.println("Usage: <module> -<command> [args]");
    Serial.println("Example: teste -t1 1, 2, 3");  
    return RESULT_OK;  // return 0 to indicate success
}

// example function to be added to the shell
uint8_t teste_1(int a, char b, uint8_t c) {
    Serial.print("Teste 1 called with args: ");
    Serial.print(a);
    Serial.print(", ");
    Serial.print(b);
    Serial.print(", ");
    Serial.print(c);
    Serial.println();
    return RESULT_OK;  // return 0 to indicate success
}

uint8_t teste_2(int a, int b, uint8_t c) {
    Serial.print("Teste 2 called with args: ");
    Serial.print(a);
    Serial.print(", ");
    Serial.print(b);
    Serial.print(", ");
    Serial.print(c);
    Serial.println();
    return RESULT_ERROR;  // return 255 to indicate an error
}

void setup() {
    Serial.begin(921600);
    delay(1000);

    // create the modules
    ts.create_module("teste", "Funcoes de teste com texto");
    ts.create_module("help", "ajuda e informacoes");

    // add the functions to the modules
    ts.add(teste_1, "t1", "Teste de funcao com 3 parametros", "teste");
    ts.add(teste_2, "t2", "Teste de funcao com 3 parametros", "teste");

    // add the wrapper functions to the help module
    ts.add(wrapper_h, "h", "Lista os modulos", "help");
    ts.add(wrapper_l, "l", "Lista as funcoes de um modulo", "help");
    ts.add(wrapper_e, "e", "Explica o uso do comando", "help");
}

String commandBuffer = "";

void loop() {

    // the code below reads commands from the serial port
    // it has nothing to do with the shell itself
    // you can remove it if you want to use the shell in another way
    // you just need to send a string formatted as "<module> -<command> [args]\n"

    while (Serial.available()) {
        char c = Serial.read();

        // detects if the character is a control character
        if (c == 8 || c == 127) {  // backspace/delete
            if (!commandBuffer.isEmpty()) {
                commandBuffer.remove(commandBuffer.length() - 1);

                // deletes the last character from the serial output
                Serial.print("\b \b");
            }
            continue;
        }

        // echo the character to the serial output
        Serial.write(c);

        // end of command detection
        if (c == '\n') {
            commandBuffer.trim();  // remove leading/trailing whitespace
            Serial.println();      // new line for better readability

            Serial.println("Received command: " + commandBuffer);

            // run the command in the shell
            string response = ts.run_line_command(commandBuffer.c_str());

            delay(100);  // give some time for the shell to process the command
            Serial.println(String(response.c_str()));

            commandBuffer = "";
        } else {
            commandBuffer += c;
        }
    }

    delay(10);
}