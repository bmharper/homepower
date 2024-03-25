#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <string>
#include "json.hpp"
#include "server/inverter.h"

using namespace std;
using namespace homepower;

nlohmann::json Record_QPIGS_ToJSON(Inverter::Record_QPIGS r);

void ShowHelp() {
	fprintf(stderr, "query <device> <cmd>\n");
	fprintf(stderr, "  example device = /dev/hidraw0 (/dev/ttyUSB0 for RS232-to-USB adapter)\n");
	fprintf(stderr, "  example cmd    = QPIGS\n");
}

int main(int argc, char** argv) {
	if (argc < 3) {
		ShowHelp();
		return (int) Inverter::Response::InvalidCommand;
	}
	Inverter inv;
	inv.Devices = {argv[1]};
	string cmd  = argv[2];
	string response;
	auto   r = inv.Execute(cmd, response);
	printf("%s\n", response.c_str());

	// special case processing for known commands
	if (r == homepower::Inverter::Response::OK && cmd == "QPIGS") {
		homepower::Inverter::Record_QPIGS out;
		if (inv.Interpret(response, out)) {
			printf("Interpreted response:\n%s\n", Record_QPIGS_ToJSON(out).dump(4).c_str());
		} else {
			printf("Failed to interpret response\n");
		}
	}

	return (int) r;
}

nlohmann::json Record_QPIGS_ToJSON(Inverter::Record_QPIGS r) {
	return nlohmann::json({
	    {"Raw", r.Raw},
	    {"ACInV", r.ACInV},
	    {"ACInHz", r.ACInHz},
	    {"ACOutV", r.ACOutV},
	    {"ACOutHz", r.ACOutHz},
	    {"LoadVA", r.LoadVA},
	    {"LoadW", r.LoadW},
	    {"LoadP", r.LoadP},
	    {"BusV", r.BusV},
	    {"BatV", r.BatV},
	    {"BatChA", r.BatChA},
	    {"BatP", r.BatP},
	    {"Temp", r.Temp},
	    {"PvA", r.PvA},
	    {"PvV", r.PvV},
	    {"PvW", r.PvW},
	    {"Unknown1", r.Unknown1},
	    {"Unknown2", r.Unknown2},
	    {"Unknown3", r.Unknown3},
	    {"Unknown4", r.Unknown4},
	    {"Unknown5", r.Unknown5},
	    {"Unknown6", r.Unknown6},
	});
}
