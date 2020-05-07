#include "controller.h"
#include "string.h"
#include "monitor.h"
#include "http.h"

int main(int argc, char** argv) {
	bool runController = true;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-r") == 0) {
			runController = false;
		} else {
			fprintf(stderr, "Unknown command '%s'\n", argv[i]);
			return 1;
		}
	}
	homepower::Monitor monitor;
	monitor.Start();
	bool ok = true;
	if (runController) {
		homepower::Controller controller(&monitor);
		controller.SetHeavyLoadMode(homepower::HeavyLoadMode::Grid);
		controller.Start();
		ok = homepower::RunHttpServer(controller);
		controller.Stop();
	}
	monitor.Stop();
	return ok ? 0 : 1;
}