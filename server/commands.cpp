#include <string.h>
#include "commands.h"

namespace homepower {

const char* PowerSourceToString(PowerSource v) {
	switch (v) {
	case PowerSource::Unknown: return "POP01"; // Unexpected code path
	case PowerSource::USB: return "POP00";
	case PowerSource::SUB: return "POP01";
	case PowerSource::SBU: return "POP02";
	}
}

const char* PowerSourceDescribe(PowerSource v) {
	switch (v) {
	case PowerSource::Unknown: return "Unknown";
	case PowerSource::USB: return "USB";
	case PowerSource::SUB: return "SUB";
	case PowerSource::SBU: return "SBU";
	}
}

const char* ChargerPriorityToString(ChargerPriority v) {
	switch (v) {
	case ChargerPriority::Unknown: return "PCP02"; // Unexpected code path
	case ChargerPriority::Utility: return "PCP00";
	case ChargerPriority::SolarFirst: return "PCP01";
	case ChargerPriority::UtilitySolar: return "PCP02";
	case ChargerPriority::SolarOnly: return "PCP03";
	}
}

const char* ChargerPriorityDescribe(ChargerPriority v) {
	switch (v) {
	case ChargerPriority::Unknown: return "Unknown";
	case ChargerPriority::Utility: return "Utility only";
	case ChargerPriority::SolarFirst: return "Solar first";
	case ChargerPriority::UtilitySolar: return "Solar and Utility";
	case ChargerPriority::SolarOnly: return "Solar only";
	}
}

const char* TriStateToString(TriState s) {
	switch (s) {
	case TriState::Off: return "Off";
	case TriState::On: return "On";
	case TriState::Auto: return "Auto";
	}
}

TriState ParseTriState(const char* s) {
	if (strcmp(s, "Off") == 0)
		return TriState::Off;
	if (strcmp(s, "On") == 0)
		return TriState::On;
	if (strcmp(s, "Auto") == 0)
		return TriState::Auto;
	return TriState::Auto;
}

} // namespace homepower