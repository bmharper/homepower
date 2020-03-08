#include <stdio.h>
#include <time.h>
#include <string>
#include "phttp/phttp.h"
#include <wiringPi.h>

static const int PIN_GRID           = 0;
static const int PIN_INVERTER       = 1;
static const int SLEEP_MILLISECONDS = 20; // 50hz = 20ms cycle time. Hager ESC225 have 25ms switch off time, and 10ms switch on time.

using namespace std;

int main(int argc, char** argv) {
	wiringPiSetup();
	pinMode(PIN_GRID, OUTPUT);
	pinMode(PIN_INVERTER, OUTPUT);

	digitalWrite(PIN_GRID, 0);
	digitalWrite(PIN_INVERTER, 0);
	string currentMode = "off";

	auto change = [&currentMode](string mode) {
		if (currentMode == mode)
			return;
		timespec pause;
		pause.tv_sec  = 0;
		pause.tv_nsec = SLEEP_MILLISECONDS * 1000000;

		if (mode == "inverter") {
			digitalWrite(PIN_GRID, 0);
			nanosleep(&pause, nullptr);
			digitalWrite(PIN_INVERTER, 1);
		} else if (mode == "grid") {
			digitalWrite(PIN_INVERTER, 0);
			nanosleep(&pause, nullptr);
			digitalWrite(PIN_GRID, 1);
		} else if (mode == "off") {
			digitalWrite(PIN_INVERTER, 0);
			digitalWrite(PIN_GRID, 0);
		}
		currentMode = mode;
	};

	phttp::Server server;
	server.ListenAndRun("0.0.0.0", 8080, [&](phttp::Response& w, phttp::RequestPtr r) {
		if (r->Method == "POST") {
			if (r->Path == "/switch/inverter")
				change("inverter");
			else if (r->Path == "/switch/grid")
				change("grid");
			else if (r->Path == "/switch/off")
				change("off");
			else {
				w.SetStatusAndBody(404, "");
				return;
			}
			w.SetStatusAndBody(200, "OK");
		} else {
			w.SetStatusAndBody(404, "Unknown request");
		}
	});
	return 0;
}