#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <string>
#include "server/inverter.h"

using namespace std;
using namespace homepower;

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
	inv.Device = argv[1];
	string cmd = argv[2];
	string response;
	auto   r = inv.Execute(cmd, response);
	printf("%s\n", response.c_str());

	// special case processing for known commands
	if (r == homepower::Inverter::Response::OK && cmd == "QPIGS") {
		homepower::Inverter::Record_QPIGS out;
		if (inv.Interpret(response, out)) {
			printf("Interpreted response:\n%s\n", out.ToJSON().dump(4).c_str());
		} else {
			printf("Failed to interpret response\n");
		}
	}

	return (int) r;
}
