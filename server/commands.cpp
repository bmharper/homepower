#include "commands.h"

namespace homepower {

const char* PowerSourceToString(PowerSource v) {
	switch (v) {
	case PowerSource::Unknown: return "01"; // Unexpected code path
	case PowerSource::UtilitySolarBattery: return "00";
	case PowerSource::SolarUtilityBattery: return "01";
	case PowerSource::SolarBatteryUtility: return "02";
	}
}

const char* PowerSourceDescribe(PowerSource v) {
	switch (v) {
	case PowerSource::Unknown: return "Unknown";
	case PowerSource::UtilitySolarBattery: return "USB";
	case PowerSource::SolarUtilityBattery: return "SUB";
	case PowerSource::SolarBatteryUtility: return "SBU";
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