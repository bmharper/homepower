#include <iostream>
#include <stdio.h>
#include <assert.h>
#include "controllerUtils.h"
#include "ringbuffer.h"
#include "monitorUtils.h"

// For debugging:
// clang -g -o testUtils server/testUtils.cpp server/monitorUtils.cpp -std=c++11 -lstdc++ && ./testUtils

// For benchmarking:
// clang -O2 -o testUtils server/testUtils.cpp server/monitorUtils.cpp -std=c++11 -lstdc++ && ./testUtils

using namespace std;
using namespace homepower;

template <typename T>
void AssertEqualPrecision(T expected, T actual, T precision) {
	if (std::abs(expected - actual) > precision) {
		printf("Expected:\n  ");
		cout << expected << endl;
		printf("Actual:\n  ");
		cout << actual << endl;
		assert(false);
	}
}

template <typename T>
void AssertEqual(T expected, T actual) {
	if (expected != actual) {
		printf("Expected:\n  ");
		cout << expected << endl;
		printf("Actual:\n  ");
		cout << actual << endl;
		assert(false);
	}
}

void TestTimeInterpolate() {
	{
		const int n    = 2;
		TimePoint t[n] = {TimePoint(6, 15), TimePoint(18, 30)};
		float     v[n] = {30, 85};
		AssertEqualPrecision(TimePoint::Interpolate(TimePoint(6, 15), n, t, v), 30.f, 1.f);
		AssertEqualPrecision(TimePoint::Interpolate(TimePoint(6, 0), n, t, v), 31.f, 1.f);
		AssertEqualPrecision(TimePoint::Interpolate(TimePoint(6, 30), n, t, v), 31.f, 1.f);
		AssertEqualPrecision(TimePoint::Interpolate(TimePoint(13, 0), n, t, v), 60.f, 1.f);
		AssertEqualPrecision(TimePoint::Interpolate(TimePoint(18, 29), n, t, v), 85.f, 1.f);
		AssertEqualPrecision(TimePoint::Interpolate(TimePoint(18, 30), n, t, v), 85.f, 1.f);
		AssertEqualPrecision(TimePoint::Interpolate(TimePoint(18, 31), n, t, v), 85.f, 1.f);
		AssertEqualPrecision(TimePoint::Interpolate(TimePoint(19, 10), n, t, v), 81.f, 1.f);
		AssertEqualPrecision(TimePoint::Interpolate(TimePoint(23, 59), n, t, v), 59.f, 1.f);
		AssertEqualPrecision(TimePoint::Interpolate(TimePoint(6, 14), n, t, v), 30.f, 1.f);
	}
	{
		const int n    = 3;
		TimePoint t[n] = {TimePoint(6, 15), TimePoint(18, 30), TimePoint(22, 20)};
		float     v[n] = {30, 85, 84};
		AssertEqualPrecision(TimePoint::Interpolate(TimePoint(6, 15), n, t, v), 30.f, 0.1f);
		AssertEqualPrecision(TimePoint::Interpolate(TimePoint(18, 30), n, t, v), 85.f, 0.1f);
		AssertEqualPrecision(TimePoint::Interpolate(TimePoint(22, 20), n, t, v), 84.f, 0.1f);
		AssertEqualPrecision(TimePoint::Interpolate(TimePoint(22, 59), n, t, v), 79.5f, 0.1f);
		AssertEqualPrecision(TimePoint::Interpolate(TimePoint(20, 0), n, t, v), 84.6f, 0.1f);
	}
}

Inverter::Record_QPIGS MakeRecordLoadHeavy(time_t t, float loadW, bool heavy) {
	Inverter::Record_QPIGS r;
	r.Time  = t;
	r.LoadW = loadW;
	r.Heavy = heavy;
	return r;
}

void TestHeavyPowerEstimate() {
	RingBuffer<Inverter::Record_QPIGS> records;
	records.Initialize(256);
	RingBuffer<History> loadWHistory;
	loadWHistory.Initialize(256);

	{
		// Too little data
		records.Clear();
		loadWHistory.Clear();
		records.Add(MakeRecordLoadHeavy(0, 100, false));
		records.Add(MakeRecordLoadHeavy(1, 100, true));

		AnalyzeRecentReadings(records, loadWHistory);
		AssertEqual(0u, loadWHistory.Size());

		// At 3+, we can generate data
		records.Add(MakeRecordLoadHeavy(2, 500, true));
		AnalyzeRecentReadings(records, loadWHistory);
		AssertEqual(1u, loadWHistory.Size());
	}
	{
		// Lag
		records.Clear();
		loadWHistory.Clear();
		records.Add(MakeRecordLoadHeavy(0, 100, false));
		records.Add(MakeRecordLoadHeavy(1, 100, true));
		records.Add(MakeRecordLoadHeavy(2, 600, true));

		AnalyzeRecentReadings(records, loadWHistory);
		AssertEqual(1u, loadWHistory.Size());
	}
	{
		// No lag
		records.Clear();
		loadWHistory.Clear();
		records.Add(MakeRecordLoadHeavy(0, 100, false));
		records.Add(MakeRecordLoadHeavy(1, 600, true));
		records.Add(MakeRecordLoadHeavy(2, 150, false));

		// On this sample, we measure the delta from switching heavy loads ON
		AnalyzeRecentReadings(records, loadWHistory);
		AssertEqual(1u, loadWHistory.Size());

		// On this sample, we measure the delta from switching heavy loads OFF,
		records.Add(MakeRecordLoadHeavy(3, 120, false));

		AnalyzeRecentReadings(records, loadWHistory);
		AssertEqual(2u, loadWHistory.Size());
		AssertEqual(500.0f, loadWHistory.Peek(0).Value);
		AssertEqual(450.0f, loadWHistory.Peek(1).Value);
	}
	{
		loadWHistory.Clear();
		AssertEqualPrecision<float>(0, EstimateHeavyLoadWatts(0, loadWHistory), 0);
		AssertEqualPrecision<float>(0, EstimateHeavyLoadWatts(10000, loadWHistory), 0);
		AssertEqualPrecision<float>(0, EstimateHeavyLoadWatts(-10000, loadWHistory), 0);

		loadWHistory.Clear();
		loadWHistory.Add({0, 500});
		loadWHistory.Add({100, 510});
		loadWHistory.Add({190, 490});
		AssertEqualPrecision<float>(490, EstimateHeavyLoadWatts(200, loadWHistory), 10);
		AssertEqualPrecision<float>(490, EstimateHeavyLoadWatts(300, loadWHistory), 10);
		AssertEqualPrecision<float>(122, EstimateHeavyLoadWatts(400, loadWHistory), 1);
		AssertEqualPrecision<float>(0, EstimateHeavyLoadWatts(500, loadWHistory), 1);

		loadWHistory.Clear();
		loadWHistory.Add({0, 10});
		loadWHistory.Add({200, 500});
		loadWHistory.Add({800, 500});                                                   // 600 seconds of 500 watts
		AssertEqualPrecision<float>(500, EstimateHeavyLoadWatts(790, loadWHistory), 1); // time in the past is predicted to equal latest sample
		AssertEqualPrecision<float>(250, EstimateHeavyLoadWatts(800 + 600 + 300, loadWHistory), 10);
		AssertEqualPrecision<float>(0, EstimateHeavyLoadWatts(800 + 600 + 600, loadWHistory), 10);
		AssertEqualPrecision<float>(0, EstimateHeavyLoadWatts(800 + 1000000, loadWHistory), 10);
	}
}

//static int OptBreaker;

void PrintBenchmark(const char* operation, int n, clock_t start, int optimizeBreaker) {
	//OptBreaker   = optimizeBreaker;
	auto   end     = clock();
	auto   elapsed = double(end - start) / (double) CLOCKS_PER_SEC;
	double seconds = elapsed / (double) n;
	if (seconds < 1e-6) {
		printf("%s %.0f nanoseconds per operation (opt breaker %d)\n", operation, 1000000000.f * seconds, optimizeBreaker);
	} else if (seconds < 1e-3) {
		printf("%s %.3f microseconds per operation (opt breaker %d)\n", operation, 1000000.f * seconds, optimizeBreaker);
	} else {
		printf("%s %.3f milliseconds per operation (opt breaker %d)\n", operation, 1000.f * seconds, optimizeBreaker);
	}
}

// On a Raspberry Pi 1, it takes 0.222 milliseconds to compute an average over 4096 samples.
// On a Raspberry Pi 1, it takes 0.038 milliseconds to compute an average over 1024 samples.
void BenchmarkRingBuffer() {
	int                 n    = 1000;
	int                 size = 1024;
	RingBuffer<History> buffer;
	buffer.Initialize(size);
	for (int i = 0; i < size; i++) {
		buffer.Add({i + 100, (float) i});
	}

	auto   start = clock();
	double avg   = 0;
	for (int i = 0; i < n; i++) {
		avg += Average(0, buffer);
	}
	//printf("%f\n", avg);
	PrintBenchmark("Average of 1024 samples", n, start, (int) avg);
}

int main(int argc, char** argv) {
	TestHeavyPowerEstimate();
	BenchmarkRingBuffer();
	TestTimeInterpolate();
	return 0;
}