#include <string.h>
#include "commands.h"

namespace homepower {

const char* PowerSourceToString(PowerSource v) {
	switch (v) {
	case PowerSource::Unknown: return "01"; // Unexpected code path
	case PowerSource::USB: return "00";
	case PowerSource::SUB: return "01";
	case PowerSource::SBU: return "02";
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
	case ChargerPriority::Utility: return "";
	case ChargerPriority::SolarFirst: return "";
	case ChargerPriority::UtilitySolar: return "";
	case ChargerPriority::SolarOnly: return "";
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