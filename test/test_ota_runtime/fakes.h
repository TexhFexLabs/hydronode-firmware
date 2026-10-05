#pragma once
#include <optional>
#include <vector>
#include "ota/OtaStore.h"
namespace fake {
extern std::optional<hn::ota::Pending> record;
extern std::vector<uint8_t> config, backup;
extern bool interruptWrite, interruptCleanup;
extern uint32_t pulseLeft;
extern unsigned backupLoads;
void reset();
}
