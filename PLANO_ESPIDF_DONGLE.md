# Plano — migrar o firmware do dongle de Arduino para ESP-IDF

Objetivo: pôr o `bally_dongle` em **ESP-IDF puro via PlatformIO** (`framework = espidf`),
seguindo o mesmo formato do `bally_OS` — mesmo layout de projeto (`CMakeLists.txt` na raiz,
`components/btp`, `sdkconfig.defaults`, `partitions.csv`), mesma forma de trazer o BTP, e
`lib/*` compilando sem a camada Arduino. O `[env:native]` continua igual (já é
IDF-agnóstico).

Não é uma reescrita funcional: o comportamento (shell, BTP SerialMux, ESP-NOW, LCD, SD,
SQLite, HID) continua o mesmo. O que muda é a base: `Arduino.h`/`WiFi.h`/`USB.h`/`SD_MMC`/
`Adafruit_*`/`Preferences` saem, entram as APIs nativas do IDF.

---

## 1. Ponto de partida

| | bally_dongle (hoje) | bally_OS (alvo do formato) |
|---|---|---|
| Framework PlatformIO | `arduino` | `espidf` |
| ESP-IDF efetivo | 4.4 (via arduino-esp32) | 6.0.1 (`espressif32` 7.x → `framework-espidf` 6.0.1) |
| Entrypoint | `setup()` / `loop()` | `extern "C" void app_main()` |
| BTP | `lib_deps` git tag `#v2.0.0` | `components/btp/CMakeLists.txt` (fontes de `../BTP`, `REQUIRES mbedtls`) |
| Wi-Fi/ESP-NOW | `WiFi.mode(WIFI_STA)` | `nvs_flash_init` + `esp_netif_init` + `esp_event_loop_create_default` + `esp_wifi_init/set_mode/start` |
| USB | arduino-esp32 TinyUSB (`ARDUINO_USB_MODE=0`), `USBCDC` + `USBHIDVendor` | `esp_tinyusb` (só MSC hoje) |
| Armazenamento | `SD_MMC` (Arduino FS) | `esp_vfs_fat` + `sdmmc_host` (SPI no OS; **SDMMC no dongle**) |
| NVS | `Preferences.h` | `nvs_*` direto (`lib/KeyStore`) |
| Config de build | `build_flags`/`build_unflags` no `.ini` | `.ini` mínimo + `sdkconfig.defaults` + `sdkconfig.<board>` |

A plataforma PlatformIO já instalada (`espressif32@7.0.1`) entrega `framework-espidf`
6.0.1 — o mesmo que o `bally_OS` usa. Sem pino de versão novo: basta trocar `framework`.

---

## 2. Inventário de acoplamento (o que cada lib exige)

### Grupo A — já portáveis, zero mudança
Já compilam em `env:native` sem Arduino. Só precisam continuar no grafo de build:

`BtpTransport`, `ClockText`, `DonglePublisher`, `EspNowCommands`, `HelpCommands`,
`HubRegistry`, `HubRelay`, `ManifestCache`, `ProtocolRouter`, `RadioSeal`,
`RadioTxScheduler`, `SerialSession`, `ShellAliases`, `SubscriptionRegistry`,
`SudoCommands`, `SudoManager`.

### Grupo B — acoplamento leve (refactor mecânico)
Trocam `Stream&`/`String`/`Serial`/`millis()` por equivalentes. Sem driver novo.

| Lib | O que toca | Ação |
|---|---|---|
| `ShellOutput` | assina tudo como `Stream& io`, usa `String` | trocar `Stream&` por `ByteIO&` (§4) ou por um writer `std::function<void(const char*,size_t)>`; `String` → `std::string` |
| `ShellConfig`, `ShellCommandSupport` | `<Arduino.h>`, `String`, `context().io` (`Stream*`) | `String` → `std::string`; `io` vira `ByteIO*` |
| `DongleCommands` | `<WiFi.h>` p/ `WiFi.macAddress()`, `millis()` | `esp_read_mac(mac, ESP_MAC_WIFI_STA)`; `millis()` helper (§4) |
| `DatabaseCommands` | `millis()`, `String` | helper + `std::string` |
| `StartupConfig` | `Serial`, chama a API do LCD (Adafruit) | segue a decisão de LCD (§6); `Serial` → `ByteIO` |
| `DongleKeyStore` | `#if defined(ARDUINO)` → `Preferences` | ramo NVS nativo (`nvs_open`/`nvs_get_blob`/`nvs_set_blob`), copiar padrão de `bally_OS/lib/KeyStore` |
| `SerialMux` | `Stream* g_io` (`->write/read/available`) | `ByteIO* g_io` (§4) — é o único ponto de I/O; ~5 call sites |
| `EspNowConfig` | `Stream* g_io` p/ logs | `ByteIO* g_io` |
| `config.h` (`BoardConfig`) | `pinMode`/`digitalWrite` em `initBoardPins()` | `gpio_config()` / `gpio_set_level()`; `<Arduino.h>` → `<driver/gpio.h>` |
| `src/main.cpp` + `AppRuntime` | `setup/loop`, `USB.*`, `ESP.getFreeHeap`, `esp_ota_get_app_description`, `WiFi.*`, `delay` | ver §5 |

### Grupo C — driver reescrito

| Lib | Hoje | Alvo IDF |
|---|---|---|
| `EspNowManager` | `WiFi.mode(WIFI_STA); WiFi.disconnect()` antes de `esp_now_init()` | bring-up Wi-Fi nativo (copiar `bally_OS/utils/BallyRobot/BallyRobot.cpp:355-505`). O resto (`esp_now_*`, filas FreeRTOS, TX scheduler) **já é IDF** — não mexe |
| `DonglePeripherals` — SD | `SD_MMC.begin("/sdcard", ...)` (barramento **SDMMC** 1/4-bit, pinos custom, retries de clock) | `esp_vfs_fat_sdmmc_mount()` com `sdmmc_host_t = SDMMC_HOST_DEFAULT()` + `sdmmc_slot_config_t` (GPIO matrix p/ os pinos do T-Dongle) — manter a lógica de fallback 4→1 bit e clock reduzido |
| `DonglePeripherals` — LED | APA102 bit-bang (`digitalWrite` DI/CI) | `gpio_set_level()` direto (mesma lógica, 8 linhas) — ou `SPI` se quiser limpar |
| `DonglePeripherals` — LCD | `Adafruit_ST7735` + `Adafruit_GFX` + `SPI` Arduino | **decisão §6** |
| `LcdDashboard` | API `Adafruit_GFX` (`fillRect`, `drawFastHLine`, `setCursor/print`, `getTextBounds`, `color565`) | segue §6 — se mantiver Adafruit_GFX como componente, é quase zero diff |
| `DatabaseStore` | `<SD_MMC.h>` + `<sqlite3.h>` (`Sqlite3Esp32`) | VFS vem do `esp_vfs_fat`; sqlite via **decisão §6**. `String` → `std::string` |
| `UsbHidMux` + USB em `AppRuntime` | `USB.h`, `USBCDC` (=`Serial`), `USBHIDVendor` | `esp_tinyusb` com descritor **composto CDC-ACM + HID vendor** — **maior risco §7** |

---

## 3. Estrutura de build a criar (espelhar bally_OS)

```
bally_dongle/
├── CMakeLists.txt              # NOVO — copiar de bally_OS, trocar project()
├── partitions.csv             # NOVO — ver §3.3
├── sdkconfig.defaults         # NOVO — ver §3.2
├── sdkconfig.tdongle-s3       # gerado no 1º build, versionar
├── components/
│   └── btp/CMakeLists.txt     # NOVO — copiar de bally_OS quase literal
├── src/
│   ├── CMakeLists.txt         # NOVO — idf_component_register
│   ├── idf_component.yml      # NOVO — esp_tinyusb (+ sqlite se via registry)
│   └── main.cpp               # reescrito (app_main)
└── platformio.ini             # editado
```

### 3.1 `platformio.ini` (env de firmware)

```ini
[env:tdongle-s3]
platform            = espressif32
board               = esp32-s3-devkitc-1
framework           = espidf
board_build.f_cpu   = 240000000L
board_build.flash_size   = 16MB
board_build.partitions   = partitions.csv
monitor_speed       = 115200          ; console IDF real (não é mais cosmético)
monitor_filters     = esp32_exception_decoder, direct
upload_port         = COM5
monitor_port        = COM5

extra_scripts =
    pre:scripts/check_channel_contract.py
    pre:scripts/check_espnow_tx_owner.py
    pre:scripts/inject_git_rev.py
    ; + eventual patch do esp_tinyusb (§7)

build_flags =
    -std=gnu++2a
    -fexceptions
    -I include
    -DDONGLE_...                       ; migrar os -D que ainda fazem sentido

build_unflags =
    -std=gnu++11
    -fno-exceptions
```

Flags que **saem** (eram da camada Arduino): `ARDUINO_USB_MODE`, `ARDUINO_USB_CDC_ON_BOOT`,
`ARDUINO_RUNNING_CORE`, `BAUDRATE` (USB-CDC não tem baud), `DONGLE_USB_NO_AUTORESET`
(reimplementado no callback do TinyUSB — §7).
Flags que **ficam** (viram `-D` normais ou `sdkconfig`): `RX_ASYNC_QUEUE_DEPTH`,
`DIAG_BOOT`, `TINYSHELL_COLOR`.

O `[env:native]` **não muda** — continua `platform = native`, `symlink://../BTP`,
`symlink://../TinyShell`.

### 3.2 `sdkconfig.defaults`

Base do bally_OS, menos PSRAM (o T-Dongle-S3 não tem — confirmado no tópico 33):

```
CONFIG_FREERTOS_HZ=1000
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_COMPILER_CXX_EXCEPTIONS=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192

# SEM CONFIG_SPIRAM (dongle não tem PSRAM)

# TinyUSB — device composto CDC + HID (§7)
CONFIG_TINYUSB_CDC_ENABLED=y
CONFIG_TINYUSB_CDC_RX_BUFSIZE=1024        # equivale ao setRxBufferSize(1024)
CONFIG_TINYUSB_HID_COUNT=1
CONFIG_TINYUSB_DESC_PRODUCT_STRING="Bally Dongle"
CONFIG_TINYUSB_DESC_MANUFACTURER_STRING="Bally"

# console: UART0 no header, OU USB-Serial/JTAG — decisão §7
CONFIG_ESP_CONSOLE_UART_DEFAULT=y

# OTA / rollback — só se for adotar OTA agora (§3.3)
# CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
```

### 3.3 `partitions.csv`

O dongle hoje **não faz OTA** (não há `OTAUpdater`). Decisão em §8. Ponto de partida sem
OTA, com espaço para o SQLite/histórico ficarem no SD (não em SPIFFS):

```
# Name,   Type, SubType, Offset,  Size
nvs,      data, nvs,     ,        0x6000
phy_init, data, phy,     ,        0x1000
factory,  app,  factory, ,        0x400000
```

Se adotar OTA (espelhar bally_OS): `otadata` + `ota_0`/`ota_1` de `0x7F0000`.

### 3.4 `components/btp/CMakeLists.txt`

Cópia quase literal do bally_OS. Só muda o número de níveis do `../` até `../../BTP`
(mesma profundidade: `components/btp/` → `${CMAKE_CURRENT_LIST_DIR}/../../../BTP`). Mantém
`REQUIRES mbedtls` e a checagem de `EXISTS`. Remove a entrada `BTP.git#v2.0.0` de
`lib_deps`.

### 3.5 `src/CMakeLists.txt`

```cmake
file(GLOB_RECURSE app_sources "*.cpp" "*.c")
idf_component_register(SRCS ${app_sources}
                       INCLUDE_DIRS "." "../include")
```

O PlatformIO descobre `lib/*` sozinho e injeta como componentes; o que ele não fizer bem
sob `espidf` (LDF é mais fraco aqui) resolve-se movendo libs problemáticas para
`components/` com `CMakeLists.txt` próprio — mesma saída que o bally_OS usou para o BTP.

---

## 4. Camada de compatibilidade mínima

Criar **um** header interno (`include/compat.h` ou `lib/Compat/`) para não espalhar
`#ifdef` pelo código:

```cpp
#pragma once
#include <cstdint>
#include <cstddef>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

inline uint32_t millis() { return (uint32_t)(esp_timer_get_time() / 1000); }
inline void     delay(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

// Substitui Stream& nas assinaturas de ShellOutput / SerialMux / EspNowConfig.
struct ByteIO {
    virtual size_t write(const uint8_t* data, size_t len) = 0;
    virtual int    read() = 0;          // -1 se vazio
    virtual int    available() = 0;
    virtual explicit operator bool() const = 0;   // DTR / porta aberta
protected:
    ~ByteIO() = default;
};
```

`String` → `std::string` é troca direta na maioria dos sites (`.isEmpty()`→`.empty()`,
`.indexOf`→`.find`, `.substring`→`.substr`, concatenção com `+`). O `AppRuntime`
(`restoreShellHistoryFromDatabase`) e o `DatabaseStore` são os que mais usam `String` —
já retornam texto que pode passar a `std::string` sem perda.

Implementações concretas de `ByteIO`:
- `ConsoleCdc` — sobre `tinyusb_cdcacm_*` (§7), ou sobre UART0 no começo (stub).
- Nos testes `env:native` já existe padrão equivalente (mock) — reaproveitar.

---

## 5. `main.cpp` + `AppRuntime`

### `src/main.cpp`

```cpp
#include "AppRuntime.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace { AppRuntime g_app; }

extern "C" void app_main() {
    g_app.begin();
    while (true) {
        g_app.tick();
        vTaskDelay(pdMS_TO_TICKS(1));   // era o delay(1) no fim de tick()
    }
}
```

Alternativa (mais no estilo bally_OS): `begin()` cria uma task `"app"` com stack
dimensionada e `app_main` faz `vTaskDelete(NULL)`. O modelo acima é o menor diff — mantém
`tick()` como está.

### Trocas pontuais no `AppRuntime.cpp`

| Hoje | IDF |
|---|---|
| `#include <WiFi.h>` / `WiFi.macAddress(mac)` | `esp_read_mac(mac, ESP_MAC_WIFI_STA)` |
| `#include <USB.h>` / `USB.productName(...)` | strings no `tinyusb_config_t` / `sdkconfig` (§7) |
| `#include <Esp.h>` / `ESP.getFreeHeap()` / `getMinFreeHeap()` | `esp_get_free_heap_size()` / `esp_get_minimum_free_heap_size()` |
| `esp_ota_get_app_description()` | `esp_app_get_description()` (IDF ≥ 5) — o comentário no código já previa |
| `Serial.begin()/setRxBufferSize()/enableReboot()` | init do `ConsoleCdc` (§7) |
| `Serial.print*` / `ShellOutput::printTagged(Serial, ...)` | passa a instância `ByteIO&` |
| `delay(2000)` sob `DIAG_BOOT` | `vTaskDelay` |
| `nvs` implícito do arduino-core | `nvs_flash_init()` explícito no início do `begin()` |
| `esp_random()` | igual (já é IDF) |

`buildDongleSourceInfo()` já usa `esp_chip_info`, `esp_ota_get_running_partition` — IDF
puro, só o acessor do app_desc muda.

---

## 6. Decisões que preciso confirmar antes de começar

### D1 — Stack gráfica do LCD (ST7735)
- **(a) Adafruit_GFX + Adafruit_ST7735 como componentes standalone.** As duas libs são
  quase Arduino-free; precisam de um shim de `Print`/`SPI`. `LcdDashboard`/`StartupConfig`
  não mudam. Menos reescrita, carrega ~2 libs de terceiros no grafo IDF.
- **(b) LovyanGFX.** Suporte IDF nativo de primeira classe, API próxima da Adafruit, tem
  driver ST7735. Reescrita média de `DonglePeripherals`/`LcdDashboard` (nomes de método
  diferentes), zero shim.
- **(c) `esp_lcd` (`esp_lcd_panel_st7735`) + engine de texto própria.** Mais leve e
  "IDF-idiomático", mas `LcdDashboard` usa `getTextBounds`/fontes — teria que portar um
  renderizador de texto. Mais trabalho.
- Recomendação: **(a)** para o menor risco/diff, ou **(b)** se preferir não vendorizar
  Adafruit.

### D2 — SQLite
- **(a)** componente do registry `siara-cc/sqlite3` (`idf_component.yml`).
- **(b)** manter o `Sqlite3Esp32` atual vendorizado em `components/`.
- Recomendação: **(a)** se existir versão compatível com IDF 6; senão **(b)**. Em ambos o
  VFS passa a vir do `esp_vfs_fat` (não do `SD_MMC`).

### D3 — OTA agora ou depois?
- O dongle não tem OTA hoje. Adotar (partições duplas + rollback, espelhando bally_OS)
  encarece a fase de build mas alinha 100% com o OS. Ou `factory` única grande agora e OTA
  como tópico futuro.

### D4 — Console local: UART0 (header GPIO43/44) ou USB-Serial/JTAG?
- O console interativo + BTP SerialMux hoje vivem no **mesmo** link USB-CDC que carrega o
  protocolo. Sob IDF o CDC-ACM continua sendo esse canal (via `tinyusb_cdcacm`). O
  `CONFIG_ESP_CONSOLE_*` (logs do `ESP_LOG`, panic handler) é uma **segunda** saída — pode
  ir para UART0 no header (como o comentário do `platformio.ini` já sugere) ou para
  USB-Serial/JTAG. Não afeta o SerialMux. Recomendação: UART0 no header para os logs de
  sistema, CDC-ACM para shell+BTP (igual ao arranjo atual).

---

## 7. Maior risco: USB composto CDC-ACM + HID

O dongle expõe hoje, no mesmo device composto (`ARDUINO_USB_MODE=0`):
1. **CDC Serial** (`Serial`/`USBCDC`) — shell interativo + BTP SerialMux;
2. **HID vendor** (`USBHIDVendor`) — `UsbHidMux`, echo de report (bring-up do tópico 20).

O `bally_OS` só usa `esp_tinyusb` para **MSC**, então não há receita pronta no repo irmão.
Trabalho:

1. **Descritor composto manual.** `tinyusb_config_t.configuration_descriptor` com
   interface CDC (2 endpoints + notif) **e** interface HID (in/out). `esp_tinyusb`
   moderno aceita CDC via `tinyusb_cdcacm` e HID via `tinyusb_config_t` com callbacks
   `tud_hid_*`. O TUSB config precisa `CFG_TUD_CDC=1` + `CFG_TUD_HID=1` e um
   `TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_INOUT_DESC_LEN`.
2. **`ConsoleCdc : ByteIO`** sobre `tinyusb_cdcacm_read` / `tinyusb_cdcacm_write_queue` +
   `tinyusb_cdcacm_write_flush`. `operator bool()` = estado DTR de
   `tud_cdc_get_line_state()` / callback `tinyusb_cdcacm_register_callback(... LINE_STATE_CHANGED ...)`.
3. **Reimplementar o que o arduino-esp32 dava de graça:**
   - **1200-baud touch → bootloader:** callback `tud_cdc_line_coding_cb` — se `bit_rate == 1200`
     e DTR baixo, gravar o "reboot to bootloader" flag e `esp_restart()` (o mesmo que
     `USBCDC` faz internamente). Gated: só no dev build (equivale ao antigo
     `DONGLE_USB_NO_AUTORESET` ao contrário).
   - **`!Serial` (host fechou a porta):** `tick()` já checa `if (!Serial)` para chamar
     `SerialMux::onTransportLost()` — passa a checar `console.operator bool()`.
   - **RX buffer 1 KB:** `CONFIG_TINYUSB_CDC_RX_BUFSIZE=1024`.
4. **`UsbHidMux`** → callbacks `tud_hid_set_report_cb` (recebe report do host) e
   `tud_hid_report_complete_cb`; `tud_hid_report()` para responder. A convenção
   `prepend_size` (buffer[0] = tamanho válido) é lógica da lib, não do transporte — copia
   igual.
5. Provável **patch no `esp_tinyusb`** (como o `bally_OS` faz com
   `scripts/patch_esp_tinyusb.py`) se a versão do registry não deixar CDC+HID coexistirem
   sem editar o descritor — prever um `scripts/patch_esp_tinyusb.py` próprio.

Spike isolado recomendado **antes** de depender disso: um `main.cpp` mínimo que só sobe
CDC+HID e ecoa em ambos, validado com o host, e só então integrar.

---

## 8. Ordem de execução (fases)

Cada fase termina com **build limpo** (`platformio run -e tdongle-s3`) e, quando toca lib
compartilhada, **`platformio test -e native` verde** (o `env:native` é a rede de segurança
que garante que o refactor de `String`/`ByteIO` não mudou comportamento).

| # | Fase | Entregável | Rede de segurança |
|---|---|---|---|
| 0 | **Infra de build** | `CMakeLists.txt`, `components/btp`, `sdkconfig.defaults`, `partitions.csv`, `src/CMakeLists.txt`, `main.cpp` que só printa e pisca o LED. Compila e dá boot. | boot no monitor |
| 1 | **Compat** | `include/compat.h` (`millis`/`delay`/`ByteIO`); `ConsoleCdc` stub sobre UART0; `BoardConfig::initBoardPins` em `driver/gpio` | build |
| 2 | **Núcleo sem periférico** | Grupo A + Grupo B (menos USB/SD/LCD): `ShellOutput`, `ShellConfig`, `ShellCommandSupport`, `SerialMux`, `SerialSession`, `BtpTransport`, `ProtocolRouter`, módulos de comando — contra `ConsoleCdc` stub, sem ESP-NOW real | `test -e native` |
| 3 | **ESP-NOW** | bring-up Wi-Fi nativo em `EspNowManager::begin`; `EspNowConfig` com `ByteIO`. Link com um robô real (STATUS-C, catálogo) | link sobe |
| 4 | **NVS** | `DongleKeyStore` ramo nativo (`nvs_*`); `key L` sobrevive reboot | `hub -set_key_l` + reboot |
| 5 | **SD + SQLite** | `DonglePeripherals::beginSd` em `esp_vfs_fat_sdmmc_mount`; `DatabaseStore` contra VFS; decisão D2 | `database -*`, persistência de peers/histórico |
| 6 | **USB composto** | esp_tinyusb CDC+HID (§7); `ConsoleCdc` real ligado ao CDC; `UsbHidMux`; 1200-touch + DTR | shell + BTP + HID echo com o host; TraceView conecta |
| 7 | **LCD + LED + dashboard** | decisão D1; `DonglePeripherals` LCD/LED; `LcdDashboard`; `StartupConfig` | tela de boot + tiles |
| 8 | **Limpeza** | remover shims temporários; migrar `-D` restantes; revisar checklist do `CONTRIBUTING.md`; `README.md` (seção de build) | build limpo + `test -e native` + smoke test completo |

Fases 3–5 são independentes entre si depois da fase 2 — dá para paralelizar ou reordenar
conforme a prioridade (ESP-NOW é o que mais importa funcionalmente; LCD é o menos crítico).

---

## 9. Riscos e pontos de atenção

- **USB composto (§7)** — o único item sem precedente no `bally_OS`. Fazer o spike antes.
- **LDF do PlatformIO sob `espidf` é mais fraco** que sob `arduino`. Libs com include
  transitivo complicado podem precisar virar `components/` com `CMakeLists.txt` — o
  `CONTRIBUTING.md` §3 já mapeia a cadeia (`AppRuntime → EspNowConfig → ShellConfig → …`);
  ela é um DAG, então é factível, mas espere ajustar 2–3 libs.
- **mbedtls 4.x / PSA** — `RadioSeal`/BTP dependem do backend PSA do AEAD (nota de memória
  `btp-aead-espidf-component`). O `components/btp` com `REQUIRES mbedtls` resolve, **desde
  que** o checkout `../BTP` esteja na revisão ≥ `d96896a`. `DongleKeyStore` e o
  `KeyStore` do OS já vendorizam SHA/HMAC/PBKDF2 justamente para não depender de mbedtls —
  isso continua igual.
- **Boot-heap** (memória `dongle-boot-loop-heap`, tópicos 34/35) — a migração mexe
  exatamente nos alocadores do boot (SerialMux, ESP-NOW, filas RX, SQLite). O IDF puro
  tende a gastar **menos** heap que o arduino-core (sem o overhead do `USBCDC`, sem
  `HardwareSerial`), mas revalidar `DIAG_BOOT` / `logFreeHeap()` na fase 6.
- **SDMMC vs SPI** — o dongle usa barramento SDMMC (4-bit) com pinos não-default; o
  `bally_OS/lib/SDCard` é SPI e **não serve de cópia direta** para essa parte. Usar
  `esp_vfs_fat_sdmmc_mount` + `sdmmc_slot_config_t` com GPIO matrix.
- **Console cosmético vira real** — hoje `monitor_speed` é ignorado (USB-CDC). Sob IDF, se
  os logs de sistema forem para UART0, o baud volta a importar; se forem para
  USB-Serial/JTAG, não.
- **`scripts/*.py`** — `check_channel_contract`, `check_espnow_tx_owner`,
  `check_user_text`, `native_static`, `inject_git_rev` são agnósticos de framework e
  continuam. `inject_git_rev` segue injetando `-DDONGLE_GIT_REV` via `build_flags`.
- **`extra_scripts` sob `espidf`** rodam igual (é hook do PlatformIO, não do framework).

---

## 10. O que NÃO muda

- `[env:native]` e toda a suíte de testes host.
- `lib/` do Grupo A (lógica de protocolo, roteamento, seal).
- `include/bally_channels.h` (idêntico ao do `bally_OS` — confirmado por diff).
- O contrato de wire: BTP `#v2.0.0`, TinyShell `#v1.3.0` (TraceView e bally_OS pinam os
  mesmos).
- TinyShell — já é lib portável, entra por `lib_deps` git tag igual hoje.
- A arquitetura de camadas do `CONTRIBUTING.md` §3 (o `Context`, os módulos de comando).
