#include "controller.h"
#include "http.h"

int main(int argc, char** argv) {
	homepower::Controller controller;
	if (!homepower::RunHttpServer(controller))
		return 1;
	return 0;
}