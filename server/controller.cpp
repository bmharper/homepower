#include <time.h>
#include <unistd.h>
#include <wiringPi.h>
#include "controller.h"
#include "monitor.h"

using namespace std;

namespace homepower {

const char* ModeToString(PowerMode mode) {
	switch (mode) {
	case PowerMode::Off: return "Off";
	case PowerMode::Grid: return "Grid";
	case PowerMode::Inverter: return "Inverter";
	}
}

TimePoint TimePoint::Now(int timezoneOffsetMinutes) {
	auto t = time(nullptr);
	tm   tt;
	gmtime_r(&t, &tt);
	int sec = tt.tm_hour * 3600 + tt.tm_min * 60 + tt.tm_sec;
	sec += timezoneOffsetMinutes * 60;
	if (sec < 0)
		sec += 24 * 3600;
	sec = sec % (24 * 3600);
	TimePoint tp;
	tp.Hour   = sec / 3600;
	tp.Minute = (sec - tp.Hour * 3600) / 60;
	return tp;
}

Controller::Controller(homepower::Monitor* monitor) {
	// I don't know of any way to read the state of the GPIO output pins using WiringPI,
	// so in order to know our state at startup, we need to create it.
	// This seems like a conservative thing to do anyway.
	// I'm sure there is a way to read the state using other mechanisms, but I don't
	// have any need for that, because this server is intended to come on and stay
	// on for months, without a restart.
	Monitor  = monitor;
	MustExit = false;
	wiringPiSetup();
	pinMode(GpioPinGrid, OUTPUT);
	pinMode(GpioPinInverter, OUTPUT);
	digitalWrite(GpioPinGrid, 0);
	digitalWrite(GpioPinInverter, 0);
	Mode     = PowerMode::Off;
	auto now = Now();
	printf("Time now (local): %d:%02d\n", now.Hour, now.Minute);
}

void Controller::Start() {
	MustExit = false;
	Thread   = thread([&]() {
        printf("Controlled started\n");
        Run();
        printf("Controlled exited\n");
    });
}

void Controller::Stop() {
	MustExit = true;
	Thread.join();
}

void Controller::SetMode(PowerMode m, bool forceWrite) {
	if (Mode == m && !forceWrite)
		return;

	printf("Set mode to %s\n", ModeToString(m));

	timespec pause;
	pause.tv_sec  = 0;
	pause.tv_nsec = SleepMilliseconds * 1000 * 1000;

	if (m == PowerMode::Inverter) {
		digitalWrite(GpioPinGrid, 0);
		nanosleep(&pause, nullptr);
		digitalWrite(GpioPinInverter, 1);
		Monitor->IsHeavyOnInverter = true;
	} else if (m == PowerMode::Grid) {
		digitalWrite(GpioPinInverter, 0);
		nanosleep(&pause, nullptr);
		digitalWrite(GpioPinGrid, 1);
		Monitor->IsHeavyOnInverter = false;
	} else if (m == PowerMode::Off) {
		digitalWrite(GpioPinInverter, 0);
		digitalWrite(GpioPinGrid, 0);
		Monitor->IsHeavyOnInverter = false;
	}

	Mode = m;
}

void Controller::Run() {
	auto lastStatus = 0;
	while (!MustExit) {
		time_t    now     = time(nullptr);
		auto      nowP    = Now();
		PowerMode desired = PowerMode::Grid;

		bool monitorIsAlive   = Monitor->IsInitialized;
		bool isSolarTime      = nowP > SolarOnAt && nowP < SolarOffAt;
		int  solarV           = Monitor->AvgSolarV;
		bool hasGridPower     = Monitor->HasGridPower;
		bool haveSolarVoltage = solarV > MinSolarVoltage;
		if (monitorIsAlive && !Monitor->IsOverloaded && isSolarTime && haveSolarVoltage && hasGridPower) {
			desired = PowerMode::Inverter;
		} else {
			if (monitorIsAlive && time(nullptr) - lastStatus > 10 * 60) {
				lastStatus = time(nullptr);
				fprintf(stderr, "isSolarTime: %s, hasGridPower: %s, haveSolarVoltage(%d): %s, IsOverloaded: %s (time %d:%02d)\n",
				        isSolarTime ? "yes" : "no", hasGridPower ? "yes" : "no", solarV, haveSolarVoltage ? "yes" : "no",
				        Monitor->IsOverloaded ? "yes" : "no",
				        nowP.Hour, nowP.Minute);
				fflush(stderr);
			}
		}

		if (desired != Mode) {
			if (desired == PowerMode::Grid || now - LastSwitch > CooloffSeconds) {
				SetMode(desired);
				LastSwitch = now;
			}
		}

		int millisecond = 1000;
		usleep(100 * millisecond);
	}
}

TimePoint Controller::Now() {
	return TimePoint::Now(TimezoneOffsetMinutes);
}

} // namespace homepower