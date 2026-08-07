# Como alteramos este código

Política de engenharia do firmware T-Dongle-S3. Objetivo: manter o projeto fácil de
estender por comandos novos sem virar um emaranhado de includes, e deixar explícito o que
já foi decidido de propósito (para não reabrir a mesma discussão a cada sessão).

Leia também o [README.md](README.md) para a visão geral de arquitetura e o mapa de
comandos existentes.

## 1. Princípios gerais

- **Sem abstração prematura.** Um comando novo não precisa de uma classe nova; três
  `if`/wrappers parecidos são melhores que uma abstração genérica pra "qualquer comando
  futuro". Só extraia um helper quando o padrão já se repetiu de verdade.
- **Sem comentário do óbvio.** Comentário só quando explica um *porquê* não óbvio (uma
  quirk de hardware, uma decisão de segurança, um workaround de bug específico — ex.: o
  comentário sobre `toPanelColor` no LCD, ou o do `CONFIRMAR`/`sudo` sendo guarda-corpo vs.
  controle de acesso real). Nome de função e módulo já dizem o *o quê*.
- **Sem validação de cenário impossível.** Confie nos limites internos (ex.: `context()`
  já garantido não-nulo depois do `bind()`); valide só fronteira real (entrada do usuário
  via shell, payload ESP-NOW recebido).
- **Texto voltado ao usuário em português** (mensagens de `printLine`, `help -e`, nomes de
  comando); **comentários de código e identificadores em inglês**. É a convenção já em uso
  em todo o projeto — mantenha para não misturar os dois em um mesmo arquivo.
- **Build limpo é obrigatório antes de considerar uma mudança pronta**:
  ```bash
  platformio run -e tdongle-s3
  ```
  Zero erros e zero warnings novos (os warnings pré-existentes do `Sqlite3Esp32` upstream,
  filtrados por `scripts/pio_warnings.py`, não contam).

## 2. Padrão de "módulo de comando"

Cada módulo do TinyShell (`dongle`, `espnow`, `database`, `sudo`, `help`) é uma lib própria
em `lib/<Nome>Commands/` com exatamente essa forma:

```cpp
// <Nome>Commands.cpp
namespace {
using ShellCommandSupport::context;
using ShellCommandSupport::failWithCode;
using ShellCommandSupport::printLine;
// ...

uint8_t wrapper_modulo_comando(string arg1, string arg2 = "") {
    if (context().<serviço> == nullptr) {
        return failWithCode(AppError::Code::..._NOT_READY, "mensagem em pt-br");
    }
    // ... lógica ...
    printLine("[modulo] resultado");
    return RESULT_OK;
}
} // namespace

namespace <Nome>Commands {
uint8_t registerAll() {
    context().shell->create_module("modulo", "descrição em inglês, curta");
    context().shell->add(wrapper_modulo_comando, "comando", "help text: <args>", "modulo");
    return RESULT_OK;
}
}
```

### Passo a passo para adicionar um comando novo

1. Escreva o wrapper na lib do módulo certo (`DongleCommands` para algo local ao dongle,
   `EspNowCommands` para gestão de peers/envio, `DatabaseCommands` para o SQLite, ou crie um
   módulo novo se for um domínio novo de verdade — como foi feito para `SudoCommands`).
2. Pegue os serviços via `ShellCommandSupport::context()` (`shell`, `espNow`,
   `peripherals`, `lcdDashboard`, `database`, `io`). Nunca acesse os objetos globais direto
   — sempre pelo `context()`.
3. Toda saída pro usuário via `ShellCommandSupport::printLine()` (vai pra serial + LCD +
   buffer de persistência ao mesmo tempo). Não use `Serial.print` direto num wrapper.
4. Erros: `failWithCode(AppError::Code::X, "detalhe")` retorna `RESULT_ERROR` e imprime.
   Avisos não-fatais: `warnWithCode(...)` (não muda o resultado do comando).
5. Se o comando for destrutivo (apaga dado, reseta hardware): exija
   `SudoManager::isElevated(ShellCommandSupport::currentUserId())` antes de agir — ver
   seção 5. Não reintroduza um token de confirmação em texto (`CONFIRMAR`) como única
   proteção; isso já foi removido de propósito por não proteger contra um peer remoto.
6. Registre em `registerAll()` com `context().shell->add(wrapper, "nome", "help: <args>",
   "modulo")`. Argumentos opcionais do TinyShell = parâmetro com valor default no wrapper.
7. Documente em `help -e` (`lib/HelpCommands/HelpCommands.cpp`) se for algo que um usuário
   descobriria por conta própria (não precisa documentar toda flag ali, só o essencial).
8. Atualize a tabela do módulo correspondente no `README.md`.
9. Rode o build limpo (seção 1) antes de dar por encerrado.

## 3. Arquitetura e camadas

O grafo de dependências entre libs foi auditado nesta revisão: **é um DAG, sem nenhum
ciclo real** (nenhum par onde A inclui B e B inclui A de volta, nem direto nem via `.cpp`).
Isso importa porque o LDF do PlatformIO (modo chain) tem uma falha conhecida: dependência
circular entre duas libs quebra a resolução transitiva de headers; dependência
unidirecional — mesmo em cadeia longa (`AppRuntime → EspNowConfig → ShellConfig →
ShellCommandSupport → DatabaseStore → EspNowManager`) — funciona sem problema. **Antes de
adicionar um novo `#include` entre libs, confirme que a lib alvo (no `.h` e no `.cpp`) não
inclui de volta, nem transitivamente, a lib de origem.** Um `grep` rápido pelos includes já
resolve a dúvida.

Camadas atuais (de cima pra baixo, cada uma só depende das de baixo):

1. **`AppRuntime`** — único dono dos objetos runtime (`EspNowManager`, `DatabaseStore`,
   `DonglePeripherals`, `LcdDashboard`, `TinyShell`, `ShellSerial`); monta tudo no `begin()`.
2. **`ShellConfig`** — recebe os objetos via `bind()`, registra os módulos de comando,
   implementa `runLine()` (chaining por `;`, alias, dedup de histórico, persistência).
3. **Módulos de comando** (`DongleCommands`, `EspNowCommands`, `DatabaseCommands`,
   `SudoCommands`, `HelpCommands`) — cada um só sabe do seu próprio domínio de comandos.
4. **`ShellCommandSupport`** — o *hub* intencional: agrega ponteiros pros serviços runtime
   em um `Context` só, porque as funções wrapper do TinyShell são ponteiros de função
   simples (sem forma de receber um contexto por chamada) — um contexto global é a solução
   prática pra esse formato de dispatch, não é dívida técnica a "consertar".
5. **Serviços de domínio** (`EspNowManager`, `DatabaseStore`, `DonglePeripherals`,
   `LcdDashboard`, `SudoManager`) — sem saber nada do shell.

### Quando incluir direto vs. quando passar por `Context`

`Context` (em `ShellCommandSupport.h`/`ShellConfig.h`) só guarda **ponteiros pra objetos
runtime instanciados** (`EspNowManager*`, `DatabaseStore*`, etc. — coisas que o `AppRuntime`
possui e injeta). `SudoManager` e `ShellAliases`, assim como o próprio
`ShellCommandSupport`, são **namespaces de serviço sem estado por instância** (funções
livres sobre um `static`/global interno) — está correto um módulo de comando incluir esses
direto (`#include "SudoManager.h"`) em vez de forçá-los para dentro do `Context`. Regra
prática: se o serviço é "um objeto que o `AppRuntime` cria e configura uma vez", ele entra
no `Context`; se é "um utilitário de processo, tipo uma função global", inclui direto.

## 4. Códigos de erro (`include/error_codes.h`)

Um range de 100 por subsistema, adicione sempre no fim do range do seu subsistema:

| Range | Subsistema |
|---|---|
| 1000–1099 | Shell / core / permissão (`SHELL_NOT_READY`, `PERMISSION_DENIED`, ...) |
| 1100–1199 | Dongle / periféricos (RTC, LCD, SD) |
| 1200–1299 | ESP-NOW |
| 1300–1399 | Database |

Ao adicionar um código: declare no `enum class Code`, adicione o `case` em `name()`. Nunca
reaproveite um número já usado (mesmo de um código removido) — histórico de logs no SD pode
ter referências antigas.

## 5. Modelo de segurança

- **`sudo` é a única barreira real para comandos destrutivos** (`dongle -sd_wipe`,
  `database -drop`, `database -rebuild`, `database -clear_logs`). A senha
  (`BoardConfig::SUDO_PASSWORD` em `include/config.h`) é uma constante compilada — trocar
  exige reflash. Elevação é por identidade (`SudoManager`), vive só em RAM, e sempre reseta
  no boot — não persista elevação em SD/banco.
- **Identidade** = string livre: `"serial"` pro console local, `"espnow:<MAC>"` por peer
  registrado. Um transporte novo (ex. MQTT) só precisa de um prefixo de identidade novo,
  sem mudar `SudoManager`.
- **Execução remota via ESP-NOW (`CMDO`)**: só roda comando de um MAC já cadastrado no
  registry de peers. Decisão deliberada: **sem restrição de conteúdo** — um peer cadastrado
  pode mandar qualquer comando, incluindo `sudo -login`. Ou seja, a defesa contra um peer
  malicioso é (a) controlar quem vira peer cadastrado e (b) a senha do `sudo`. Não adicione
  uma lista de comandos "permitidos"/"proibidos" via `CMDO` sem alinhar antes — foi uma
  escolha explícita, não uma lacuna esquecida.
- Se um comando novo for destrutivo o suficiente pra merecer o mesmo tratamento, siga o
  padrão da seção 2, passo 5 — não invente um mecanismo de confirmação novo.

## 6. Git

- Só crie commits quando pedido explicitamente.
- Build limpo (seção 1) antes de qualquer commit.
- Mensagem de commit em português, curta, focada no "porquê" — mesmo estilo do histórico
  atual (`git log`).
