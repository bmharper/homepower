#pragma once

namespace homepower {

enum class PowerMode {
	Off,
	Grid,
	Inverter,
};

class Controller {
public:
	int GpioPinGrid       = 0;
	int GpioPinInverter   = 1;
	int SleepMilliseconds = 20; // 50hz = 20ms cycle time. Hager ESC225 have 25ms switch off time, and 10ms switch on time.

	Controller();
	void SetMode(PowerMode m, bool forceWrite = false);

private:
	PowerMode Mode = PowerMode::Off;
};

} // namespace homepower