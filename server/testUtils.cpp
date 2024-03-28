#include <iostream>
#include <stdio.h>
#include <assert.h>
#include "controllerUtils.h"
#include "ringbuffer.h"
#include "monitorUtils.h"

// For debugging:
// clang -g -o testUtils server/testUtils.cpp -std=c++11 -lstdc++ && ./testUtils

// For benchmarking:
// clang -O2 -o testUtils server/testUtils.cpp -std=c++11 -lstdc++ && ./testUtils

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
	BenchmarkRingBuffer();
	TestTimeInterpolate();
	return 0;
}