#include "controller.h"
#include "string.h"
#include "monitor.h"
#include "http.h"
#include <unistd.h>

bool equals(const char* a, const char* b) {
	return strcmp(a, b) == 0;
}

int main(int argc, char** argv) {
	bool               runController = false;
	bool               showHelp      = false;
	homepower::Monitor monitor;
	for (int i = 1; i < argc; i++) {
		const char* arg = argv[i];
		if (equals(arg, "-c")) {
			runController = true;
		} else if (equals(arg, "-?") || equals(arg, "-h") || equals(arg, "--help")) {
			showHelp = true;
		} else if (i + 1 < argc && (equals(arg, "-i") || equals(arg, "--inv"))) {
			monitor.Inverter.Device = argv[i + 1];
			i++;
		} else {
			fprintf(stderr, "Unknown argument '%s'\n", arg);
			showHelp = true;
		}
	}

	if (showHelp) {
		fprintf(stderr, "server - Monitor Axpert/Voltronic inverter, and write stats to Postgres database\n");
		fprintf(stderr, " -c                Run controller, which switches heavy loads on GPIO pins 0 and 1\n");
		fprintf(stderr, " -i --inv <device> Specify inverter device communication channel\n");
		fprintf(stderr, "                   (eg /dev/hidraw0 for direct USB, or /dev/ttyUSB0\n");
		fprintf(stderr, "                   for RS232-to-USB adapter). Default %s\n", monitor.Inverter.Device.c_str());
		return 1;
	}

	monitor.Start();
	bool ok = true;
	if (runController) {
		homepower::Controller controller(&monitor);
		controller.SetHeavyLoadMode(homepower::HeavyLoadMode::Grid);
		controller.Start();
		ok = homepower::RunHttpServer(controller);
		controller.Stop();
	} else {
		// should ideally listen for SIGHUP or something
		while (true) {
			sleep(10);
		}
	}
	monitor.Stop();
	return ok ? 0 : 1;
}