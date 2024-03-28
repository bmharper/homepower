#pragma once

namespace homepower {

enum class PowerSource {
	Unknown,
	USB, // Utility Solar Battery
	SUB, // Solar Utility Battery
	SBU, // Solar Battery Utility
};

enum class ChargerPriority {
	Unknown,
	Utility,
	SolarFirst,
	UtilitySolar,
	SolarOnly,
};

enum class TriState {
	Off,
	On,
	Auto,
};

const char* PowerSourceToString(PowerSource v);
const char* PowerSourceDescribe(PowerSource v);
const char* ChargerPriorityToString(ChargerPriority v);
const char* TriStateToString(TriState s);
TriState    ParseTriState(const char* s);

} // namespace homepower
