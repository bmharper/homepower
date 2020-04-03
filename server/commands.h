#pragma once

namespace homepower {

enum class PowerSource {
	Unknown,
	UtilitySolarBattery,
	SolarUtilityBattery,
	SolarBatteryUtility,
};

enum class ChargerPriority {
	Utility,
	SolarFirst,
	UtilitySolar,
	SolarOnly,
};

const char* PowerSourceToString(PowerSource v);
const char* PowerSourceDescribe(PowerSource v);
const char* ChargerPriorityToString(ChargerPriority v);

} // namespace homepower
