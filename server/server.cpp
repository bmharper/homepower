#include "controller.h"
#include "monitor.h"
#include "http.h"

int main(int argc, char** argv) {
	homepower::Monitor    monitor;
	homepower::Controller controller(&monitor);
	monitor.Start();
	controller.SetMode(homepower::PowerMode::Grid);
	controller.Start();
	bool ok = homepower::RunHttpServer(controller);
	controller.Stop();
	monitor.Stop();
	return ok ? 0 : 1;
}