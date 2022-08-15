#pragma once

#include <assert.h>

// A fixed-size power-of-2 ring buffer.
// Max elements in buffer is size - 1
template <typename T>
class RingBuffer {
public:
	T*       Items = nullptr;
	uint32_t Mask  = 0;
	uint32_t Tail  = 0;
	uint32_t Head  = 0;

	~RingBuffer() {
		Free();
	}

	void Free() {
		delete[] Items;
		Items = nullptr;
		Mask  = 0;
		Tail  = 0;
		Head  = 0;
	}

	// Size must be a power of 2
	void Initialize(uint32_t size) {
		if ((size & (size - 1)) != 0 || size < 2) {
			assert(false && "size must be a power of 2, and minimum 2");
		}
		Free();
		Items = new T[size];
		Mask  = size - 1;
	}

	// Clears all items, but does not free memory
	void Clear() {
		Tail = 0;
		Head = 0;
	}

	// Returns the number of items in the buffer
	uint32_t Size() const {
		return (Head - Tail) & Mask;
	}

	// Returns true if the buffer is full
	bool IsFull() const {
		return Size() == Mask;
	}

	// Peek returns the Tail+i element from the buffer.
	const T& Peek(uint32_t i) const {
		return Items[(Tail + i) & Mask];
	}

	// Next returns the oldest element from the buffer, and pops it.
	T Next() {
		uint32_t t = Tail;
		Tail       = (Tail + 1) & Mask;
		return Items[t & Mask];
	}

	// Add an item.
	// Pop the oldest item if the buffer is full.
	void Add(const T& item) {
		if (IsFull()) {
			Next();
		}
		Items[Head] = item;
		Head        = (Head + 1) & Mask;
	}
};