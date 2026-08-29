#pragma once

#include <cstdint>

/**
 * @brief "espnow" module: peer management and message sending over ESP-NOW.
 */
namespace EspNowCommands {

/**
 * @brief One snapshot of both hops' cumulative counters, for "espnow -stats".
 *
 * Deliberately a local mirror of EspNowConfig::RxCounters plus the fields of
 * SerialMux::TxCounters this command prints, instead of including those
 * headers: EspNowConfig.h would close the cycle EspNowCommands ->
 * EspNowConfig -> ShellConfig -> EspNowCommands that CONTRIBUTING.md section
 * 3 warns about and the README's dependency graph claims does not exist
 * (ShellConfig.cpp includes this header; EspNowConfig.cpp includes
 * ShellConfig.h). Same reason -- and the same duplication -- as
 * ShellCommandSupport::Context mirroring ShellConfig::Context field for field.
 *
 * Everything here is cumulative and never cleared by a read, so two snapshots
 * and the elapsed time between them give a rate.
 */
struct StatsSnapshot {
    // ESP-NOW hop (robot -> dongle).
    uint32_t datagrams;
    uint32_t fragmentsAccepted;
    uint32_t routedTelemetry;
    uint32_t routedLog;
    uint32_t routedCommand;
    uint32_t routedControl;
    uint32_t routedTerminal;
    uint32_t droppedRx;
    uint32_t droppedDecode;
    uint32_t droppedCrc;
    uint32_t droppedReassembly;
    uint32_t droppedQueueFull;
    // Topico 30: a channel C (dongle<->robot) frame that did not open under
    // key L -- no key configured, missing ENCRYPTED, wrong cipher, or a
    // forged/corrupted tag. Bench-only (not part of hub.link's wire
    // schema): "espnow -stats" is the one place this dongle can currently
    // see it.
    uint32_t droppedAuth;
    // Radio datagrams dropped because async RX never came up at boot (heap
    // starvation). Nonzero = the dongle booted degraded and cannot hear the
    // radio at all -- fix the boot heap (topico 34/35), do not chase a
    // "robot offline". Bench-only, like droppedAuth.
    uint32_t syncFallbackDrops;

    // ESP-NOW TX scheduler (Critical, Control, Data in that order).
    uint32_t txEnqueued[3];
    uint32_t txDroppedQueueFull[3];
    uint32_t txDriverRejected;
    uint32_t txCallbackTimeouts;
    uint32_t txCallbacksReceived;

    // USB-Serial hop (dongle -> desktop).
    uint64_t framesRx;
    uint64_t framesTx;
    // Frames the mux could not push to the port -- the host is not asserting
    // DTR on the CDC (so USBCDC::write() drops every byte) or the cable is
    // gone. framesRx climbing while framesTx is flat and this rises is the
    // signature of "the desktop sends but never hears us" (topico 35 F1).
    uint64_t framesTxStalled;
    uint64_t telemetryDropped;
    uint64_t droppedSession;
    uint64_t droppedTerminal;
    uint64_t droppedLogStatus;
    uint64_t droppedTelemetryQueue;
    bool protocolled;
};

/**
 * @brief Fills a snapshot from whatever layer actually owns those counters.
 *
 * Injected by AppRuntime, the only layer that already reaches both
 * EspNowConfig and SerialMux -- same callback-injection boundary AppRuntime
 * already uses to hand SerialMux::begin its RunShellLineFn and
 * RelayToRadioFn.
 */
using StatsProviderFn = void (*)(StatsSnapshot& out);

/**
 * @brief Binds the provider "espnow -stats" reads. Call once from
 * AppRuntime::begin(). Without it, the command reports that stats are
 * unavailable instead of printing zeros that would read as "no traffic".
 */
void setStatsProvider(StatsProviderFn provider);

/**
 * @brief Creates the "espnow" module and registers its commands.
 */
uint8_t registerAll();

} // namespace EspNowCommands
