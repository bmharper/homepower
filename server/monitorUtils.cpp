#include "monitorUtils.h"

namespace homepower {

void AnalyzeRecentReadings(RingBuffer<Inverter::Record_QPIGS>& records, RingBuffer<History>& heavyLoadDeltas) {
	// If we've recently switched heavy loads on/off, then take a sample of the delta,
	// so that we can improve our estimate of the heavy loads wattage.
	if (records.Size() >= 3) {
		// Find records a few samples apart where heavy state differs.
		// The switching on/off of heavy loads is not coordinated with the reading of inverter data,
		// so we need to add some time buffer to ensure we're getting a reliable reading.
		// However, the system will quickly switch heavy loads OFF if it detects that heavy loads
		// are overloading the inverter. We also don't want to miss these events, so we can't just
		// add a large buffer around our sampling. We need to be as precise as possible, and use
		// a time window as small as possible, around the transition point.
		//
		// Data example WITH lag:
		//        time         | loadW | heavy | <variable names used below>
		// 2025-01-28 14:28:19 |   515 | f     |
		// 2025-01-28 14:28:20 |   567 | f     |  <t0>
		// 2025-01-28 14:28:21 |   567 | t     |  <t1>
		// 2025-01-28 14:28:22 |  1841 | t     |  <t2>
		// 2025-01-28 14:28:23 |  1841 | t     |
		// From the above example, we can see that the loadW can lag the heavy state by up to one sample.
		// So we look for the record where we transition, and then we use the reading from one record after that.
		//
		// Data example WITHOUT lag, and where we immediately switched heavy loads off due to overload:
		//        time         | loadW | heavy | <variable names used below>
		// 2025-01-28 14:28:19 |   515 | f     |
		// 2025-01-28 14:28:20 |   567 | f     |  <t0>
		// 2025-01-28 14:28:21 |  5800 | t     |  <t1>
		// 2025-01-28 14:28:22 |   555 | f     |  <t2>
		// 2025-01-28 14:28:23 |   548 | f     |
		// This example data was synthesized. I believe it is a plausible scenario, but I should measure it
		// in the wild to really be sure.
		//
		// We want to support both cases - with and without lag.
		// Our test for whether we have a genuine heavy/not heavy transition is if the load differs by
		// a critical threshold. Luckily for us, if the load doesn't differ by a sufficient threshold,
		// it doesn't actually matter if we get it wrong.
		//
		// The variable names here (t0,t1,t2) can be viewed in the table above to understand the intent.
		uint32_t    n         = records.Size();
		const auto& t0        = records.Peek(n - 3);
		const auto& t1        = records.Peek(n - 2);
		const auto& t2        = records.Peek(n - 1);
		float       delta1    = t1.LoadW - t0.LoadW;
		float       delta2    = t2.LoadW - t0.LoadW;
		float       threshold = 200;
		if ((!t0.Heavy && t1.Heavy && delta1 > threshold) || (t0.Heavy && !t1.Heavy && delta1 < -threshold)) {
			// transition without lag
			heavyLoadDeltas.Add({t1.Time, std::abs(delta1)});
		} else if ((!t0.Heavy && t1.Heavy && delta2 > threshold) || (t0.Heavy && !t1.Heavy && delta2 < -threshold)) {
			// transition with lag
			heavyLoadDeltas.Add({t1.Time, std::abs(delta2)});
		}
	}
}

// Given a buffer of delta measurements, estimate the heavy load wattage.
// The buffer may contain any number of samples, including zero.
// Also, the most recent samples may be very far in the past.
float EstimateHeavyLoadWatts(time_t now, const RingBuffer<History>& deltas) {
	const int maxHistorySeconds = 60 * 60; // don't look further back than 60 minutes
	if (deltas.Size() == 0) {
		return 0;
	}
	// Our first approximation is the most recent measurement
	uint32_t n    = deltas.Size();
	History  last = deltas.Peek(n - 1);

	// Make sure we don't have issues with numerical overflow
	if (now - last.Time > maxHistorySeconds) {
		return 0;
	}

	// We want to decay our most recent measurement linearly over time.
	// But how fast that decay occurs, depends on the measurements going back in time.
	// Walk backwards until we have a sample that is less than 75% of the most recent
	// sample.
	uint32_t i = 2;
	for (; i < n; i++) {
		History sample = deltas.Peek(n - i);
		if (sample.Value < last.Value * 0.75f || last.Time - sample.Time > maxHistorySeconds) {
			break;
		}
	}

	// move i back to last matching sample
	i--;

	// For how many seconds have we observed the same (or more) heavy load wattage?
	int secondsOfSame = last.Time - deltas.Peek(n - i).Time;

	// If we have no history to look back on, then assume that the present
	// observation will hold for the next 2 minutes.
	secondsOfSame = Clamp(secondsOfSame, 2 * 60, maxHistorySeconds);

	// Predict that we'll see the same thing for the next N seconds, and thereafter
	// there will be linear falloff down to zero, which lasts for another N seconds.
	/*
	The following illustration is intended to convey the pattern that we predict:
	|
	|         ,-----------.   
	| ---_,--'             \   <--- Watts
	|                       \
	|                        \
	| ---------+----+----+----+  <--- Time periods
	|     t0     t1 ^ t2   t3
	|               ^
	|               ^
	|              now
	|      
	t0: ignored period where load wattage was significantly less than the most recent observation
	t1: observed heavy load wattage, where amount was similar (or more) than most recent reading
	t2: predicted heavy load wattage (same duration as t1)
	t3: linear decay down to zero (same duration as t1)
	*/
	float decaySeconds = now - (last.Time + secondsOfSame);
	decaySeconds       = Clamp(decaySeconds, 0.0f, (float) secondsOfSame);
	float decay        = 1.0f - decaySeconds / (float) secondsOfSame;
	return last.Value * decay;
}

} // namespace homepower
