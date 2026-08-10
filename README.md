# T-Dongle S3 Firmware (t_dongle_develop)

Firmware para ESP32-S3 (LILYGO T-Dongle-S3) que transforma o dongle em um terminal
embarcado de operação/diagnóstico para uma malha de robôs via ESP-NOW:

- shell interativo via Serial USB, com histórico, edição in-line e autocompletar
- comunicação ESP-NOW (peers, envio unicast/broadcast, heartbeat de link, **execução remota de comando**)
- persistência SQLite em cartão SD (SD_MMC)
- saída em Serial + dashboard visual em grade no LCD ST7735
- controle de perifériros locais (LED RGB, LCD, SD, RTC)
- permissão em camadas estilo `sudo`, por identidade (serial ou peer ESP-NOW)

Consulte também [CONTRIBUTING.md](CONTRIBUTING.md) para o padrão de código e o processo
para alterar/estender o firmware.

## 1. Stack

- Framework Arduino via PlatformIO
- ESP32-S3 (`esp32-s3-devkitc-1`), C++17
- [TinyShell](https://github.com/AlisonTristao/TinyShell) — dispatch de módulos/comandos
- [bally_protocol](C:\git\bally_protocol) (`symlink://../bally_protocol`) — codec/fragmentação
  do Bally Telemetry Protocol v1, compartilhado com `bally_software`/`TraceView`
- SQLite (`Sqlite3Esp32`) sobre SD_MMC
- Adafruit GFX + ST7735 (LCD 160x80)

## 2. Hardware alvo: T-Dongle-S3

Todo o mapeamento de pinos fica centralizado em [include/config.h](include/config.h)
(`namespace BoardConfig`):

| Periférico | Pinos |
|---|---|
| LED RGB onboard (DI/CI) | GPIO40 / GPIO39 |
| LCD ST7735 (SPI bit-banged) | CS=4, SDA(MOSI)=3, SCL(SCLK)=5, DC=2, RST=1, BL=38 |
| SD_MMC | D0=14, D1=17, D2=18, D3=21, CLK=12, CMD=16 |
| Botão BOOT | GPIO0 |
| USB nativo | D-=19, D+=20 |

Observações de hardware já tratadas no código:
- calibração de offset do painel ST7735 (`TFT_COL_START=26`, `TFT_ROW_START=1`)
- correção de cor (bit-invert + swap R/B) aplicada em `ShellCommandSupport::lcdColorForLine`
- polaridade de backlight configurável em runtime (`dongle -lcd_bl_inv`)
- "PIN_TFT_SDA"/"PIN_TFT_SCL" são os nomes dos sinais MOSI/SCLK do SPI bit-banged do LCD —
  **não há I2C real neste projeto** (sem `Wire.h`, sem periférico I2C em uso)

A senha usada pelo `sudo` do shell (ver seção 7) também mora em `config.h`
(`BoardConfig::SUDO_PASSWORD`) — troque antes de gravar em um dongle real.

## 3. Arquitetura

```text
Aplicação
  src/main.cpp            -> so chama AppRuntime::begin()/tick()
  AppRuntime               -> dono de todos os objetos runtime, setup, loop, tasks FreeRTOS

Orquestração do shell
  ShellConfig              -> bind do contexto, registro de módulos, runLine() (chaining, alias,
                               dedup de histórico, persistência)
  ShellCommandSupport       -> Context compartilhado (shell/espNow/peripherals/lcd/database/io),
                               printLine/failWithCode/warnWithCode, parsing utilitário
  ShellAliases              -> tabela de atalhos ("es" = "espnow -send_to")

Módulos de comando (um por "modulo" do TinyShell)
  HelpCommands   (help)     DongleCommands (dongle)   EspNowCommands (espnow)
  DatabaseCommands (database)                          SudoCommands (sudo)

Serviços/domínio
  EspNowManager   -> registry de peers e envio/recepção de bytes crus (sem saber o que é BTP)
  BtpTransport    -> identidade BTP (source_id/boot_id), sequência, envio fragmentado,
                     envelope COMMAND_REQUEST/COMMAND_RESULT (namespace btp_command)
  ProtocolRouter  -> decode BTP + validação de CRC + reassembly compartilhado
                     (btp::Reassembler); puro C++, sem Arduino, testável em env:native
  DatabaseStore   -> schema SQLite, migrações, leituras/gravações
  DonglePeripherals -> LED, LCD (driver), SD
  LcdDashboard    -> dashboard em grade sobre DonglePeripherals
  SudoManager     -> elevação de permissão por identidade (RAM, reseta no boot)
  EspNowConfig    -> callbacks ESP-NOW, fila RX assíncrona, roteamento por MessageType,
                     filas priorizadas por tipo, heartbeat BTP, execução remota (COMMAND)

Plataforma
  ShellSerial (input não bloqueante) / ShellOutput (formatação) / StartupConfig (boot)
  Arduino/ESP-IDF: WiFi, esp_now, FreeRTOS, SD_MMC, time
  bally_protocol (lib externa, symlink://../bally_protocol): codec/fragmentação BTP v1
```

Grafo de dependências entre as libs (setas = "depende de"; auditado nesta revisão —
é um DAG, sem ciclos):

```mermaid
graph TD
    AppRuntime --> ShellConfig
    AppRuntime --> EspNowConfig
    AppRuntime --> StartupConfig
    AppRuntime --> BtpTransport
    EspNowConfig --> ShellConfig
    EspNowConfig --> DatabaseStore
    EspNowConfig --> LcdDashboard
    EspNowConfig --> BtpTransport
    EspNowConfig --> ProtocolRouter
    ShellConfig --> DongleCommands
    ShellConfig --> EspNowCommands
    ShellConfig --> DatabaseCommands
    ShellConfig --> SudoCommands
    ShellConfig --> HelpCommands
    ShellConfig --> ShellAliases
    DongleCommands --> ShellCommandSupport
    EspNowCommands --> ShellCommandSupport
    EspNowCommands --> BtpTransport
    DatabaseCommands --> ShellCommandSupport
    SudoCommands --> ShellCommandSupport
    HelpCommands --> ShellCommandSupport
    DongleCommands --> SudoManager
    DatabaseCommands --> SudoManager
    SudoCommands --> SudoManager
    HelpCommands --> ShellAliases
    ShellCommandSupport --> EspNowManager
    ShellCommandSupport --> DonglePeripherals
    ShellCommandSupport --> LcdDashboard
    ShellCommandSupport --> DatabaseStore
    DatabaseStore --> EspNowManager
    LcdDashboard --> DonglePeripherals
    StartupConfig --> DonglePeripherals
```

`BtpTransport` e `ProtocolRouter` entram como serviços de domínio (nível 5), lado a lado
com `EspNowManager`, mas **sem incluir `EspNowManager.h`**: como tanto `EspNowConfig`
(nível 5, mas acima na cadeia de RX) quanto `EspNowCommands` (nível 3) precisam enviar
frames BTP, uma dependência direta de `BtpTransport` em `EspNowManager` fecharia um ciclo
via `EspNowConfig --> ShellConfig --> EspNowCommands`. A saída: `BtpTransport::sendLogical`/
`sendLogicalWithStatus` recebem um callback de envio (`SendFn`/`SendWithStatusFn`) em vez do
`EspNowManager&` direto; cada chamador (`EspNowConfig.cpp`, `EspNowCommands.cpp`) define um
adaptador de uma linha que chama o `EspNowManager` real. `ProtocolRouter` não depende de
nada do projeto além de `bally_protocol`. Como nenhum dos dois toca Arduino/FreeRTOS, ambos
compilam e têm testes rodando sob `env:native` (`test/test_protocol_router`).

Ver [CONTRIBUTING.md § Arquitetura e camadas](CONTRIBUTING.md#arquitetura-e-camadas) para
a regra de quando um módulo novo deve entrar em `Context` ou pode ser incluído direto.

## 4. Fluxo de execução

`src/main.cpp` só chama `AppRuntime::begin()` (setup) e `AppRuntime::tick()` (loop).

### `AppRuntime::begin()`

1. `BoardConfig::initBoardPins(false)` — pinos em estado seguro (LCD apagado)
2. inicia `ShellSerial` na baudrate de `platformio.ini` (`BAUDRATE`)
3. aguarda monitor serial conectar, animando o LED (`StartupConfig`)
4. inicia SD, imprime MAC Wi-Fi, pede/ajusta data-hora do RTC local
5. conecta callbacks ESP-NOW e habilita a fila RX assíncrona (`EspNowConfig`)
6. `espNowManager_.begin(...)`, depois `databaseStore_.begin(...)` + `logBootEvent("power_on")`
7. sobe as tasks FreeRTOS (RX worker, heartbeat worker)
8. `ShellConfig::bind(...)` + `ShellConfig::registerDefaultModules()`
9. restaura histórico do shell a partir do banco (`readRecentCommands`)

### `AppRuntime::tick()` (chamado a cada `loop()`)

1. processa avisos assíncronos e saída ESP-NOW pendente
2. `lcdDashboard_.tick()` — redesenha tiles que mudaram (throttled internamente)
3. lê input da serial e roda o comando (`ShellConfig::runLine`)
4. `delay(1)` cooperativo

### Tasks FreeRTOS (fora do loop principal)

| Task | Função |
|---|---|
| `espnow_rx` | drena a fila RX e processa cada mensagem (`EspNowConfig::processRxMessage`) |
| `espnow_heartbeat` | a cada `HEARTBEAT_INTERVAL_MS`, faz ping no último peer que mandou dado real |

## 5. Shell (TinyShell)

Sintaxe: `<modulo> -<comando> [args]`. Comandos podem ser encadeados com `;`
(`dongle -ping; dongle -clock`), cada um logado separadamente. Aliases (tabela em
`ShellAliases`) substituem a primeira palavra digitada: `es 1, "dongle -ping"` vira
`espnow -send_to 1, "dongle -ping"`.

### 5.1 `help`

| Comando | Descrição |
|---|---|
| `help -h` | lista módulos |
| `help -l <modulo>` | lista comandos de um módulo |
| `help -e` | exemplos e dicas rápidas |

### 5.2 `dongle` — comandos locais

| Comando | Sintaxe | Descrição |
|---|---|---|
| `ping` | `dongle -ping` | teste local (`pong`) |
| `clock` | `dongle -clock` | data/hora do RTC local |
| `set_clock` | `dongle -set_clock "YYYY-MM-DD HH:MM:SS"` | ajusta RTC |
| `run` | `dongle -run "<comando>"` | executa outro comando localmente (bypassa `runLine`) |
| `led` / `led_off` | `dongle -led r, g, b` | LED RGB (0-255) / desliga |
| `lcd` / `lcd_clear` | `dongle -lcd "texto"` | escreve/limpa banner do LCD |
| `lcd_rot` / `lcd_rot_get` | `dongle -lcd_rot 0..3` | rotação do painel |
| `lcd_bl` / `lcd_bl_inv` | `dongle -lcd_bl 0\|1` | backlight e polaridade |
| `lcd_reinit` | `dongle -lcd_reinit` | reinicializa driver do LCD |
| `sd_init` / `sd_status` | | reinicia SD / mostra status |
| `sd_ls` / `sd_cat` | `dongle -sd_ls <path>` | lista dir / imprime arquivo texto (cap. 4KB) |
| `sd_write` / `sd_append` | `dongle -sd_write <arquivo>, <texto>` | grava/acrescenta em arquivo |
| `sd_rm` / `sd_mkdir` | `dongle -sd_rm <arquivo>` | remove arquivo / cria diretório |
| `sd_wipe` | `dongle -sd_wipe` | apaga TUDO do SD e recria o banco — **requer `sudo -login`** |
| `run_script` | `dongle -run_script <arquivo>` | roda um comando por linha de um arquivo no SD |
| `history` | `dongle -history 20` | reimprime comandos recentes do histórico persistido |
| `info` | `dongle -info` | chip/heap/flash/uptime/MAC |
| `reboot` | `dongle -reboot` | reinicia o ESP32 |

### 5.3 `espnow` — peers e mensagens

| Comando | Sintaxe | Descrição |
|---|---|---|
| `list` | `espnow -list` | lista peers cadastrados, com indicação de `boot_id` conhecido/desconhecido |
| `add` / `remove` / `remove_mac` / `update` | `espnow -add "MAC", "nome", "desc"` | gestão de peers |
| `send_to` | `espnow -send_to 1, "dongle -clock"` | envia COMMAND_REQUEST pro peer de índice 1 |
| `send_all` | `espnow -send_all "<comando>"` | envia COMMAND_REQUEST pra cada peer com `boot_id` conhecido |

`send_to`/`send_all` enviam um frame BTP `COMMAND`/`COMMAND_REQUEST` (ação "shell") — isso faz
o **peer receptor executar o comando remotamente** e responder com um `COMMAND_RESULT`,
exibido como `[cmd_result] ...` (ver seção 7). Diferença importante em relação ao antigo
`CMDO`: um `COMMAND_REQUEST` precisa do `boot_id` atual do peer, que só é conhecido depois
de já termos recebido alguma mensagem dele (sem handshake HELLO ainda, tópico 16) — por
isso o alias de broadcast `000` foi removido e `send_to`/`send_all` falham com
`PEER_BOOT_UNKNOWN` para um peer do qual nunca recebemos nada.

### 5.4 `database` — SQLite no SD

| Comando | Sintaxe | Descrição |
|---|---|---|
| `init` / `status` / `tables` | | abre banco / status geral / lista tabelas |
| `read` | `database -read peers, 20` | leitura simples com limite |
| `logs` | `database -logs 20` | histórico de comandos + saída |
| `espnow_history` | `database -espnow_history 30` | histórico TX/RX com status |
| `backup` | `database -backup` | snapshot do banco em `/database/backups` |
| `count` | `database -count <tabela>` | conta linhas |
| `delete` | `database -delete <tabela>, <condição SQL>` | remove linhas (condição obrigatória) |
| `vacuum` | `database -vacuum` | compacta o arquivo do banco |
| `export` | `database -export <tabela>` | dump da tabela em CSV (`/database/exports/`) |
| `exec` / `exec_nolog` | `database -exec "SELECT ..."` | SQL livre (com/sem log em `command_log`) |
| `drop` | `database -drop <tabela>` | remove tabela — **requer `sudo -login`** |
| `rebuild` | `database -rebuild` | recria banco do zero via bootstrap — **requer `sudo -login`** |
| `clear_logs` | `database -clear_logs` | apaga `command_log`/saídas/histórico ESP-NOW — **requer `sudo -login`** |

### 5.5 `sudo` — permissão elevada

| Comando | Descrição |
|---|---|
| `sudo -login <senha>` | eleva o usuário atual (quem digitou, local ou remoto) |
| `sudo -logout` | revoga a elevação do usuário atual |
| `sudo -status` | mostra se o usuário atual está elevado, e quantos ao todo |

Ver seção 7 para o modelo completo de identidade/permissão.

## 6. Banco de dados (SQLite)

- diretório: `/database`; bootstrap: `/database/bootstrap.sql`; arquivo: `/database/dongle.db`
  (caminho SQLite: `/sdcard/database/dongle.db`)
- backups em `/database/backups/`, exports CSV em `/database/exports/`

`DatabaseStore::begin()`: garante `/database`, cria `bootstrap.sql` se ausente, abre o banco,
aplica bootstrap + migrações idempotentes, garante o peer virtual `000` (broadcast,
`FF:FF:FF:FF:FF:FF`) e carrega peers reais para o `EspNowManager`.

Tabelas:

| Tabela | Campos principais |
|---|---|
| `peers` | `id`, `mac` (UNIQUE), `name`, `description`, `created_at`, `updated_at` |
| `command_log` | `id`, `command`, `source` (`serial`/`espnow`), `created_at` |
| `command_log_output` | `id`, `log_id` → `command_log.id`, `output`, `created_at` |
| `espnow_outgoing_log` | `id`, `peer_id` → `peers.id`, `mac`, `payload`, `payload_type`, `delivered`, `sent_at` (`payload_type` é o `btp::MessageType` numérico; `payload` é um preview textual, não o envelope BTP) |
| `boot_events` | `id`, `reason`, `boot_at` |
| `kv_store` | `key`, `value`, `updated_at` |

Cada comando rodado via `ShellConfig::runLine()` pode virar uma linha em `command_log` +
`command_log_output` — exceto: comandos idênticos ao anterior em sequência (dedup, ver
`g_lastCommandLine` em `ShellConfig.cpp`) e `database -exec_nolog`.

## 7. ESP-NOW: BTP, peers, heartbeat, execução remota e permissão

### 7.1 Envelope BTP v1 (`bally_protocol/include/btp/codec.hpp`)

Todo datagrama trocado com um peer é um frame BTP v1 (fonte canônica em
`C:\git\bally_protocol`, integrada aqui via `lib_deps = symlink://../bally_protocol`):
`btp::Header` (`type`, `flags`, `source_id`, `boot_id`, `sequence`, `timestamp_us`,
`object_id`, `fragment_index`, `fragment_count`) + payload + CRC-32. `EspNowManager` só
transporta bytes crus; `ProtocolRouter` decodifica, valida o CRC e faz reassembly
(`btp::Reassembler`) antes de rotear por `MessageType` (`Telemetry`/`Log`/`Command`/
`Terminal`/`Control`) para uma fila própria em `EspNowConfig` (uma por tipo, com contador de
drop). `source_id`/`boot_id`/`sequence`/`timestamp_us` da origem chegam intactos ao próximo
estágio; o horário de chegada é só metadado local (`RoutedMessage::arrivalMs`), nunca
sobrescreve `timestamp_us`. `TELEMETRY` é passada adiante como bytes crus (sem `String`,
`snprintf` ou qualquer formatação) — ainda não tem consumidor nesta versão (isso é dos
tópicos 13/14, TraceView); `TERMINAL`/`CONTROL` (HELLO/MANIFEST/STATUS) também não têm
consumidor ainda (tópicos 16/19) e são apenas drenados/descartados, liberando a fila.

`HELLO`/`MANIFEST` **não são `MessageType` novos**: no wire format já congelado eles são
`object_id`s dentro de `CONTROL` (`0x0001`=`HELLO`, `0x0004`=`MANIFEST_DATA`, ver
`bally_protocol/docs/COMMANDS_AND_ACTIONS.md` seção 3.2) — não havia motivo pra inventar
valores de enum novos no codec canônico só pra este tópico.

### 7.2 Identidade deste dongle (`BtpTransport`)

No boot, `AppRuntime::begin()` chama `BtpTransport::configureIdentity(source_id, boot_id)`:
`source_id` é derivado do próprio MAC Wi-Fi (mesma fórmula usada em `bally_software`, sem
handshake); `boot_id` é um valor aleatório não nulo por boot (`esp_random()`) — não há
HELLO/MANIFEST ainda (tópico 16) pra anunciar/persistir isso, e nada aqui depende de
sobreviver a um reboot.

Como o dongle não tem como descobrir o `boot_id` de um peer sem HELLO, `BtpTransport`
mantém uma tabela pequena (`rememberPeer`/`lookupPeer`) do último `(source_id, boot_id)`
visto por MAC, atualizada a cada mensagem BTP válida recebida. `espnow -send_to`/`-send_all`
só conseguem endereçar um `COMMAND_REQUEST` pra um peer que já apareceu nessa tabela.

### 7.3 Heartbeat (indicador LINK no LCD)

A cada `HEARTBEAT_INTERVAL_MS` (`EspNowConfig.h`), a task `espnow_heartbeat` manda um frame
BTP `CONTROL`/`STATUS` (`object_id=0x0009`, payload vazio — "publicação espontânea, sem
resposta" por definição no wire format) para o **último peer de onde chegou dado real** (não
é broadcast pra todos, é sempre o mais recente). O resultado (ACK de entrega, não o payload)
atualiza o tile `LINK` do dashboard.

### 7.4 Execução remota de comando (`COMMAND`/`COMMAND_REQUEST`)

`espnow -send_to`/`espnow -send_all` enviam um `COMMAND_REQUEST` (ação "shell",
`action_id=1`/`action_version=1`, mesma convenção usada em `bally_software`). Ao receber:

1. `EspNowConfig` só segue adiante se o MAC de origem **já está cadastrado no registry de
   peers** — MAC desconhecido é ignorado (a mensagem ainda é roteada/loga normalmente, só
   não dispara execução).
2. O envelope é validado (`BtpTransport::btp_command::parse_request`): `target_source_id`/
   `target_boot_id` precisam bater com a identidade deste boot; addressed-to-outro-peer é
   silenciosamente ignorado, envelope malformado responde `REJECTED`.
3. O texto do comando roda via `ShellConfig::runLine(texto, "espnow", &fullOutput, userId)`,
   onde `userId = "espnow:<MAC>"` — **cada peer é uma identidade própria** para fins de sudo.
4. A saída completa volta ao mesmo MAC como `COMMAND_RESULT` (`SUCCESS`/mensagem = saída do
   shell) — nunca como um novo `COMMAND_REQUEST`, então não há risco de loop resposta-gera-
   resposta por construção do protocolo (tipos diferentes de objeto).
5. Fragmentação agora é transparente (via `ProtocolRouter`/`btp::Reassembler`): um comando
   remoto pode ocupar vários pacotes ESP-NOW (limite prático ~512 bytes de texto).

Exemplo: um robô manda `dongle -clock` via `send_to` para buscar a hora; o dongle executa e
responde automaticamente com o horário, exibido aqui como `[cmd_result] ...`.

### 7.5 Permissão (`sudo`)

Modelo de "usuário" análogo ao `sudo` do Linux, implementado em `SudoManager`:

- cada identidade que fala com o shell é uma string livre: `"serial"` para o console
  USB/UART, `"espnow:<MAC>"` para cada peer registrado — extensível pra transportes futuros
  (ex.: `"mqtt:<client_id>"`) sem mudar o `SudoManager`.
- `sudo -login <senha>` eleva **a identidade que rodou o comando**, comparando com
  `BoardConfig::SUDO_PASSWORD` (constante compilada, ver `include/config.h`).
- a elevação vive só em RAM (`std::set` de identidades elevadas dentro de `SudoManager.cpp`)
  e **sempre reseta no boot** — não há persistência em SD/banco de propósito.
- comandos destrutivos (`dongle -sd_wipe`, `database -drop`, `database -rebuild`,
  `database -clear_logs`) checam `SudoManager::isElevated(currentUserId())` antes de agir.

**Modelo de confiança para execução remota (decisão deliberada, preservada na migração pra
BTP):** qualquer peer já cadastrado no registry pode disparar qualquer comando do shell via
`COMMAND`/`COMMAND_REQUEST`, sem restrição de conteúdo — inclusive `sudo -login`. A
identidade sudo continua vinculada ao **peer autorizado por MAC** (`espnow:<MAC>`), nunca ao
conteúdo do comando ou a algo do envelope BTP em si. Ou seja, a barreira real contra um peer
malicioso é (a) não deixar MACs não confiáveis virarem peers cadastrados, e (b) a senha do
`sudo` para os comandos destrutivos. Um peer cadastrado que souber a senha tem controle total
do dongle.

## 8. LCD Dashboard (`LcdDashboard`)

Substitui o antigo terminal de rolagem (o texto já existe na serial e no banco). Layout
160x80, atualizado em `tick()` a cada loop mas com redesenho throttled
(`REFRESH_INTERVAL_MS`):

```text
[ banner de mensagem/relógio                     ]
[  LINK  |   RX   |   TX   ]
[      STATE (2x largura)  |   ERR   ]
```

- **LINK**: resultado do heartbeat (verde=entregue, vermelho=falhou)
- **RX** / **TX**: pulso a cada mensagem recebida/enviada (`PULSE_HOLD_MS`)
- **STATE**: último `state changed: OLD -> NEW` visto em uma mensagem RX (extraído por
  `tryExtractRobotState` em `EspNowConfig.cpp`)
- **ERR**: contador de pacotes ESP-NOW descartados/sobrescritos por fila cheia

## 9. Serial e UX

- `ShellSerial`: input não bloqueante, edição in-line (setas, backspace), histórico
  navegável (`ESC[A`/`ESC[B`), restaurado do banco no boot
- `ShellOutput`: prefixa linhas (`! ` erro, `> ` resposta comum)
- `ShellCommandSupport::printLine()` manda a mesma linha para: serial, buffer de
  persistência (`command_log_output`) e banner do LCD (com cor por heurística de texto)

## 10. Build, upload e monitor

Config principal: [platformio.ini](platformio.ini) — ambiente `tdongle-s3`,
`COM5 @ 921600`, C++17, USB CDC habilitado no boot.

```bash
platformio run -e tdongle-s3
platformio run -e tdongle-s3 -t upload
platformio run -e tdongle-s3 -t monitor
```

`scripts/pio_warnings.py` aplica `-Wno-discarded-qualifiers` na compilação C do
`Sqlite3Esp32` (upstream gera esse warning, não é nosso código).

Há também um ambiente host-only, `env:native`, que roda `ProtocolRouter`/`BtpTransport`
contra os vetores canônicos de `C:\git\bally_protocol\test-vectors\v1` sem precisar de
hardware (mesmo padrão de `bally_software`, exige `bally_protocol` como diretório irmão):

```bash
platformio test -e native
```

## 11. Estrutura do repositório

```text
include/
  config.h              # pinos + BoardConfig::SUDO_PASSWORD
  error_codes.h         # AppError::Code (um range por subsistema)
lib/
  AppRuntime/            # dono dos objetos runtime, setup/loop, tasks FreeRTOS
  ShellConfig/           # bind + registro de módulos + runLine()
  ShellCommandSupport/   # Context compartilhado + helpers de wrapper
  ShellAliases/          # tabela de atalhos de comando
  DongleCommands/ EspNowCommands/ DatabaseCommands/ SudoCommands/ HelpCommands/
  EspNowManager/          # registry de peers + envio/recepção de bytes crus (sem BTP)
  BtpTransport/           # identidade BTP, sequência, envio fragmentado, COMMAND envelope
  ProtocolRouter/         # decode BTP + CRC + reassembly compartilhado (puro C++)
  EspNowConfig/           # callbacks ESP-NOW, filas priorizadas por tipo, heartbeat, exec remota
  DatabaseStore/                 # schema SQLite e leituras/gravações
  DonglePeripherals/ LcdDashboard/  # LED/LCD/SD e dashboard em grade
  SudoManager/                   # elevação de permissão por identidade
  ShellSerial/ ShellOutput/      # input/format de terminal
  StartupConfig/                 # sequência de boot
src/
  main.cpp
scripts/
  pio_warnings.py
  native_static.py      # so para env:native (evita runtime MinGW solto no PATH)
test/
  test_tablelinker/      # demo antiga do TinyShell, hardware-only
  test_protocol_router/  # BTP vs. vetores canônicos, roda em env:native
platformio.ini
CONTRIBUTING.md
```

## 12. Limitações conhecidas

- ESP-NOW sem criptografia por padrão (`encrypt=false`)
- sem retry automático de envio após timeout de callback
- limite de peers em memória: `MAX_DEVICES = 16`
- payload reassemblado limitado a `ProtocolRouter::kMaxPayloadSize` (600 bytes; cobre um
  `COMMAND_REQUEST` de texto completo com folga) — mensagem maior que isso é rejeitada
  como `MessageTooLarge` antes de virar `RoutedMessage`
- `espnow -send_to`/`-send_all` só conseguem endereçar um peer do qual já recebemos alguma
  mensagem BTP (não há handshake HELLO ainda, tópico 16, pra descobrir `boot_id` de outra
  forma) — o antigo alias de broadcast `000` foi removido por isso
- RTC depende de ajuste manual no boot (sem RTC externo dedicado)
- fila RX pode perder pacotes sob alta carga (contador de dropped exposto no tile `ERR`)
- `TELEMETRY`/`TERMINAL`/`CONTROL` roteados ainda não têm consumidor nesta versão (ficam
  para os tópicos 13/14/16/19) — são drenados e descartados, só contabilizados
- banco em arquivo local no SD (sujeito a falhas de cartão/contato)
- senha do `sudo` é uma constante compilada — trocar exige reflash; qualquer peer
  cadastrado que souber a senha tem controle total do dongle (ver seção 7.5)
- bug intermitente conhecido, não resolvido: o LCD ocasionalmente "apaga" a tela por um
  instante enquanto a serial continua atualizando normalmente; suspeita não confirmada é
  contenção entre o SPI bit-banged do LCD e a task de rádio WiFi/ESP-NOW

## 13. Dicas rápidas de uso

- primeiro comando sugerido: `help -e`
- verificar SD/banco: `dongle -sd_status` e `database -status`
- listar peers: `espnow -list`
- teste local: `dongle -ping`
- teste remoto (depois que o peer já tiver mandado algo, ver seção 7.2): `espnow -send_to 1, "dongle -ping"`
- elevar permissão antes de um comando destrutivo: `sudo -login <senha>`
