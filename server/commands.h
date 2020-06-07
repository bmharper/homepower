#pragma once

namespace homepower {

enum class PowerSource {
	Unknown,
	USB, // Utility Solar Battery
	SUB, // Solar Utility Battery
	SBU, // Solar Battery Utility
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
