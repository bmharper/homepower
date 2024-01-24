#include "../phttp/phttp.h"
#include "controller.h"

namespace homepower {

bool RunHttpServer(Controller& controller) {
	phttp::Server server;
	return server.ListenAndRun("0.0.0.0", 8080, [&](phttp::Response& w, phttp::RequestPtr r) {
		if (r->Method == "POST") {
			if (r->Path == "/switch/inverter")
				controller.SetHeavyLoadState(HeavyLoadState::Inverter);
			else if (r->Path == "/switch/grid")
				controller.SetHeavyLoadState(HeavyLoadState::Grid);
			else if (r->Path == "/switch/off")
				controller.SetHeavyLoadState(HeavyLoadState::Off);
			else if (r->Path == "/mode/sub")
				controller.ChangePowerSource(PowerSource::SUB);
			else if (r->Path == "/mode/sbu")
				controller.ChangePowerSource(PowerSource::SBU);
			else if (r->Path == "/mode/usb")
				controller.ChangePowerSource(PowerSource::USB);
			else if (r->Path == "/heavy/solar")
				controller.SetHeavyLoadMode(HeavyLoadMode::OnWithSolar);
			else if (r->Path == "/heavy/always")
				controller.SetHeavyLoadMode(HeavyLoadMode::AlwaysOn);
			else {
				w.SetStatusAndBody(404, "Unknown POST request");
				return;
			}
			w.SetStatusAndBody(200, "OK");
		} else {
			w.SetStatusAndBody(404, "Unknown request");
		}
	});
}
} // namespace homepower