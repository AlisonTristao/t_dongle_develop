# T-Dongle S3 Firmware (t_dongle_develop)

Firmware para ESP32-S3 (LILYGO T-Dongle-S3) com:
- shell interativo via Serial USB
- saida em Serial + LCD ST7735
- comunicacao ESP-NOW
- persistencia SQLite em SD_MMC
- controle de perifericos locais (LED, LCD, SD, clock)

Este projeto foi organizado para funcionar como um terminal embarcado de operacao e diagnostico, com historico persistente de comandos e trafego ESP-NOW.

## 1. Visao Geral

Objetivos principais:
- operar o dongle localmente por shell (`<modulo> -<comando> [args]`)
- registrar peers ESP-NOW e mensagens enviadas/recebidas
- manter historico em banco SQLite no SD
- exibir feedback tanto no monitor serial quanto no LCD

Stack principal:
- Framework Arduino (PlatformIO)
- ESP32-S3 (`esp32-s3-devkitc-1`)
- TinyShell (modulos e wrappers de comando)
- SQLite (`Sqlite3Esp32`) no SD_MMC
- Adafruit GFX + ST7735

## 2. Hardware Alvo: T-Dongle-S3

Mapeamento centralizado em `include/config.h`.

Perifericos usados:
- LED RGB onboard (linhas DI/CI): GPIO40 e GPIO39
- LCD ST7735 SPI: CS=4, SDA=3, SCL=5, DC=2, RST=1, BL=38
- SD_MMC: D0=14, D1=17, D2=18, D3=21, CLK=12, CMD=16
- Botao BOOT: GPIO0
- USB nativo: D-=19, D+=20

Observacoes de hardware implementadas no codigo:
- calibracao de offset do painel ST7735 (`col_start=26`, `row_start=1`)
- polaridade de backlight configuravel (default pratico: LOW=ON)
- startup visual com LED animado e status no LCD

## 3. Arquitetura do Codigo

A arquitetura esta dividida em camadas de responsabilidade:

```text
Application Layer
  - src/main.cpp
  - src/startup_config.cpp
  - src/shell_config.cpp
  - src/shell_output.cpp
  - src/espnow_config.cpp

Service/Domain Layer (lib)
  - EspNowManager
  - DatabaseStore
  - ShellSerial
  - LcdTerminal
  - DonglePeripherals

Platform Layer
  - Arduino/ESP-IDF (WiFi, esp_now, FreeRTOS, SD_MMC, time)
```

Responsabilidade de cada modulo:

- `DonglePeripherals`
  - abstracao de LED, LCD e SD
  - init seguro de perifericos
  - utilitarios de status de SD e limpeza recursiva (`sd_wipe`)

- `EspNowManager`
  - inicializacao ESP-NOW
  - registry local de peers (max 16)
  - envio para peer, MAC direta, ou todos os peers
  - envio com espera de callback de entrega (`status=true/false`)

- `DatabaseStore`
  - bootstrap do banco no SD
  - migracoes runtime idempotentes
  - persistencia de peers, logs de comando, logs ESP-NOW, eventos de boot

- `ShellSerial`
  - leitura de linha nao bloqueante
  - edicao in-line (backspace e setas)
  - historico com navegacao `ESC[A` / `ESC[B`

- `LcdTerminal`
  - rendering tipo terminal no ST7735
  - moldura, area de texto, truncamento e limpeza de tela

- `ShellConfig`
  - bind do contexto runtime
  - registro de modulos/comandos TinyShell
  - wrappers de comandos (`dongle`, `espnow`, `database`, `help`)
  - normalizacao de comandos (`espnow -send_to <texto>` vira `espnow -send_to 000, <texto>`)

## 4. Fluxo de Execucao (setup/loop)

### Setup

Sequencia executada em `src/main.cpp`:

1. configura pinos base (`BoardConfig::initBoardPins(false)`)
2. inicia shell serial (`ShellSerial`) em 921600
3. aguarda monitor serial conectado com animacao de LED (`StartupConfig::waitForSerialAndAnimateLed`)
4. inicia SD
5. mostra MAC Wi-Fi local
6. pede data/hora e aplica no RTC (`StartupConfig::promptAndSetDateTime`)
7. conecta callbacks ESP-NOW (`EspNowConfig::attachCallbacks`)
8. habilita fila async RX (`EspNowConfig::enableAsyncRx(24)`)
9. inicializa ESP-NOW (`espNowManager.begin(0, false)`)
10. inicializa database e loga boot event (`databaseStore.begin` + `logBootEvent("power_on")`)
11. cria task FreeRTOS para processar RX fora do callback
12. faz bind do shell e registra comandos default

### Loop principal

1. le input da serial (`serialShell.readInputLine`)
2. roda comando via TinyShell (`ShellConfig::runLine`)
3. imprime resposta formatada (`ShellOutput::printResponse`)
4. `delay(1)` cooperativo

## 5. Arquitetura TinyShell

Sintaxe base:

```text
<modulo> -<comando> [args]
```

Exemplos:

```text
help -e
dongle -led 255, 0, 0
espnow -send_to 000, "dongle -ping"
database -status
```

### 5.1 Modulo `help`

| Comando | Descricao |
|---|---|
| `help -h` | lista modulos |
| `help -l <modulo>` | lista comandos de um modulo |
| `help -e` | mostra exemplos e dicas de uso |

### 5.2 Modulo `dongle`

| Comando | Sintaxe | Descricao |
|---|---|---|
| `ping` | `dongle -ping` | teste local (`pong`) |
| `clock` | `dongle -clock` | mostra data/hora local e epoch |
| `set_clock` | `dongle -set_clock "YYYY-MM-DD HH:MM:SS"` | ajusta RTC local |
| `run` | `dongle -run "texto"` | placeholder de comando local |
| `led` | `dongle -led r, g, b` | seta LED RGB (0-255) |
| `led_off` | `dongle -led_off` | desliga LED |
| `lcd` | `dongle -lcd "texto"` | escreve no LCD |
| `lcd_clear` | `dongle -lcd_clear` | limpa LCD |
| `lcd_rot` | `dongle -lcd_rot 0|1|2|3` | muda rotacao |
| `lcd_rot_get` | `dongle -lcd_rot_get` | consulta rotacao atual |
| `lcd_bl` | `dongle -lcd_bl 0|1` | controla backlight (`0=ON`, `1=OFF`) |
| `lcd_bl_inv` | `dongle -lcd_bl_inv 1|0` | define polaridade (`1=HIGH_ON`, `0=LOW_ON`) |
| `lcd_reinit` | `dongle -lcd_reinit` | reinicializa LCD |
| `sd_init` | `dongle -sd_init` | reinicia SD |
| `sd_status` | `dongle -sd_status` | status de cartao/frequencia/capacidade |
| `sd_wipe` | `dongle -sd_wipe` | apaga SD e tenta recriar database |

### 5.3 Modulo `espnow`

| Comando | Sintaxe | Descricao |
|---|---|---|
| `list` | `espnow -list` | lista peers; inclui alias virtual `000` |
| `add` | `espnow -add "AA:BB:CC:DD:EE:FF", "nome", "desc"` | adiciona peer |
| `remove` | `espnow -remove 1` | remove peer por indice (1..N) |
| `remove_mac` | `espnow -remove_mac "AA:BB:CC:DD:EE:FF"` | remove peer por MAC |
| `update` | `espnow -update 1, "nome", "desc"` | atualiza metadados |
| `send_to` | `espnow -send_to 1, "msg"` | envia para 1 peer |
| `send_to` | `espnow -send_to 000, "msg"` | envia para broadcast alias 000 |
| `send_all` | `espnow -send_all "msg"` | envia para todos com agregacao de status |

Detalhes importantes:
- `send_to 000` usa MAC default de broadcast do banco (fallback `FF:FF:FF:FF:FF:FF`)
- `send_all` tenta peer a peer com status; se nao houver peers, cai em broadcast
- shortcut: `espnow -send_to "mensagem"` e normalizado para `espnow -send_to 000, "mensagem"`
- para usar virgula literal dentro de um argumento, use escape `\\,` (ex.: `espnow -send_to 2, HB -apply_pwm 50\\, 50`)

### 5.4 Modulo `database`

| Comando | Sintaxe | Descricao |
|---|---|---|
| `init` | `database -init` | abre banco e sincroniza peers |
| `status` | `database -status` | resumo geral (rows por tabela) |
| `tables` | `database -tables` | lista tabelas |
| `read` | `database -read peers, 20` | leitura simples com limite |
| `logs` | `database -logs 20` | historico de comandos + output |
| `espnow_history` | `database -espnow_history 30` | historico TX/RX com data e status |
| `drop` | `database -drop nome_tabela` | remove tabela |
| `rebuild` | `database -rebuild` | recria banco via bootstrap |
| `exec` | `database -exec "SELECT ..."` | executa SQL livre |

## 6. Arquitetura do Banco de Dados (SQLite)

### 6.1 Caminhos no SD

- diretorio: `/database`
- bootstrap script: `/database/bootstrap.sql`
- arquivo principal: `/database/dongle.db`
- caminho usado pelo SQLite: `/sdcard/database/dongle.db`

### 6.2 Ciclo de inicializacao

Quando `DatabaseStore::begin()` roda:

1. garante diretorio `/database`
2. cria `bootstrap.sql` se estiver ausente
3. abre `dongle.db`
4. aplica bootstrap SQL
5. aplica migracoes runtime (idempotentes)
6. garante peer virtual default `000` (`FF:FF:FF:FF:FF:FF`)
7. carrega peers para `EspNowManager` (ignorando peer broadcast como peer real)

### 6.3 Schema

Tabelas criadas:

- `peers`
  - `id`, `mac` (UNIQUE), `name`, `description`, `created_at`, `updated_at`

- `command_log`
  - `id`, `command`, `source`, `created_at`

- `command_log_output`
  - `id`, `log_id` (FK -> `command_log.id`), `output`, `created_at`

- `espnow_incoming_log`
  - `id`, `peer_id` (FK -> `peers.id`), `payload`, `payload_type`, `received_at`

- `espnow_outgoing_log`
  - `id`, `peer_id` (FK -> `peers.id`), `mac`, `payload`, `payload_type`, `delivered`, `sent_at`

- `boot_events`
  - `id`, `reason`, `boot_at`

- `kv_store`
  - `key`, `value`, `updated_at`

Indices runtime relevantes:
- `idx_peers_mac`
- `idx_incoming_peer`
- `idx_outgoing_peer`
- `idx_log_output_log_id`

### 6.4 Politica de logs

- cada comando shell executado em `ShellConfig::runLine()` pode ser persistido em:
  - `command_log`
  - `command_log_output`
- mensagens ESP-NOW recebidas sao persistidas em `espnow_incoming_log`
- mensagens ESP-NOW enviadas sao persistidas em `espnow_outgoing_log` com status de entrega
- boot do sistema registra evento em `boot_events`

## 7. Arquitetura ESP-NOW

### 7.1 Registro de peers

`EspNowManager` mantem array local com limite:
- `MAX_DEVICES = 16`

Cada peer tem:
- MAC (6 bytes)
- `name`
- `description`

### 7.2 Payload padrao

Struct de mensagem usada:

```cpp
struct message {
  uint32_t timer;
  char msg[231];
  logType type;
};
```

### 7.3 RX assincrono

- callback de RX tenta enfileirar evento em fila FreeRTOS (`queueDepth` default 24)
- task dedicada processa a fila:
  - imprime payload
  - auto-add peer desconhecido (`peer-XXYY`)
  - grava no banco
- se fila lotar, incrementa contador de drops (`espnow rx dropped=...`)

## 8. Serial e UX de Operacao

- `ShellSerial` oferece input editavel com setas esquerda/direita e historico up/down
- `ShellOutput` prefixa linhas:
  - `! ` para erros
  - `> ` para respostas comuns
- `ShellConfig::printLine()` envia para:
  - serial
  - buffer de output para persistencia
  - LCD terminal (quando pronto)

No LCD:
- colorizacao por heuristica de texto (erro, aviso, sucesso)
- limpeza de prefixos de modulo como `[dongle]`, `[espnow]`, `[database]`
- preservacao de tags numericas como `[000]`, `[001]`

## 9. Build, Upload e Monitor

Configuracao principal em `platformio.ini`:
- ambiente: `tdongle-s3`
- upload/monitor: `COM5` @ `921600`
- C++17
- USB CDC habilitado no boot

Dependencias:
- `Adafruit GFX Library`
- `Adafruit ST7735 and ST7789 Library`
- `Sqlite3Esp32`
- `TinyShell` (repo Git)

Comandos uteis:

```bash
platformio run -e tdongle-s3
platformio run -e tdongle-s3 -t upload
platformio run -e tdongle-s3 -t monitor
```

Script auxiliar:
- `scripts/pio_warnings.py` aplica `-Wno-discarded-qualifiers` para compilacao C (sqlite3 upstream)

## 10. Estrutura do Repositorio

```text
include/
  config.h
  error_codes.h
  espnow_config.h
  shell_config.h
  shell_output.h
  startup_config.h
lib/
  DatabaseStore/
  DonglePeripherals/
  EspNowManager/
  LcdTerminal/
  ShellSerial/
src/
  main.cpp
  espnow_config.cpp
  shell_config.cpp
  shell_output.cpp
  startup_config.cpp
scripts/
  pio_warnings.py
test/
  test_tablelinker.cpp
platformio.ini
```

## 11. Testes

Arquivo de teste existente:
- `test/test_tablelinker.cpp`

Ele demonstra criacao de modulos/funcoes no ecossistema TinyShell/TableLinker e chamadas de funcoes de teste via serial.

## 12. Limitacoes Conhecidas

- ESP-NOW sem criptografia por default (`encrypt=false`)
- sem retry automatico de envio apos timeout de callback
- limite de peers em memoria: 16
- payload textual limitado a 231 bytes por mensagem
- RTC depende de ajuste manual no boot (sem RTC externo dedicado)
- fila RX pode perder pacotes sob alta carga (contador de dropped)
- database em arquivo local no SD (sujeito a falhas de cartao/contato)

## 13. Dicas Rapidas de Uso

- primeiro comando sugerido apos iniciar: `help -e`
- verificar SD/database: `dongle -sd_status` e `database -status`
- listar peers: `espnow -list`
- testar comando local simples: `dongle -ping`
- testar envio broadcast: `espnow -send_to 000, "dongle -ping"`

---

Se voce quiser, o proximo passo natural e adicionar neste README uma secao de "playbook de campo" com passos padrao de diagnostico (startup, SD, peer, envio, leitura de logs) para uso operacional diario.
