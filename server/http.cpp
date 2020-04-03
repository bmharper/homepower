#include "../phttp/phttp.h"
#include "controller.h"

namespace homepower {

bool RunHttpServer(Controller& controller) {
	phttp::Server server;
	return server.ListenAndRun("0.0.0.0", 8080, [&](phttp::Response& w, phttp::RequestPtr r) {
		if (r->Method == "POST") {
			if (r->Path == "/switch/inverter")
				controller.SetHeavyLoadMode(HeavyLoadMode::Inverter);
			else if (r->Path == "/switch/grid")
				controller.SetHeavyLoadMode(HeavyLoadMode::Grid);
			else if (r->Path == "/switch/off")
				controller.SetHeavyLoadMode(HeavyLoadMode::Off);
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