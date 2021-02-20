#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <string>
#include <termios.h>
#include "inverter.h"

/*

QPIGS:



(000.0 00.0 228.2 50.0 0346 0337 011 429 27.00 000 095 0038 01.3 248.1 00.00 00001 10010000 00 00 00336 010
 ACInV      AcOutV     VA        Load%   BattV     Bat%     BattA?                                SolW      
       AcInHz     AcOutHz   LoadW    BusV      BatChgA          SolV


(000.0  00.0    228.2   50.0     0346    0337   011    429   27.00  000     095   0038  01.3  248.1  00.00  00001   10010000  00  00  00336       010
 AcInV  AcInHz  AcOutV  AcOutHz  LoadVA  LoadW  Load%  BusV  BatV   BatChA  Bat%  Temp  PvA   PvV                                     PvW 

*/

using namespace std;

namespace homepower {

uint16_t CRC(const uint8_t* pin, size_t len) {
	uint16_t crc_ta[16] = {
	    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5, 0x60c6, 0x70e7,
	    0x8108, 0x9129, 0xa14a, 0xb16b, 0xc18c, 0xd1ad, 0xe1ce, 0xf1ef};

	auto     ptr = pin;
	uint16_t crc = 0;

	while (len-- != 0) {
		uint8_t da = ((uint8_t)(crc >> 8)) >> 4;
		crc <<= 4;
		crc ^= crc_ta[da ^ (*ptr >> 4)];
		da = ((uint8_t)(crc >> 8)) >> 4;
		crc <<= 4;
		crc ^= crc_ta[da ^ (*ptr & 0x0f)];
		ptr++;
	}

	uint8_t bCRCLow  = crc;
	uint8_t bCRCHign = (uint8_t)(crc >> 8);

	if (bCRCLow == 0x28 || bCRCLow == 0x0d || bCRCLow == 0x0a)
		bCRCLow++;
	if (bCRCHign == 0x28 || bCRCHign == 0x0d || bCRCHign == 0x0a)
		bCRCHign++;

	crc = ((uint16_t) bCRCHign) << 8;
	crc += bCRCLow;
	return crc;
}

string FinishMsg(const string& raw) {
	string   b   = raw;
	uint16_t crc = CRC((const uint8_t*) raw.data(), raw.size());
	b += char(crc >> 8);
	b += char(crc & 0xff);
	b += char(0x0d);
	return b;
}

bool IsValidResponse(string& msg) {
	size_t len = msg.size();
	if (len < 4)
		return false;
	size_t i = len - 1;
	for (; i != -1 && msg[i] == 0; i--) {
	}
	if (i == -1 || i < 4)
		return false;

	len = i + 1;
	//printf("Testing length of %d\n", len);
	uint16_t crc  = CRC((const uint8_t*) msg.data(), len - 3);
	uint8_t  crc1 = crc >> 8;
	uint8_t  crc2 = crc & 0xff;
	if (crc1 == msg[len - 3] && crc2 == msg[len - 2]) {
		msg = msg.substr(0, len - 3);
		return true;
	}
	return false;
}

void DumpMsg(const string& raw) {
	printf("Message: [");
	for (size_t i = 0; i < raw.size(); i++) {
		int c = raw[i];
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
			printf("%c", c);
		else
			printf("%02X", c);
	}
	printf("]\n");
}

bool SendMsg(int fd, const string& raw) {
	string      msg    = FinishMsg(raw);
	const char* out    = msg.data();
	size_t      remain = msg.size();
	//DumpMsg(msg);
	do {
		int n = write(fd, out, remain);
		if (n <= 0)
			return false;
		//printf("wrote %d/%d bytes\n", n, (int) msg.size());
		out += n;
		remain -= n;
	} while (remain != 0);
	return true;
}

double GetTime() {
	timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double) t.tv_sec + (double) t.tv_nsec / 1e9;
}

bool RecvMsg(int fd, double timeout, string& msg) {
	char buf[1024];
	auto start = GetTime();
	while (true) {
		int n = read(fd, buf, sizeof(buf));
		if (n > 0) {
			msg.append((const char*) buf, n);
			if (IsValidResponse(msg))
				return true;
		}
		if (GetTime() - start > timeout)
			return false;
		usleep(100);
	}
}

// Interpret a known command
bool Inverter::Interpret(const std::string& resp, Record_QPIGS& out) {
	// (000.0  00.0    228.2   50.0     0346    0337   011    429   27.00  000     095   0038  01.3  248.1  00.00  00001   10010000  00  00  00336       010
	//  AcInV  AcInHz  AcOutV  AcOutHz  LoadVA  LoadW  Load%  BusV  BatV   BatChA  Bat%  Temp  PvA   PvV                                     PvW
	double acInV   = 0;
	double acInHz  = 0;
	double acOutV  = 0;
	double acOutHz = 0;
	double loadVA  = 0;
	double loadW   = 0;
	double loadP   = 0;
	double busV    = 0;
	double batV    = 0;
	double batChA  = 0;
	double batP    = 0;
	double temp    = 0;
	double pvA     = 0;
	double pvV     = 0;
	double pvW     = 0;
	double n[1]    = {0};
	char   s[5][40];
	int    tok = sscanf(resp.c_str() + 1, "%lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %s %s %s %s %lf %s",
                     &acInV, &acInHz, &acOutV, &acOutHz, &loadVA, &loadW, &loadP, &busV, &batV, &batChA, &batP, &temp, &pvA, &pvV,
                     n + 0, (char*) (s + 0), (char*) (s + 1), (char*) (s + 2), (char*) (s + 3), &pvW, (char*) (s + 4));
	if (tok == 21) {
		out.Raw      = resp;
		out.Time     = time(nullptr);
		out.ACInV    = acInV;
		out.ACInHz   = acInHz;
		out.ACOutV   = acOutV;
		out.ACOutHz  = acOutHz;
		out.LoadVA   = loadVA;
		out.LoadW    = loadW;
		out.LoadP    = loadP;
		out.BusV     = busV;
		out.BatP     = batP;
		out.BatChA   = batChA;
		out.BatV     = batV;
		out.Temp     = temp;
		out.PvA      = pvA;
		out.PvV      = pvV;
		out.PvW      = pvW;
		out.Unknown1 = n[0];
		out.Unknown2 = s[0];
		out.Unknown3 = s[1];
		out.Unknown4 = s[2];
		out.Unknown5 = s[3];
		out.Unknown6 = s[4];
		return true;
	}
	return false;
}

nlohmann::json Inverter::Record_QPIGS::ToJSON() const {
	return nlohmann::json({
	    {"Raw", Raw},
	    {"ACInV", ACInV},
	    {"ACInHz", ACInHz},
	    {"ACOutV", ACOutV},
	    {"ACOutHz", ACOutHz},
	    {"LoadVA", LoadVA},
	    {"LoadW", LoadW},
	    {"LoadP", LoadP},
	    {"BusV", BusV},
	    {"BatV", BatV},
	    {"BatChA", BatChA},
	    {"BatP", BatP},
	    {"Temp", Temp},
	    {"PvA", PvA},
	    {"PvV", PvV},
	    {"PvW", PvW},
	    {"Unknown1", Unknown1},
	    {"Unknown2", Unknown2},
	    {"Unknown3", Unknown3},
	    {"Unknown4", Unknown4},
	    {"Unknown5", Unknown5},
	    {"Unknown6", Unknown6},
	});
}

Inverter::~Inverter() {
	Close();
}

bool Inverter::Open() {
	Close();

	FD = open(Device.c_str(), O_RDWR | O_NONBLOCK);
	if (FD == -1) {
		fprintf(stderr, "Unable to open device file '%s' (errno=%d %s)\n", Device.c_str(), errno, strerror(errno));
		return false;
	}

	// If this looks like an RS232-to-USB adapter, then set the serial port parameters
	if (Device.find("ttyUSB") != -1) {
		// apparently 9600 is flaky on these things
		speed_t baud = B2400;

		// Speed settings (in this case, 2400 8N1)
		struct termios settings;
		int            r = 0;
		if ((r = tcgetattr(FD, &settings)) != 0) {
			fprintf(stderr, "tcgetattr failed with %d\n", r);
			Close();
			return false;
		}

		// baud rate
		r = cfsetospeed(&settings, baud);
		if ((r = cfsetospeed(&settings, baud)) != 0) {
			fprintf(stderr, "cfsetospeed failed with %d\n", r);
			Close();
			return false;
		}
		cfmakeraw(&settings);        // It's vital to set this to RAW mode (instead of LINE)
		settings.c_cflag &= ~PARENB; // no parity
		settings.c_cflag &= ~CSTOPB; // 1 stop bit
		settings.c_cflag &= ~CSIZE;
		settings.c_cflag |= CS8 | CLOCAL; // 8 bits
		// settings.c_lflag = ICANON;         // canonical mode
		settings.c_oflag &= ~OPOST; // remove post-processing

		if ((r = tcsetattr(FD, TCSANOW, &settings)) != 0) {
			fprintf(stderr, "tcsetattr failed with %d\n", r);
			Close();
			return false;
		}
		tcflush(FD, TCOFLUSH);
		//tcdrain(fd);
	}
	return true;
}

void Inverter::Close() {
	if (FD == -1)
		return;
	close(FD);
	FD = -1;
}

Inverter::Response Inverter::Execute(Record_QPIGS& response) {
	return ExecuteT("QPIGS", response);
}

Inverter::Response Inverter::Execute(string cmd) {
	string resp;
	return Execute(cmd, resp);
}

Inverter::Response Inverter::Execute(string cmd, std::string& response) {
	if (FD == -1) {
		if (!Open())
			return Response::FailOpenFile;
	}

	auto res = Response::FailRecv;
	if (SendMsg(FD, cmd)) {
		if (RecvMsg(FD, RecvTimeout, response)) {
			//auto inter = Interpret(cmd, resp);
			//if (!inter.is_null()) {
			//	// Dump the JSON to stdout
			//	res = Response::OK;
			//	//printf("%s\n", inter.dump(4).c_str());
			//	response = inter.dump(4);
			//} else if (response == "(ACK") {
			if (response == "(ACK") {
				// In this case, we produce no output, but the caller can tell by our exit code that the command succeeded
				// Write a single byte, so that the caller (who is using popen), can detect that we are finished
				//printf("OK");
				res = Response::OK;
			} else if (response == "(NAK") {
				fprintf(stderr, "NAK (Not Acknowledged). This is likely a CRC failure, so something wrong with the COM port or BAUD rate, etc\n");
				res = Response::NAK;
			} else {
				//res = Response::DontUnderstand;
				//fprintf(stderr, "RAW: <<%s>>", resp.c_str());
				res = Response::OK;
			}
		} else {
			res        = Response::FailRecv;
			size_t len = response.size();
			if (len > 10)
				fprintf(stderr, "RecvMsg Fail: %s\nLast 10 bytes: %d %d %d %d %d %d %d %d %d %d\n", response.c_str(), response[len - 10], response[len - 9], response[len - 8], response[len - 7], response[len - 6], response[len - 5], response[len - 4], response[len - 3], response[len - 2], response[len - 1]);
			else
				fprintf(stderr, "RecvMsg Fail: %s\n", response.c_str());
		}
	} else {
		res = Response::FailWriteFile;
	}

	return res;
}

string Inverter::RawToPrintable(const string& raw) {
	string r;
	for (size_t i = 0; i < raw.size(); i++) {
		int c = raw[i];
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
			r += (char) c;
		} else {
			char buf[4];
			sprintf(buf, ".%02X", c);
			r += buf;
		}
	}
	return r;
}

} // namespace homepower