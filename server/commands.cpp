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

}