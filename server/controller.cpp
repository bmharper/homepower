#include <time.h>
#include <wiringPi.h>
#include "controller.h"

namespace homepower {

Controller::Controller() {
	// I don't know of any way to read the state of the GPIO output pins using WiringPI,
	// so in order to know our state at startup, we need to create it.
	// This seems like a conservative thing to do anyway.
	// I'm sure there is a way to read the state using other mechanisms, but I don't
	// have any need for that, because this server is intended to come on and stay
	// on for months, without a restart.
	wiringPiSetup();
	pinMode(GpioPinGrid, OUTPUT);
	pinMode(GpioPinInverter, OUTPUT);
	digitalWrite(GpioPinGrid, 0);
	digitalWrite(GpioPinInverter, 0);
	Mode = PowerMode::Off;
}

void Controller::SetMode(PowerMode m, bool forceWrite) {
	if (Mode == m && !forceWrite)
		return;

	timespec pause;
	pause.tv_sec  = 0;
	pause.tv_nsec = SleepMilliseconds * 1000000;

	if (m == PowerMode::Inverter) {
		digitalWrite(GpioPinGrid, 0);
		nanosleep(&pause, nullptr);
		digitalWrite(GpioPinInverter, 1);
	} else if (m == PowerMode::Grid) {
		digitalWrite(GpioPinInverter, 0);
		nanosleep(&pause, nullptr);
		digitalWrite(GpioPinGrid, 1);
	} else if (m == PowerMode::Off) {
		digitalWrite(GpioPinInverter, 0);
		digitalWrite(GpioPinGrid, 0);
	}

	Mode = m;
}

} // namespace homepower