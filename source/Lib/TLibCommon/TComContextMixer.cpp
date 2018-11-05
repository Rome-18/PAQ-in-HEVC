#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <ctype.h>
#define NDEBUG  // remove for debugging (turns on Array bound checks)
#include <assert.h>

#ifdef UNIX
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#endif

#ifdef WINDOWS
#include <windows.h>
#endif

#include"TComRom.h"
#include"TComContextMixer.h"

#if NN_BASED_CABAC

#ifndef DEFAULT_OPTION
#define DEFAULT_OPTION 5
#endif

// min, max functions
#ifndef WINDOWS
inline int min(int a, int b)
{
	return a < b ? a : b;
}
inline int max(int a, int b)
{
	return a < b ? b : a;
}
#endif

// Error handler: print message if any, and exit
void quit(const char* message = 0)
{
	throw message;
}

//////////////////////// Program Checker /////////////////////

// Track time and memory used
class ProgramChecker {
	int memused;  // bytes allocated by Array<T> now
	int maxmem;   // most bytes allocated ever
	clock_t start_time;  // in ticks
public:
	void alloc(int n)
	{  // report memory allocated, may be negative
		memused += n;
		if (memused > maxmem) maxmem = memused;
	}
	ProgramChecker() : memused(0), maxmem(0)
	{
		start_time = clock();
		assert(sizeof(U8) == 1);
		assert(sizeof(U16) == 2);
		assert(sizeof(U32) == 4);
		assert(sizeof(short) == 2);
		assert(sizeof(int) == 4);
	}
	void print() const
	{  // print time and memory used
		printf("Time %1.2f sec, used %d bytes of memory\n",
			double(clock() - start_time) / CLOCKS_PER_SEC, maxmem);
	}
} programChecker;

//////////////////////////// Array ////////////////////////////

// Array<T, ALIGN> a(n); creates n elements of T initialized to 0 bits.
// Constructors for T are not called.
// Indexing is bounds checked if assertions are on.
// a.size() returns n.
// a.resize(n) changes size to n, padding with 0 bits or truncating.
// a.push_back(x) appends x and increases size by 1, reserving up to size*2.
// a.pop_back() decreases size by 1, does not free memory.
// Copy and assignment are not supported.
// Memory is aligned on a ALIGN byte boundary (power of 2), default is none.

template <class T, int ALIGN = 0> class newArray {
private:
	int n;     // user size
	int reserved;  // actual size
	char *ptr; // allocated memory, zeroed
	T* data;   // start of n elements of aligned data
	void create(int i);  // create with size i
public:
	explicit newArray(int i = 0)
	{
		create(i);
	}
	~newArray();
	T& operator[](int i)
	{
#ifndef NDEBUG
		if (i < 0 || i >= n) fprintf(stderr, "%d out of bounds %d\n", i, n), quit();
#endif
		return data[i];
	}
	const T& operator[](int i) const
	{
#ifndef NDEBUG
		if (i < 0 || i >= n) fprintf(stderr, "%d out of bounds %d\n", i, n), quit();
#endif
		return data[i];
	}
	int size() const
	{
		return n;
	}
	void resize(int i);  // change size to i
	void pop_back()
	{
		if (n > 0) --n;
	}  // decrement size
	void push_back(const T& x);  // increment size, append x
private:
	newArray(const newArray&);  // no copy or assignment
	newArray& operator=(const newArray&);
};

template<class T, int ALIGN> void newArray<T, ALIGN>::resize(int i)
{
	if (i <= reserved) {
		n = i;
		return;
	}
	char *saveptr = ptr;
	T *savedata = data;
	int saven = n;
	create(i);
	if (saveptr) {
		if (savedata) {
			memcpy(data, savedata, sizeof(T)*min(i, saven));
			programChecker.alloc(-ALIGN - n*sizeof(T));
		}
		free(saveptr);
	}
}

template<class T, int ALIGN> void newArray<T, ALIGN>::create(int i)
{
	n = reserved = i;
	if (i <= 0) {
		data = 0;
		ptr = 0;
		return;
	}
	const int sz = ALIGN + n*sizeof(T);
	programChecker.alloc(sz);
	ptr = (char*)calloc(sz, 1);
	if (!ptr) quit("Out of memory");
	data = (ALIGN ? (T*)(ptr + ALIGN - (((long)ptr)&(ALIGN - 1))) : (T*)ptr);
	assert((char*)data >= ptr && (char*)data <= ptr + ALIGN);
}

template<class T, int ALIGN> newArray<T, ALIGN>::~newArray()
{
	programChecker.alloc(-ALIGN - n*sizeof(T));
	free(ptr);
}

template<class T, int ALIGN> void newArray<T, ALIGN>::push_back(const T& x)
{
	if (n == reserved) {
		int saven = n;
		resize(max(1, n * 2));
		n = saven;
	}
	data[n++] = x;
}

/////////////////////////// String /////////////////////////////

// A tiny subset of std::string
// size() includes NUL terminator.

class String : public newArray<char> {
public:
	const char* c_str() const
	{
		return &(*this)[0];
	}
	void operator=(const char* s)
	{
		resize(strlen(s) + 1);
		strcpy(&(*this)[0], s);
	}
	void operator+=(const char* s)
	{
		assert(s);
		pop_back();
		while (*s) push_back(*s++);
		push_back(0);
	}
	String(const char* s = "") : newArray<char>(1)
	{
		(*this) += s;
	}
};


//////////////////////////// rnd ///////////////////////////////

// 32-bit pseudo random number generator
class Random {
	newArray<U32> table;
	int i;
public:
	Random() : table(64)
	{
		table[0] = 123456789;
		table[1] = 987654321;
		for (int j = 0; j < 62; j++) table[j + 2] = table[j + 1] * 11 + table[j] * 23 / 16;
		i = 0;
	}
	U32 operator()()
	{
		return ++i, table[i & 63] = table[i - 24 & 63] ^ table[i - 55 & 63];
	}
} rnd;

////////////////////////////// Buf /////////////////////////////

// Buf(n) buf; creates an array of n bytes (must be a power of 2).
// buf[i] returns a reference to the i'th byte with wrap (no out of bounds).
// buf(i) returns i'th byte back from pos (i > 0) 
// buf.size() returns n.

int pos;  // Number of input bytes in buf (not wrapped)

class Buf {
	newArray<U8> b;
public:
#if 1 // bug_fix 
	Buf(int i = 16777216) : b(i)
	{
	}
#else
	Buf(int i = 0) : b(i)
	{
	}
#endif 
	void setsize(int i)
	{
		if (!i) return;
		assert(i > 0 && (i&(i - 1)) == 0);
		b.resize(i);
	}
	U8& operator[](int i)
	{
		return b[i&b.size() - 1];
	}
	int operator()(int i) const
	{
		assert(i > 0);
		return b[pos - i&b.size() - 1];
	}
	int size() const
	{
		return b.size();
	}
};

// IntBuf(n) is a buffer of n int (must be a power of 2).
// intBuf[i] returns a reference to i'th element with wrap.

class IntBuf {
	newArray<int> b;
public:
	IntBuf(int i = 0) : b(i)
	{
	}
	int& operator[](int i)
	{
		return b[i&b.size() - 1];
	}
};

/////////////////////// Global context /////////////////////////

//int level = DEFAULT_OPTION;  // Compression level 0 to 9
//#define MEM (0x10000<<level)
//int y = 0;  // Last bit, 0 or 1, set by encoder

// Global context set by Predictor and available to all models.
int c0 = 1; // Last 0-7 bits of the partial byte with a leading 1 bit (1-255)
U32 c4 = 0; // Last 4 whole bytes, packed.  Last byte is bits 0-7.
int bpos = 0; // bits in c0 (0 to 7)
Buf buf;  // Rotating input queue set by Predictor

///////////////////////////// ilog //////////////////////////////

// ilog(x) = round(log2(x) * 16), 0 <= x < 64K
class Ilog {
	newArray<U8> t;
public:
	int operator()(U16 x) const
	{
		return t[x];
	}
	Ilog();
} ilog;

// Compute lookup table by numerical integration of 1/x
Ilog::Ilog() : t(65536)
{
	U32 x = 14155776;
	for (int i = 2; i < 65536; ++i) {
		x += 774541002 / (i * 2 - 1);  // numerator is 2^29/ln 2
		t[i] = x >> 24;
	}
}

// llog(x) accepts 32 bits
inline int llog(U32 x)
{
	if (x >= 0x1000000)
		return 256 + ilog(x >> 16);
	else if (x >= 0x10000)
		return 128 + ilog(x >> 8);
	else
		return ilog(x);
}

static const U8 State_table[256][4] = {
	{ 1, 2, 0, 0 }, { 3, 5, 1, 0 }, { 4, 6, 0, 1 }, { 7, 10, 2, 0 }, // 0-3
	{ 8, 12, 1, 1 }, { 9, 13, 1, 1 }, { 11, 14, 0, 2 }, { 15, 19, 3, 0 }, // 4-7
	{ 16, 23, 2, 1 }, { 17, 24, 2, 1 }, { 18, 25, 2, 1 }, { 20, 27, 1, 2 }, // 8-11
	{ 21, 28, 1, 2 }, { 22, 29, 1, 2 }, { 26, 30, 0, 3 }, { 31, 33, 4, 0 }, // 12-15
	{ 32, 35, 3, 1 }, { 32, 35, 3, 1 }, { 32, 35, 3, 1 }, { 32, 35, 3, 1 }, // 16-19
	{ 34, 37, 2, 2 }, { 34, 37, 2, 2 }, { 34, 37, 2, 2 }, { 34, 37, 2, 2 }, // 20-23
	{ 34, 37, 2, 2 }, { 34, 37, 2, 2 }, { 36, 39, 1, 3 }, { 36, 39, 1, 3 }, // 24-27
	{ 36, 39, 1, 3 }, { 36, 39, 1, 3 }, { 38, 40, 0, 4 }, { 41, 43, 5, 0 }, // 28-31
	{ 42, 45, 4, 1 }, { 42, 45, 4, 1 }, { 44, 47, 3, 2 }, { 44, 47, 3, 2 }, // 32-35
	{ 46, 49, 2, 3 }, { 46, 49, 2, 3 }, { 48, 51, 1, 4 }, { 48, 51, 1, 4 }, // 36-39
	{ 50, 52, 0, 5 }, { 53, 43, 6, 0 }, { 54, 57, 5, 1 }, { 54, 57, 5, 1 }, // 40-43
	{ 56, 59, 4, 2 }, { 56, 59, 4, 2 }, { 58, 61, 3, 3 }, { 58, 61, 3, 3 }, // 44-47
	{ 60, 63, 2, 4 }, { 60, 63, 2, 4 }, { 62, 65, 1, 5 }, { 62, 65, 1, 5 }, // 48-51
	{ 50, 66, 0, 6 }, { 67, 55, 7, 0 }, { 68, 57, 6, 1 }, { 68, 57, 6, 1 }, // 52-55
	{ 70, 73, 5, 2 }, { 70, 73, 5, 2 }, { 72, 75, 4, 3 }, { 72, 75, 4, 3 }, // 56-59
	{ 74, 77, 3, 4 }, { 74, 77, 3, 4 }, { 76, 79, 2, 5 }, { 76, 79, 2, 5 }, // 60-63
	{ 62, 81, 1, 6 }, { 62, 81, 1, 6 }, { 64, 82, 0, 7 }, { 83, 69, 8, 0 }, // 64-67
	{ 84, 71, 7, 1 }, { 84, 71, 7, 1 }, { 86, 73, 6, 2 }, { 86, 73, 6, 2 }, // 68-71
	{ 44, 59, 5, 3 }, { 44, 59, 5, 3 }, { 58, 61, 4, 4 }, { 58, 61, 4, 4 }, // 72-75
	{ 60, 49, 3, 5 }, { 60, 49, 3, 5 }, { 76, 89, 2, 6 }, { 76, 89, 2, 6 }, // 76-79
	{ 78, 91, 1, 7 }, { 78, 91, 1, 7 }, { 80, 92, 0, 8 }, { 93, 69, 9, 0 }, // 80-83
	{ 94, 87, 8, 1 }, { 94, 87, 8, 1 }, { 96, 45, 7, 2 }, { 96, 45, 7, 2 }, // 84-87
	{ 48, 99, 2, 7 }, { 48, 99, 2, 7 }, { 88, 101, 1, 8 }, { 88, 101, 1, 8 }, // 88-91
	{ 80, 102, 0, 9 }, { 103, 69, 10, 0 }, { 104, 87, 9, 1 }, { 104, 87, 9, 1 }, // 92-95
	{ 106, 57, 8, 2 }, { 106, 57, 8, 2 }, { 62, 109, 2, 8 }, { 62, 109, 2, 8 }, // 96-99
	{ 88, 111, 1, 9 }, { 88, 111, 1, 9 }, { 80, 112, 0, 10 }, { 113, 85, 11, 0 }, // 100-103
	{ 114, 87, 10, 1 }, { 114, 87, 10, 1 }, { 116, 57, 9, 2 }, { 116, 57, 9, 2 }, // 104-107
	{ 62, 119, 2, 9 }, { 62, 119, 2, 9 }, { 88, 121, 1, 10 }, { 88, 121, 1, 10 }, // 108-111
	{ 90, 122, 0, 11 }, { 123, 85, 12, 0 }, { 124, 97, 11, 1 }, { 124, 97, 11, 1 }, // 112-115
	{ 126, 57, 10, 2 }, { 126, 57, 10, 2 }, { 62, 129, 2, 10 }, { 62, 129, 2, 10 }, // 116-119
	{ 98, 131, 1, 11 }, { 98, 131, 1, 11 }, { 90, 132, 0, 12 }, { 133, 85, 13, 0 }, // 120-123
	{ 134, 97, 12, 1 }, { 134, 97, 12, 1 }, { 136, 57, 11, 2 }, { 136, 57, 11, 2 }, // 124-127
	{ 62, 139, 2, 11 }, { 62, 139, 2, 11 }, { 98, 141, 1, 12 }, { 98, 141, 1, 12 }, // 128-131
	{ 90, 142, 0, 13 }, { 143, 95, 14, 0 }, { 144, 97, 13, 1 }, { 144, 97, 13, 1 }, // 132-135
	{ 68, 57, 12, 2 }, { 68, 57, 12, 2 }, { 62, 81, 2, 12 }, { 62, 81, 2, 12 }, // 136-139
	{ 98, 147, 1, 13 }, { 98, 147, 1, 13 }, { 100, 148, 0, 14 }, { 149, 95, 15, 0 }, // 140-143
	{ 150, 107, 14, 1 }, { 150, 107, 14, 1 }, { 108, 151, 1, 14 }, { 108, 151, 1, 14 }, // 144-147
	{ 100, 152, 0, 15 }, { 153, 95, 16, 0 }, { 154, 107, 15, 1 }, { 108, 155, 1, 15 }, // 148-151
	{ 100, 156, 0, 16 }, { 157, 95, 17, 0 }, { 158, 107, 16, 1 }, { 108, 159, 1, 16 }, // 152-155
	{ 100, 160, 0, 17 }, { 161, 105, 18, 0 }, { 162, 107, 17, 1 }, { 108, 163, 1, 17 }, // 156-159
	{ 110, 164, 0, 18 }, { 165, 105, 19, 0 }, { 166, 117, 18, 1 }, { 118, 167, 1, 18 }, // 160-163
	{ 110, 168, 0, 19 }, { 169, 105, 20, 0 }, { 170, 117, 19, 1 }, { 118, 171, 1, 19 }, // 164-167
	{ 110, 172, 0, 20 }, { 173, 105, 21, 0 }, { 174, 117, 20, 1 }, { 118, 175, 1, 20 }, // 168-171
	{ 110, 176, 0, 21 }, { 177, 105, 22, 0 }, { 178, 117, 21, 1 }, { 118, 179, 1, 21 }, // 172-175
	{ 110, 180, 0, 22 }, { 181, 115, 23, 0 }, { 182, 117, 22, 1 }, { 118, 183, 1, 22 }, // 176-179
	{ 120, 184, 0, 23 }, { 185, 115, 24, 0 }, { 186, 127, 23, 1 }, { 128, 187, 1, 23 }, // 180-183
	{ 120, 188, 0, 24 }, { 189, 115, 25, 0 }, { 190, 127, 24, 1 }, { 128, 191, 1, 24 }, // 184-187
	{ 120, 192, 0, 25 }, { 193, 115, 26, 0 }, { 194, 127, 25, 1 }, { 128, 195, 1, 25 }, // 188-191
	{ 120, 196, 0, 26 }, { 197, 115, 27, 0 }, { 198, 127, 26, 1 }, { 128, 199, 1, 26 }, // 192-195
	{ 120, 200, 0, 27 }, { 201, 115, 28, 0 }, { 202, 127, 27, 1 }, { 128, 203, 1, 27 }, // 196-199
	{ 120, 204, 0, 28 }, { 205, 115, 29, 0 }, { 206, 127, 28, 1 }, { 128, 207, 1, 28 }, // 200-203
	{ 120, 208, 0, 29 }, { 209, 125, 30, 0 }, { 210, 127, 29, 1 }, { 128, 211, 1, 29 }, // 204-207
	{ 130, 212, 0, 30 }, { 213, 125, 31, 0 }, { 214, 137, 30, 1 }, { 138, 215, 1, 30 }, // 208-211
	{ 130, 216, 0, 31 }, { 217, 125, 32, 0 }, { 218, 137, 31, 1 }, { 138, 219, 1, 31 }, // 212-215
	{ 130, 220, 0, 32 }, { 221, 125, 33, 0 }, { 222, 137, 32, 1 }, { 138, 223, 1, 32 }, // 216-219
	{ 130, 224, 0, 33 }, { 225, 125, 34, 0 }, { 226, 137, 33, 1 }, { 138, 227, 1, 33 }, // 220-223
	{ 130, 228, 0, 34 }, { 229, 125, 35, 0 }, { 230, 137, 34, 1 }, { 138, 231, 1, 34 }, // 224-227
	{ 130, 232, 0, 35 }, { 233, 125, 36, 0 }, { 234, 137, 35, 1 }, { 138, 235, 1, 35 }, // 228-231
	{ 130, 236, 0, 36 }, { 237, 125, 37, 0 }, { 238, 137, 36, 1 }, { 138, 239, 1, 36 }, // 232-235
	{ 130, 240, 0, 37 }, { 241, 125, 38, 0 }, { 242, 137, 37, 1 }, { 138, 243, 1, 37 }, // 236-239
	{ 130, 244, 0, 38 }, { 245, 135, 39, 0 }, { 246, 137, 38, 1 }, { 138, 247, 1, 38 }, // 240-243
	{ 140, 248, 0, 39 }, { 249, 135, 40, 0 }, { 250, 69, 39, 1 }, { 80, 251, 1, 39 }, // 244-247
	{ 140, 252, 0, 40 }, { 249, 135, 41, 0 }, { 250, 69, 40, 1 }, { 80, 251, 1, 40 }, // 248-251
	{ 140, 252, 0, 41 } };  // 252, 253-255 are reserved

#define nex(state,sel) State_table[state][sel]


///////////////////////////// Squash //////////////////////////////

// return p = 1/(1 + exp(-d)), d scaled by 8 bits, p scaled by 12 bits
int squash(int d)
{
	static const int t[33] = {
		1, 2, 3, 6, 10, 16, 27, 45, 73, 120, 194, 310, 488, 747, 1101,
		1546, 2047, 2549, 2994, 3348, 3607, 3785, 3901, 3975, 4022,
		4050, 4068, 4079, 4085, 4089, 4092, 4093, 4094 };
	if (d > 2047) return 4095;
	if (d < -2047) return 0;
	int w = d & 127;
	d = (d >> 7) + 16;
	return (t[d] * (128 - w) + t[(d + 1)] * w + 64) >> 7;
}

//////////////////////////// Stretch ///////////////////////////////

// Inverse of squash. d = ln(p/(1-p)), d scaled by 8 bits, p by 12 bits.
// d has range -2047 to 2047 representing -8 to 8.  p has range 0 to 4095.

class Stretch {
	newArray<short> t;
public:
	Stretch();
	int operator()(int p) const
	{
		assert(p >= 0 && p < 4096);
		return t[p];
	}
} stretch;

Stretch::Stretch() : t(4096)
{
	int pi = 0;
	for (int x = -2047; x <= 2047; ++x) {  // invert squash()
		int i = squash(x);
		for (int j = pi; j <= i; ++j)
			t[j] = x;
		pi = i + 1;
	}
	t[4095] = 2047;
}

//////////////////////////// Mixer /////////////////////////////

// Mixer m(N, M, S=1, w=0) combines models using M neural networks with
//   N inputs each, of which up to S may be selected.  If S > 1 then
//   the outputs of these neural networks are combined using another
//   neural network (with parameters S, 1, 1).  If S = 1 then the
//   output is direct.  The weights are initially w (+-32K).
//   It is used as follows:
// m.update() trains the network where the expected output is the
//   last bit (in the global variable y).
// m.add(stretch(p)) inputs prediction from one of N models.  The
//   prediction should be positive to predict a 1 bit, negative for 0,
//   nominally +-256 to +-2K.  The maximum allowed value is +-32K but
//   using such large values may cause overflow if N is large.
// m.set(cxt, range) selects cxt as one of 'range' neural networks to
//   use.  0 <= cxt < range.  Should be called up to S times such
//   that the total of the ranges is <= M.
// 
// m.p() returns the output prediction that the next bit is 1 as a
//   12 bit number (0 to 4095).
// m.p()���ص�����һ��bit��1�ĸ��ʣ�12-bit��

// dot_product returns dot product t*w of n elements.  n is rounded
// up to a multiple of 8.  Result is scaled down by 8 bits.
int dot_product(short *t, short *w, int n)
{
	int sum = 0;
	n = (n + 7)&-8;
	for (int i = 0; i < n; i += 2)
		sum += (t[i] * w[i] + t[i + 1] * w[i + 1]) >> 8;
	return sum;
}

// Train neural network weights w[n] given inputs t[n] and err.
// w[i] += t[i]*err, i=0..n-1.  t, w, err are signed 16 bits (+- 32K).
// err is scaled 16 bits (representing +- 1/2).  w[i] is clamped to +- 32K
// and rounded.  n is rounded up to a multiple of 8.
void train(short *t, short *w, int n, int err)
{
	n = (n + 7)&-8;
	for (int i = 0; i < n; ++i) {
		int wt = w[i] + ((t[i] * err * 2 >> 16) + 1 >> 1);
		if (wt<-32768) wt = -32768;
		if (wt>32767) wt = 32767;
		w[i] = wt;
	}
}

//->! ��ʼ�ص�ѧϰ�ĵط�
class Mixer {
	const int N, M, S;   // max inputs, max contexts, max context sets
	newArray<short, 16> tx; // N inputs from add()  //!!!!!->!! N��������ڵ���Ϊinput���൱��si
	newArray<short, 16> wx; // N*M weights  //Ȩ������
	newArray<int> cxt;  // S contexts  //
	int ncxt;        // number of contexts (0 to S)     //!!!!!->!!�ܹ��ο��������ĵĸ���
	int base;        // offset of next context        /// 
	int nx;          // Number of inputs in tx, 0 to N  //�ڵ����
	newArray<int> pr;   // last result (scaled 12 bits)   //!!!!!->!!��������Ҫ�õ������ϸ���
	Mixer* mp;       // points to a Mixer to combine results
public:
	Mixer(int n, int m, int s = 1, int w = 0);

	// Adjust weights to minimize coding cost of last prediction
	void update()
	{
		for (int i = 0; i < ncxt; ++i) {
			int err = ((y << 12) - pr[i]) * 7;
			assert(err >= -32768 && err < 32768);
			train(&tx[0], &wx[cxt[i] * N], nx, err);
		}
		nx = base = ncxt = 0;
	}

	// Input x (call up to N times)
	void add(int x)
	{
		assert(nx < N);
		tx[nx++] = x;
	}

	// Set a context (call S times, sum of ranges <= M)
	void set(int cx, int range)
	{  ///ѡ��cxt as one of 'range' neural networks to
		// use�� range ����ȡֵΪ1
		assert(range >= 0);
		assert(ncxt < S);
		assert(cx >= 0);
		assert(base + cx < M);
		cxt[ncxt++] = base + cx;
		base += range;
	}

	// predict next bit
	int p()
	{
		while (nx & 7) tx[nx++] = 0;  // pad
		if (mp) {  // combine outputs
			mp->update();
			for (int i = 0; i < ncxt; ++i) {
				pr[i] = squash(dot_product(&tx[0], &wx[cxt[i] * N], nx) >> 5);
				mp->add(stretch(pr[i]));  //add output from one of N models tx[nx++] = 
			}
			mp->set(0, 1);
			return mp->p();
		}
		else {  // S=1 context   //ֻ��һ��context�����
			return pr[0] = squash(dot_product(&tx[0], &wx[0], nx) >> 8);
		}
	}
	~Mixer();
};

Mixer::~Mixer()
{
	delete mp;
}


Mixer::Mixer(int n, int m, int s, int w) :
N((n + 7)&-8), M(m), S(s), tx(N), wx(N*M),
cxt(S), ncxt(0), base(0), nx(0), pr(S), mp(0)
{
	assert(n > 0 && N > 0 && (N & 7) == 0 && M > 0);
	for (int i = 0; i < S; ++i)
		pr[i] = 2048;
	for (int i = 0; i<N*M; ++i)
		wx[i] = w;
	if (S>1) mp = new Mixer(S, 1, 1, 0x7fff);
}

//////////////////////////// APM //////////////////////////////
//�õ���ϸ���Pr֮��һ�㻹Ҫ��������APM��Adaptive Probability Map����
//Ҳ���Ƕ����źŹ��ƣ�SSE�����Ը��������Բ�ֵ���������õ�һ�����Ӿ�ȷ��Ԥ����
// APM maps a probability and a context into a new probability
// that bit y will next be 1.  After each guess it updates
// its state to improve future guesses.  Methods:
//
// APM a(N) creates with N contexts, uses 66*N bytes memory.
// a.p(pr, cx, rate=7) returned adjusted probability in context cx (0 to
//   N-1).  rate determines the learning rate (smaller = faster, default 7).
//   Probabilities are scaled 12 bits (0-4095).

class APM {
	int index;     // last p, context  ��һ��p���ֵ�����
	const int N;   // number of contexts
	newArray<U16> t;  // [N][33]:  p, context -> p
public:
	APM(int n);
	int p(int pr = 2048, int cxt = 0, int rate = 7)
	{
		assert(pr >= 0 && pr < 4096 && cxt >= 0 && cxt<N && rate>0 && rate < 32);
		pr = stretch(pr);
		int g = (y << 16) + (y << rate) - y - y;
		t[index] += g - t[index] >> rate;
		t[index + 1] += g - t[index + 1] >> rate;
		const int w = pr & 127;  // interpolation weight (33 points)
		index = (pr + 2048 >> 7) + cxt * 33;
		return t[index] * (128 - w) + t[index + 1] * w >> 11;
	}
};

// maps p, cxt -> p initially
APM::APM(int n) : index(0), N(n), t(n * 33)
{
	for (int i = 0; i < N; ++i)
	for (int j = 0; j < 33; ++j)
		t[i * 33 + j] = i == 0 ? squash((j - 16) * 128) * 16 : t[j];
}

//////////////////////////// StateMap //////////////////////////
//���ڽ�ĳ��״̬�����������ģ�ӳ��õ�ģ�͵�Ԥ����ʣ�
//�ò����Ƕ�ÿһ��������ά��һ�����ʣ���ʼֵΪ0.5
//Ȼ��ĳ�������ĳ���ʱ��������һ�����bit y��Ԥ�����֮���������Ԥ����ʣ�
// p = p + ��*(y - p); ���Ц���learning rate
//��
// A StateMap maps a nonstationary counter state to a probability.
// After each mapping, the mapping is adjusted to improve future
// predictions.  Methods:
//
// sm.p(cx) converts state cx (0-255) to a probability (0-4095).

// Counter state -> probability * 4096
class StateMap {
protected:
	int cxt;  // context
	newArray<U16> t; // 256 states -> probability * 64K
public:
	StateMap();
	int p(int cx)
	{
		assert(cx >= 0 && cx < t.size());
		t[cxt] += (y << 16) - t[cxt] + 128 >> 8;
		return t[cxt = cx] >> 4;
	}
};

StateMap::StateMap() : cxt(0), t(256)
{
	for (int i = 0; i < 256; ++i) {
		int n0 = nex(i, 2);
		int n1 = nex(i, 3);
		if (n0 == 0) n1 *= 64;
		if (n1 == 0) n0 *= 64;
		t[i] = 65536 * (n1 + 1) / (n0 + n1 + 2);
	}
}

//////////////////////////// hash //////////////////////////////

// Hash 2-5 ints.
inline U32 hash(U32 a, U32 b, U32 c = 0xffffffff, U32 d = 0xffffffff,
	U32 e = 0xffffffff)
{
	U32 h = a * 200002979u + b * 30005491u + c * 50004239u + d * 70004807u + e * 110002499u;
	return h^h >> 9 ^ a >> 2 ^ b >> 3 ^ c >> 4 ^ d >> 5 ^ e >> 6;
}

///////////////////////////// BH ////////////////////////////////

// A BH maps a 32 bit hash to an array of B bytes (checksum and B-2 values)
//
// BH bh(N); creates N element table with B bytes each.
//   N must be a power of 2.  The first byte of each element is
//   reserved for a checksum to detect collisions.  The remaining
//   B-1 bytes are values, prioritized by the first value.  This
//   byte is 0 to mark an unused element.
//   
// bh[i] returns a pointer to the i'th element, such that
//   bh[i][0] is a checksum of i, bh[i][1] is the priority, and
//   bh[i][2..B-1] are other values (0-255).
//   The low lg(n) bits as an index into the table.
//   If a collision is detected, up to M nearby locations in the same
//   cache line are tested and the first matching checksum or
//   empty element is returned.
//   If no match or empty element is found, then the lowest priority
//   element is replaced.

// 2 byte checksum with LRU replacement (except last 2 by priority)
template <int B> class BH {
	enum {
		M = 8
	};  // search limit
	newArray<U8, 64> t; // elements
	U32 n; // size-1
public:
	BH(int i) : t(i*B), n(i - 1)
	{
		assert(B >= 2 && i > 0 && (i&(i - 1)) == 0); // size a power of 2?
	}
	U8* operator[](U32 i);
};

template <int B>
inline  U8* BH<B>::operator[](U32 i)
{
	int chk = (i >> 16 ^ i) & 0xffff;
	i = i*M&n;
	U8 *p;
	U16 *cp;
	int j;
	for (j = 0; j < M; ++j) {
		p = &t[(i + j)*B];
		cp = (U16*)p;
		if (p[2] == 0) *cp = chk;
		if (*cp == chk) break;  // found
	}
	if (j == 0) return p + 1;  // front
	static U8 tmp[B];  // element to move to front
	if (j == M) {
		--j;
		memset(tmp, 0, B);
		*(U16*)tmp = chk;
		if (M > 2 && t[(i + j)*B + 2] > t[(i + j - 1)*B + 2]) --j;
	}
	else memcpy(tmp, cp, B);
	memmove(&t[(i + 1)*B], &t[i*B], j*B);
	memcpy(&t[i*B], tmp, B);
	return &t[i*B + 1];
}

/////////////////////////// ContextMap /////////////////////////
//
// A ContextMap maps contexts to a bit histories and makes predictions
// to a Mixer.  Methods common to all classes:
//
// ContextMap cm(M, C); creates using about M bytes of memory (a power
//   of 2) for C contexts.
// cm.set(cx);  sets the next context to cx, called up to C times
//   cx is an arbitrary 32 bit value that identifies the context.
//   It should be called before predicting the first bit of each byte.
// cm.mix(m) updates Mixer m with the next prediction.  Returns 1
//   if context cx is found, else 0.  Then it extends all the contexts with
//   global bit y.  It should be called for every bit:
//
//     if (bpos==0) 
//       for (int i=0; i<C; ++i) cm.set(cxt[i]);
//     cm.mix(m);
//
// The different types are as follows:
//
// - RunContextMap.  The bit history is a count of 0-255 consecutive
//     zeros or ones.  Uses 4 bytes per whole byte context.  C=1.
//     The context should be a hash.
// - SmallStationaryContextMap.  0 <= cx < M/512.
//     The state is a 16-bit probability that is adjusted after each
//     prediction.  C=1.
// - ContextMap.  For large contexts, C >= 1.  Context need not be hashed.

// Predict to mixer m from bit history state s, using sm to map s to
// a probability.
inline int mix2(Mixer& m, int s, StateMap& sm)
{
	int p1 = sm.p(s);
	int n0 = -!nex(s, 2);
	int n1 = -!nex(s, 3);
	int st = stretch(p1) >> 2;
	m.add(st);
	p1 >>= 4;
	int p0 = 255 - p1;
	m.add(p1 - p0);
	m.add(st*(n1 - n0));
	m.add((p1&n0) - (p0&n1));
	m.add((p1&n1) - (p0&n0));
	return s > 0;
}

// A RunContextMap maps a context into the next byte and a repeat
// count up to M.  Size should be a power of 2.  Memory usage is 3M/4.
class RunContextMap {
	BH<4> t;
	U8* cp;
public:
	RunContextMap(int m) : t(m / 4)
	{
		cp = t[0] + 1;
	}
	void set(U32 cx)
	{  // update count
		if (cp[0] == 0 || cp[1] != buf(1)) cp[0] = 1, cp[1] = buf(1);
		else if (cp[0] < 255) ++cp[0];
		cp = t[cx] + 1;
	}
	int p()
	{  // predict next bit
		if (cp[1] + 256 >> 8 - bpos == c0)
			return ((cp[1] >> 7 - bpos & 1) * 2 - 1)*ilog(cp[0] + 1) * 8;
		else
			return 0;
	}
	int mix(Mixer& m)
	{  // return run length
		m.add(p());
		return cp[0] != 0;
	}
};

// Context is looked up directly.  m=size is power of 2 in bytes.
// Context should be < m/512.  High bits are discarded.
class SmallStationaryContextMap {
	newArray<U16> t;
	int cxt;
	U16 *cp;
public:
	SmallStationaryContextMap(int m) : t(m / 2), cxt(0)
	{
		assert((m / 2 & m / 2 - 1) == 0); // power of 2?
		for (int i = 0; i < t.size(); ++i)
			t[i] = 32768;
		cp = &t[0];
	}
	void set(U32 cx)
	{
		cxt = cx * 256 & t.size() - 256;
	}
	void mix(Mixer& m, int rate = 7)
	{
		*cp += (y << 16) - *cp + (1 << rate - 1) >> rate;
		cp = &t[cxt + c0];
		m.add(stretch(*cp >> 4));
	}
};

// Context map for large contexts.  Most modeling uses this type of context
// map.  It includes a built in RunContextMap to predict the last byte seen
// in the same context, and also bit-level contexts that map to a bit
// history state.
//
// Bit histories are stored in a hash table.  The table is organized into
// 64-byte buckets alinged on cache page boundaries.  Each bucket contains
// a hash chain of 7 elements, plus a 2 element queue (packed into 1 byte) 
// of the last 2 elements accessed for LRU replacement.  Each element has
// a 2 byte checksum for detecting collisions, and an array of 7 bit history
// states indexed by the last 0 to 2 bits of context.  The buckets are indexed
// by a context ending after 0, 2, or 5 bits of the current byte.  Thus, each
// byte modeled results in 3 main memory accesses per context, with all other
// accesses to cache.
//
// On bits 0, 2 and 5, the context is updated and a new bucket is selected.
// The most recently accessed element is tried first, by comparing the
// 16 bit checksum, then the 7 elements are searched linearly.  If no match
// is found, then the element with the lowest priority among the 5 elements 
// not in the LRU queue is replaced.  After a replacement, the queue is
// emptied (so that consecutive misses favor a LFU replacement policy).
// In all cases, the found/replaced element is put in the front of the queue.
//
// The priority is the state number of the first element (the one with 0
// additional bits of context).  The states are sorted by increasing n0+n1
// (number of bits seen), implementing a LFU replacement policy.
//
// When the context ends on a byte boundary (bit 0), only 3 of the 7 bit
// history states are used.  The remaining 4 bytes implement a run model
// as follows: <count:7,d:1> <b1> <unused> <unused> where <b1> is the last byte
// seen, possibly repeated.  <count:7,d:1> is a 7 bit count and a 1 bit
// flag (represented by count * 2 + d).  If d=0 then <count> = 1..127 is the 
// number of repeats of <b1> and no other bytes have been seen.  If d is 1 then 
// other byte values have been seen in this context prior to the last <count> 
// copies of <b1>.
//
// As an optimization, the last two hash elements of each byte (representing
// contexts with 2-7 bits) are not updated until a context is seen for
// a second time.  This is indicated by <count,d> = <1,0> (2).  After update,
// <count,d> is updated to <2,0> or <1,1> (4 or 3).

class ContextMap {
	const int C;  // max number of contexts
	class E {  // hash element, 64 bytes
		U16 chk[7];  // byte context checksums
		U8 last;     // last 2 accesses (0-6) in low, high nibble
	public:
		U8 bh[7][7]; // byte context, 3-bit context -> bit history state
		// bh[][0] = 1st bit, bh[][1,2] = 2nd bit, bh[][3..6] = 3rd bit
		// bh[][0] is also a replacement priority, 0 = empty
		U8* get(U16 chk);  // Find element (0-6) matching checksum.
		// If not found, insert or replace lowest priority (not last).
	};
	newArray<E, 64> t;  // bit histories for bits 0-1, 2-4, 5-7
	// For 0-1, also contains a run count in bh[][4] and value in bh[][5]
	// and pending update count in bh[7]
	newArray<U8*> cp;   // C pointers to current bit history
	newArray<U8*> cp0;  // First element of 7 element array containing cp[i]
	newArray<U32> cxt;  // C whole byte contexts (hashes)
	newArray<U8*> runp; // C [0..3] = count, value, unused, unused
	StateMap *sm;    // C maps of state -> p
	int cn;          // Next context to set by set()
	void update(U32 cx, int c);  // train model that context cx predicts c
	int mix1(Mixer& m, int cc, int bp, int c1, int y1);
	// mix() with global context passed as arguments to improve speed.
public:
	ContextMap(int m, int c = 1);  // m = memory in bytes, a power of 2, C = c
	~ContextMap();
	void set(U32 cx, int next = -1);   // set next whole byte context to cx
	// if next is 0 then set order does not matter
	int mix(Mixer& m)
	{
		return mix1(m, c0, bpos, buf(1), y);
	}
};

// Find or create hash element matching checksum ch
inline U8* ContextMap::E::get(U16 ch)
{
	if (chk[last & 15] == ch) return &bh[last & 15][0];
	int b = 0xffff, bi = 0;
	for (int i = 0; i < 7; ++i) {
		if (chk[i] == ch) return last = last << 4 | i, &bh[i][0];
		int pri = bh[i][0];
		if ((last & 15) != i && last >> 4 != i && pri < b) b = pri, bi = i;
	}
	return last = 0xf0 | bi, chk[bi] = ch, (U8*)memset(&bh[bi][0], 0, 7);
}

// Construct using m bytes of memory for c contexts
ContextMap::ContextMap(int m, int c) : C(c), t(m >> 6), cp(c), cp0(c),
cxt(c), runp(c), cn(0)
{
	assert(m >= 64 && (m&m - 1) == 0);  // power of 2?
	assert(sizeof(E) == 64);
	sm = new StateMap[C];
	for (int i = 0; i < C; ++i) {
		cp0[i] = cp[i] = &t[0].bh[0][0];
		runp[i] = cp[i] + 3;
	}
}

ContextMap::~ContextMap()
{
	delete[] sm;
}

// Set the i'th context to cx
inline void ContextMap::set(U32 cx, int next)
{
	int i = cn++;
	i &= next;
	assert(i >= 0 && i < C);
	cx = cx * 987654323 + i;  // permute (don't hash) cx to spread the distribution
	cx = cx << 16 | cx >> 16;
	cxt[i] = cx * 123456791 + i;
}

// Update the model with bit y1, and predict next bit to mixer m.
// Context: cc=c0, bp=bpos, c1=buf(1), y1=y.
int ContextMap::mix1(Mixer& m, int cc, int bp, int c1, int y1)
{

	// Update model with y
	int result = 0;
	for (int i = 0; i<cn; ++i) {
		if (cp[i]) {
			assert(cp[i] >= &t[0].bh[0][0] && cp[i] <= &t[t.size() - 1].bh[6][6]);
			assert((long(cp[i]) & 63) >= 15);
			int ns = nex(*cp[i], y1);
			if (ns >= 204 && rnd() << (452 - ns >> 3)) ns -= 4;  // probabilistic increment
			*cp[i] = ns;
		}

		// Update context pointers
		if (bpos>1 && runp[i][0] == 0)
			cp[i] = 0;
		else if (bpos == 1 || bpos == 3 || bpos == 6)
			cp[i] = cp0[i] + 1 + (cc & 1);
		else if (bpos == 4 || bpos == 7)
			cp[i] = cp0[i] + 3 + (cc & 3);
		else {
			cp0[i] = cp[i] = t[cxt[i] + cc&t.size() - 1].get(cxt[i] >> 16);

			// Update pending bit histories for bits 2-7
			if (bpos == 0) {
				if (cp0[i][3] == 2) {
					const int c = cp0[i][4] + 256;
					U8 *p = t[cxt[i] + (c >> 6)&t.size() - 1].get(cxt[i] >> 16);
					p[0] = 1 + ((c >> 5) & 1);
					p[1 + ((c >> 5) & 1)] = 1 + ((c >> 4) & 1);
					p[3 + ((c >> 4) & 3)] = 1 + ((c >> 3) & 1);
					p = t[cxt[i] + (c >> 3)&t.size() - 1].get(cxt[i] >> 16);
					p[0] = 1 + ((c >> 2) & 1);
					p[1 + ((c >> 2) & 1)] = 1 + ((c >> 1) & 1);
					p[3 + ((c >> 1) & 3)] = 1 + (c & 1);
					cp0[i][6] = 0;
				}
				// Update run count of previous context
				if (runp[i][0] == 0)  // new context
					runp[i][0] = 2, runp[i][1] = c1;
				else if (runp[i][1] != c1)  // different byte in context
					runp[i][0] = 1, runp[i][1] = c1;
				else if (runp[i][0] < 254)  // same byte in context
					runp[i][0] += 2;
				else if (runp[i][0] == 255)
					runp[i][0] = 128;
				runp[i] = cp0[i] + 3;
			}
		}

		// predict from last byte in context
		int rc = runp[i][0];  // count*2, +1 if 2 different bytes seen
		if (runp[i][1] + 256 >> 8 - bp == cc) {
			int b = (runp[i][1] >> 7 - bp & 1) * 2 - 1;  // predicted bit + for 1, - for 0
			int c = ilog(rc + 1) << 2 + (~rc & 1);
			m.add(b*c);
		}
		else
			m.add(0);

		// predict from bit context
		result += mix2(m, cp[i] ? *cp[i] : 0, sm[i]);
	}
	if (bp == 7) cn = 0;
	return result;
}

/*
typedef enum {
DEFAULT, JPEG, EXE, TEXT
} Filetype;
*/

#if 0
//////////////////////////// Models //////////////////////////////

// All of the models below take a Mixer as a parameter and write
// predictions to it.

//////////////////////////// matchModel ///////////////////////////

// matchModel() finds the longest matching context and returns its length

int matchModel(Mixer& m)
{
	const int MAXLEN = 65534;  // longest allowed match + 1
	static newArray<int> t(MEM);  // hash table of pointers to contexts
	static int h = 0;  // hash of last 7 bytes
	static int ptr = 0;  // points to next byte of match if any
	static int len = 0;  // length of match, or 0 if no match
	static int result = 0;

	static SmallStationaryContextMap scm1(0x20000);

	if (!bpos) {
		h = h * 997 * 8 + buf(1) + 1 & t.size() - 1;  // update context hash
		if (len) ++len, ++ptr;
		else {  // find match
			ptr = t[h];
			if (ptr && pos - ptr < buf.size())
			while (buf(len + 1) == buf[ptr - len - 1] && len<MAXLEN) ++len;
		}
		t[h] = pos;  // update hash table
		result = len;
		//    if (result>0 && !(result&0xfff)) printf("pos=%d len=%d ptr=%d\n", pos, len, ptr);
		scm1.set(pos);
	}

	// predict
	if (len>MAXLEN) len = MAXLEN;
	int sgn;
	if (len && buf(1) == buf[ptr - 1] && c0 == buf[ptr] + 256 >> 8 - bpos) {
		if (buf[ptr] >> 7 - bpos & 1) sgn = 1;
		else sgn = -1;
	}
	else sgn = len = 0;
	m.add(sgn * 4 * ilog(len));
	m.add(sgn * 64 * min(len, 32));
	scm1.mix(m);
	return result;
}


//////////////////////////// wordModel /////////////////////////

// Model English text (words and columns/end of line)

void wordModel(Mixer& m)
{
	static U32 word0 = 0, word1 = 0, word2 = 0, word3 = 0, word4 = 0, word5 = 0;  // hashes
	static U32 text0 = 0;  // hash stream of letters
	static ContextMap cm(MEM * 16, 20);
	static int nl1 = -3, nl = -2;  // previous, current newline position

	// Update word hashes
	if (bpos == 0) {
		int c = c4 & 255;
		if (c >= 'A' && c <= 'Z')
			c += 'a' - 'A';
		if (c >= 'a' && c <= 'z' || c >= 128) {
			word0 = word0 * 263 * 32 + c;
			text0 = text0 * 997 * 16 + c;
		}
		else if (word0) {
			word5 = word4 * 23;
			word4 = word3 * 19;
			word3 = word2 * 17;
			word2 = word1 * 13;
			word1 = word0 * 11;
			word0 = 0;
		}
		if (c == 10) nl1 = nl, nl = pos - 1;
		int col = min(255, pos - nl), above = buf[nl1 + col]; // text column context
		U32 h = word0 * 271 + buf(1);

		cm.set(h);
		cm.set(word0);
		cm.set(h + word1);
		cm.set(word0 + word1 * 31);
		cm.set(h + word1 + word2 * 29);
		cm.set(text0 & 0xffffff);
		cm.set(text0 & 0xfffff);

		cm.set(h + word2);
		cm.set(h + word3);
		cm.set(h + word4);
		cm.set(h + word5);
		cm.set(buf(1) | buf(3) << 8 | buf(5) << 16);
		cm.set(buf(2) | buf(4) << 8 | buf(6) << 16);

		cm.set(h + word1 + word3);
		cm.set(h + word2 + word3);

		// Text column models
		cm.set(col << 16 | buf(1) << 8 | above);
		cm.set(buf(1) << 8 | above);
		cm.set(col << 8 | buf(1));
		cm.set(col);
	}
	cm.mix(m);
}

//////////////////////////// recordModel ///////////////////////

// Model 2-D data with fixed record length.  Also order 1-2 models
// that include the distance to the last match.

void recordModel(Mixer& m)
{
	static int cpos1[256], cpos2[256], cpos3[256], cpos4[256];
	static int wpos1[0x10000]; // buf(1..2) -> last position
	static int rlen = 2, rlen1 = 3, rlen2 = 4;  // run length and 2 candidates
	static int rcount1 = 0, rcount2 = 0;  // candidate counts
	static ContextMap cm(32768, 3), cn(32768 / 2, 3), co(32768 * 2, 3), cp(MEM, 3);

	// Find record length
	if (!bpos) {
		int w = c4 & 0xffff, c = w & 255, d = w >> 8;
#if 1
		int r = pos - cpos1[c];
		if (r > 1 && r == cpos1[c] - cpos2[c]
			&& r == cpos2[c] - cpos3[c] && r == cpos3[c] - cpos4[c]
			&& (r > 15 || (c == buf(r * 5 + 1)) && c == buf(r * 6 + 1))) {
			if (r == rlen1) ++rcount1;
			else if (r == rlen2) ++rcount2;
			else if (rcount1 > rcount2) rlen2 = r, rcount2 = 1;
			else rlen1 = r, rcount1 = 1;
		}
		if (rcount1 > 15 && rlen != rlen1) rlen = rlen1, rcount1 = rcount2 = 0;
		if (rcount2 > 15 && rlen != rlen2) rlen = rlen2, rcount1 = rcount2 = 0;

		// Set 2 dimensional contexts
		assert(rlen > 0);
#endif
		cm.set(c << 8 | (min(255, pos - cpos1[c]) / 4));
		cm.set(w << 9 | llog(pos - wpos1[w]) >> 2);

		cm.set(rlen | buf(rlen) << 10 | buf(rlen * 2) << 18);
		cn.set(w | rlen << 8);
		cn.set(d | rlen << 16);
		cn.set(c | rlen << 8);

		co.set(buf(1) << 8 | min(255, pos - cpos1[buf(1)]));
		co.set(buf(1) << 17 | buf(2) << 9 | llog(pos - wpos1[w]) >> 2);
		int col = pos%rlen;
		co.set(buf(1) << 8 | buf(rlen));

		//cp.set(w*16);
		//cp.set(d*32);
		//cp.set(c*64);
		cp.set(rlen | buf(rlen) << 10 | col << 18);
		cp.set(rlen | buf(1) << 10 | col << 18);
		cp.set(col | rlen << 12);

		// update last context positions
		cpos4[c] = cpos3[c];
		cpos3[c] = cpos2[c];
		cpos2[c] = cpos1[c];
		cpos1[c] = pos;
		wpos1[w] = pos;
	}
	cm.mix(m);
	cn.mix(m);
	co.mix(m);
	cp.mix(m);
}


//////////////////////////// sparseModel ///////////////////////

// Model order 1-2 contexts with gaps.

void sparseModel(Mixer& m, int seenbefore, int howmany)
{
	static ContextMap cm(MEM * 2, 48);
	static int mask = 0;

	if (bpos == 0) {

		cm.set(c4 & 0x00f0f0f0);
		cm.set((c4 & 0xf0f0f0f0) + 1);
		cm.set((c4 & 0x00f8f8f8) + 2);
		cm.set((c4 & 0xf8f8f8f8) + 3);
		cm.set((c4 & 0x00e0e0e0) + 4);
		cm.set((c4 & 0xe0e0e0e0) + 5);
		cm.set((c4 & 0x00f0f0ff) + 6);

		cm.set(seenbefore);
		cm.set(howmany);
		cm.set(c4 & 0x00ff00ff);
		cm.set(c4 & 0xff0000ff);
		cm.set(buf(1) | buf(5) << 8);
		cm.set(buf(1) | buf(6) << 8);
		cm.set(buf(3) | buf(6) << 8);
		cm.set(buf(4) | buf(8) << 8);

		for (int i = 1; i < 8; ++i) {
			cm.set((buf(i + 1) << 8) | buf(i + 2));
			cm.set((buf(i + 1) << 8) | buf(i + 3));
			cm.set(seenbefore | buf(i) << 8);
		}

		int fl = 0;
		if (c4 & 0xff != 0) {
			if (isalpha(c4 & 0xff)) fl = 1;
			else if (ispunct(c4 & 0xff)) fl = 2;
			else if (isspace(c4 & 0xff)) fl = 3;
			else if (c4 & 0xff == 0xff) fl = 4;
			else if (c4 & 0xff < 16) fl = 5;
			else if (c4 & 0xff < 64) fl = 6;
			else fl = 7;
		}
		mask = (mask << 3) | fl;
		cm.set(mask);
		cm.set(mask << 8 | buf(1));
		cm.set(mask << 17 | buf(2) << 8 | buf(3));
		cm.set(mask & 0x1ff | ((c4 & 0xf0f0f0f0) << 9));
	}
	cm.mix(m);
}

//////////////////////////// distanceModel ///////////////////////

// Model for modelling distances between symbols

void distanceModel(Mixer& m)
{
	static ContextMap cr(MEM, 3);
	if (bpos == 0) {
		static int pos00 = 0, pos20 = 0, posnl = 0;
		int c = c4 & 0xff;
		if (c == 0x00)pos00 = pos;
		if (c == 0x20)pos20 = pos;
		if (c == 0xff || c == '\r' || c == '\n')posnl = pos;
		cr.set(min(pos - pos00, 255) | (c << 8));
		cr.set(min(pos - pos20, 255) | (c << 8));
		cr.set(min(pos - posnl, 255) | (c << 8) + 234567);
	}
	cr.mix(m);
}

//////////////////////////// bmpModel /////////////////////////////////

// Model a 24-bit color uncompressed .bmp or .tif file.  Return
// width in pixels if an image file is detected, else 0.

// 32-bit little endian number at buf(i)..buf(i-3)
inline U32 i4(int i)
{
	assert(i > 3);
	return buf(i) + 256 * buf(i - 1) + 65536 * buf(i - 2) + 16777216 * buf(i - 3);
}

// 16-bit
inline int i2(int i)
{
	assert(i > 1);
	return buf(i) + 256 * buf(i - 1);
}

// Square buf(i)
inline int sqrbuf(int i)
{
	assert(i > 0);
	return buf(i)*buf(i);
}

int bmpModel(Mixer& m)
{
	static int w = 0;  // width of image in bytes (pixels * 3)
	static int eoi = 0;     // end of image
	static U32 tiff = 0;  // offset of tif header
	const int SC = 0x20000;
	static SmallStationaryContextMap scm1(SC), scm2(SC),
		scm3(SC), scm4(SC), scm5(SC), scm6(SC * 2);
	static ContextMap cm(MEM * 4, 8);

	// Detect .bmp file header (24 bit color, not compressed)
	if (!bpos && buf(54) == 'B' && buf(53) == 'M'
		&& i4(44) == 54 && i4(40) == 40 && i4(24) == 0) {
		w = (i4(36) + 3 & -4) * 3;  // image width
		const int height = i4(32);
		eoi = pos;
		if (w < 0x30000 && height < 0x10000) {
			eoi = pos + w*height;  // image size in bytes
			printf("BMP %dx%d ", w / 3, height);
		}
		else
			eoi = pos;
	}

	// Detect .tif file header (24 bit color, not compressed).
	// Parsing is crude, won't work with weird formats.
	if (!bpos) {
		if (c4 == 0x49492a00) tiff = pos;  // Intel format only
		if (pos - tiff == 4 && c4 != 0x08000000) tiff = 0; // 8=normal offset to directory
		if (tiff && pos - tiff == 200) {  // most of directory should be read by now
			int dirsize = i2(pos - tiff - 4);  // number of 12-byte directory entries
			w = 0;
			int bpp = 0, compression = 0, width = 0, height = 0;
			for (int i = tiff + 6; i<pos - 12 && --dirsize>0; i += 12) {
				int tag = i2(pos - i);  // 256=width, 257==height, 259: 1=no compression
				// 277=3 samples/pixel
				int tagfmt = i2(pos - i - 2);  // 3=short, 4=long
				int taglen = i4(pos - i - 4);  // number of elements in tagval
				int tagval = i4(pos - i - 8);  // 1 long, 1-2 short, or points to array
				if ((tagfmt == 3 || tagfmt == 4) && taglen == 1) {
					if (tag == 256) width = tagval;
					if (tag == 257) height = tagval;
					if (tag == 259) compression = tagval; // 1 = no compression
					if (tag == 277) bpp = tagval;  // should be 3
				}
			}
			if (width > 0 && height > 0 && width*height > 50 && compression == 1
				&& (bpp == 1 || bpp == 3))
				eoi = tiff + width*height*bpp, w = width*bpp;
			if (eoi > pos)
				printf("TIFF %dx%dx%d ", width, height, bpp);
			else
				tiff = w = 0;
		}
	}
	if (pos > eoi) return w = 0;

	// Select nearby pixels as context
	if (!bpos) {
		assert(w > 3);
		int color = pos % 3;
		int mean = buf(3) + buf(w - 3) + buf(w) + buf(w + 3);
		const int var = sqrbuf(3) + sqrbuf(w - 3) + sqrbuf(w) + sqrbuf(w + 3) - mean*mean / 4 >> 2;
		mean >>= 2;
		const int logvar = ilog(var);
		int i = 0;
		cm.set(hash(++i, buf(3) >> 2, buf(w) >> 2, color));
		cm.set(hash(++i, buf(3) >> 2, buf(1) >> 2, color));
		cm.set(hash(++i, buf(3) >> 2, buf(2) >> 2, color));
		cm.set(hash(++i, buf(w) >> 2, buf(1) >> 2, color));
		cm.set(hash(++i, buf(w) >> 2, buf(2) >> 2, color));
		cm.set(hash(++i, buf(3) + buf(w) >> 1, color));
		cm.set(hash(++i, buf(3) + buf(w) >> 3, buf(1) >> 5, buf(2) >> 5, color));
		cm.set(hash(++i, mean, logvar >> 5, color));
		scm1.set(buf(3) + buf(w) >> 1);
		scm2.set(buf(3) + buf(w) - buf(w + 3) >> 1);
		scm3.set(buf(3) * 2 - buf(6) >> 1);
		scm4.set(buf(w) * 2 - buf(w * 2) >> 1);
		scm5.set(buf(3) + buf(w) - buf(w - 3) >> 1);
		scm6.set(mean >> 1 | logvar << 1 & 0x180);
	}

	// Predict next bit
	scm1.mix(m);
	scm2.mix(m);
	scm3.mix(m);
	scm4.mix(m);
	scm5.mix(m);
	scm6.mix(m);
	cm.mix(m);
	return w;
}

//////////////////////////// jpegModel /////////////////////////

// Model JPEG. Return 1 if a JPEG file is detected or else 0.
// Only the baseline and 8 bit extended Huffman coded DCT modes are
// supported.  The model partially decodes the JPEG image to provide
// context for the Huffman coded symbols.

// JPEG model from paq8fthis2 by Jan Ondrus.

// Print a JPEG segment at buf[p...] for debugging
void dump(const char* msg, int p)
{
	printf("%s:", msg);
	int len = buf[p + 2] * 256 + buf[p + 3];
	for (int i = 0; i < len + 2; ++i)
		printf(" %02X", buf[p + i]);
	printf("\n");
}

// Detect invalid JPEG data.  The proper response is to silently
// fall back to a non-JPEG model.
#define jassert(x) if (!(x)) { \
	/*  printf("JPEG error at %d, line %d: %s\n", pos, __LINE__, #x); */ \
	jpeg = 0; \
	return next_jpeg; }

struct HUF {
	U32 min, max; int val;
}; // Huffman decode tables
// huf[Tc][Th][m] is the minimum, maximum+1, and pointer to codes for
// coefficient type Tc (0=DC, 1=AC), table Th (0-3), length m+1 (m=0-15)

int jpegModel(Mixer& m)
{

	// State of parser
	enum {
		SOF0 = 0xc0, SOF1, SOF2, SOF3, DHT, RST0 = 0xd0, SOI = 0xd8, EOI, SOS, DQT,
		DNL, DRI, APP0 = 0xe0, COM = 0xfe, FF
	};  // Second byte of 2 byte codes
	static int jpeg = 0;  // 1 if JPEG is header detected, 2 if image data
	static int next_jpeg = 0;  // updated with jpeg on next byte boundary
	static int app;  // Bytes remaining to skip in APPx or COM field
	static int sof = 0, sos = 0, data = 0;  // pointers to buf
	static newArray<int> ht(8);  // pointers to Huffman table headers
	static int htsize = 0;  // number of pointers in ht

	// Huffman decode state
	static U32 huffcode = 0;  // Current Huffman code including extra bits
	static int huffbits = 0;  // Number of valid bits in huffcode
	static int huffsize = 0;  // Number of bits without extra bits
	static int rs = -1;  // Decoded huffcode without extra bits.  It represents
	// 2 packed 4-bit numbers, r=run of zeros, s=number of extra bits for
	// first nonzero code.  huffcode is complete when rs >= 0.
	// rs is -1 prior to decoding incomplete huffcode.
	static int mcupos = 0;  // position in MCU (0-639).  The low 6 bits mark
	// the coefficient in zigzag scan order (0=DC, 1-63=AC).  The high
	// bits mark the block within the MCU, used to select Huffman tables.

	// Decoding tables
	static newArray<HUF> huf(128);  // Tc*64+Th*16+m -> min, max, val
	static int mcusize = 0;  // number of coefficients in an MCU
	static int linesize = 0; // width of image in MCU
	static int hufsel[2][10];  // DC/AC, mcupos/64 -> huf decode table
	static newArray<U8> hbuf(2048);  // Tc*1024+Th*256+hufcode -> RS

	// Image state
	static newArray<int> color(10);  // block -> component (0-3)
	static newArray<int> pred(4);  // component -> last DC value
	static int dc = 0;  // DC value of the current block
	static int width = 0;  // Image width in MCU
	static int row = 0, column = 0;  // in MCU (column 0 to width-1)
	static Buf cbuf(0x20000); // Rotating buffer of coefficients, coded as:
	// DC: level shifted absolute value, low 4 bits discarded, i.e.
	//   [-1023...1024] -> [0...255].
	// AC: as an RS code: a run of R (0-15) zeros followed by an S (0-15)
	//   bit number, or 00 for end of block (in zigzag order).
	//   However if R=0, then the format is ssss11xx where ssss is S,
	//   xx is the first 2 extra bits, and the last 2 bits are 1 (since
	//   this never occurs in a valid RS code).
	static int cpos = 0;  // position in cbuf
	static U32 huff1 = 0, huff2 = 0, huff3 = 0, huff4 = 0;  // hashes of last codes
	static int rs1, rs2, rs3, rs4;  // last 4 RS codes
	static int ssum = 0, ssum1 = 0, ssum2 = 0, ssum3 = 0, ssum4 = 0;
	// sum of S in RS codes in block and last 4 values

	static IntBuf cbuf2(0x20000);
	static newArray<int> adv_pred(24);
	static newArray<int> sum(16);

	//for parsing Quantization tables
	static int dqt_state = -1, dqt_end = 0, qnum = 0;
	static newArray<U8> qtab(256); // table
	static newArray<int> qmap(10); // block -> table number

	const static U8 zzu[64] = {  // zigzag coef -> u,v
		0, 1, 0, 0, 1, 2, 3, 2, 1, 0, 0, 1, 2, 3, 4, 5, 4, 3, 2, 1, 0, 0, 1, 2, 3, 4, 5, 6, 7, 6, 5, 4,
		3, 2, 1, 0, 1, 2, 3, 4, 5, 6, 7, 7, 6, 5, 4, 3, 2, 3, 4, 5, 6, 7, 7, 6, 5, 4, 5, 6, 7, 7, 6, 7 };
	const static U8 zzv[64] = {
		0, 0, 1, 2, 1, 0, 0, 1, 2, 3, 4, 3, 2, 1, 0, 0, 1, 2, 3, 4, 5, 6, 5, 4, 3, 2, 1, 0, 0, 1, 2, 3,
		4, 5, 6, 7, 7, 6, 5, 4, 3, 2, 1, 2, 3, 4, 5, 6, 7, 7, 6, 5, 4, 3, 4, 5, 6, 7, 7, 6, 5, 6, 7, 7 };

	// Be sure to quit on a byte boundary
	if (!bpos) next_jpeg = jpeg > 1;
	if (bpos && !jpeg) return next_jpeg;
	if (!bpos && app > 0) --app;
	if (app > 0) return next_jpeg;
	if (!bpos) {

		// Parse.  Baseline DCT-Huffman JPEG syntax is:
		// SOI APPx... misc... SOF0 DHT... SOS data EOI
		// SOI (= FF D8) start of image.
		// APPx (= FF Ex) len ... where len is always a 2 byte big-endian length
		//   including the length itself but not the 2 byte preceding code.
		//   Application data is ignored.  There may be more than one APPx.
		// misc codes are DQT, DNL, DRI, COM (ignored).
		// SOF0 (= FF C0) len 08 height width Nf [C HV Tq]...
		//   where len, height, width (in pixels) are 2 bytes, Nf is the repeat
		//   count (1 byte) of [C HV Tq], where C is a component identifier
		//   (color, 0-3), HV is the horizontal and vertical dimensions
		//   of the MCU (high, low bits, packed), and Tq is the quantization
		//   table ID (not used).  An MCU (minimum compression unit) consists
		//   of 64*H*V DCT coefficients for each color.
		// DHT (= FF C4) len [TcTh L1...L16 V1,1..V1,L1 ... V16,1..V16,L16]...
		//   defines Huffman table Th (1-4) for Tc (0=DC (first coefficient)
		//   1=AC (next 63 coefficients)).  L1..L16 are the number of codes
		//   of length 1-16 (in ascending order) and Vx,y are the 8-bit values.
		//   A V code of RS means a run of R (0-15) zeros followed by S (0-15)
		//   additional bits to specify the next nonzero value, negative if
		//   the first additional bit is 0 (e.g. code x63 followed by the
		//   3 bits 1,0,1 specify 7 coefficients: 0, 0, 0, 0, 0, 0, 5.
		//   Code 00 means end of block (remainder of 63 AC coefficients is 0).
		// SOS (= FF DA) len Ns [Cs TdTa]... 0 3F 00
		//   Start of scan.  TdTa specifies DC/AC Huffman tables (0-3, packed
		//   into one byte) for component Cs matching C in SOF0, repeated
		//   Ns (1-4) times.
		// EOI (= FF D9) is end of image.
		// Huffman coded data is between SOI and EOI.  Codes may be embedded:
		// RST0-RST7 (= FF D0 to FF D7) mark the start of an independently
		//   compressed region.
		// DNL (= FF DC) 04 00 height
		//   might appear at the end of the scan (ignored).
		// FF 00 is interpreted as FF (to distinguish from RSTx, DNL, EOI).

		// Detect JPEG (SOI, APPx)
		if (!jpeg && buf(4) == FF && buf(3) == SOI && buf(2) == FF && buf(1) >> 4 == 0xe) {
			jpeg = 1;
			app = sos = sof = htsize = data = mcusize = linesize = 0;
			huffcode = huffbits = huffsize = mcupos = cpos = 0, rs = -1;
			memset(&huf[0], 0, huf.size()*sizeof(HUF));
			memset(&pred[0], 0, pred.size()*sizeof(int));
		}

		// Detect end of JPEG when data contains a marker other than RSTx
		// or byte stuff (00).
		if (jpeg && data && buf(2) == FF && buf(1) && (buf(1) & 0xf8) != RST0) {
			jassert(buf(1) == EOI);
			jpeg = 0;
		}
		if (!jpeg) return next_jpeg;

		// Detect APPx or COM field
		if (!data && !app && buf(4) == FF && (buf(3) >> 4 == 0xe || buf(3) == COM))
			app = buf(2) * 256 + buf(1) + 2;

		// Save pointers to sof, ht, sos, data,
		if (buf(5) == FF && buf(4) == SOS) {
			int len = buf(3) * 256 + buf(2);
			if (len == 6 + 2 * buf(1) && buf(1) && buf(1) <= 4)  // buf(1) is Ns
				sos = pos - 5, data = sos + len + 2, jpeg = 2;
		}
		if (buf(4) == FF && buf(3) == DHT && htsize < 8) ht[htsize++] = pos - 4;
		if (buf(4) == FF && buf(3) == SOF0) sof = pos - 4;

		// Parse Quantizazion tables
		if (buf(4) == FF && buf(3) == DQT) {
			dqt_end = pos + buf(2) * 256 + buf(1) - 1;
			dqt_state = 0;
		}
		else if (dqt_state >= 0) {
			if (pos >= dqt_end) {
				dqt_state = -1;
			}
			else {
				if (dqt_state % 65 == 0) {
					qnum = buf(1);
				}
				else {
					jassert(buf(1) > 0);
					jassert(qnum >= 0 && qnum < 4);
					qtab[qnum * 64 + ((dqt_state % 65) - 1)] = buf(1) - 1;
				}
				dqt_state++;
			}
		}

		// Restart
		if (buf(2) == FF && (buf(1) & 0xf8) == RST0) {
			huffcode = huffbits = huffsize = mcupos = 0, rs = -1;
			memset(&pred[0], 0, pred.size()*sizeof(int));
		}
	}

	{
		// Build Huffman tables
		// huf[Tc][Th][m] = min, max+1 codes of length m, pointer to byte values
		if (pos == data && bpos == 1) {
			jassert(htsize > 0);
			for (int i = 0; i < htsize; ++i) {
				int p = ht[i] + 4;  // pointer to current table after length field
				int end = p + buf[p - 2] * 256 + buf[p - 1] - 2;  // end of Huffman table
				int count = 0;  // sanity check
				while (p < end && end < pos && end < p + 2100 && ++count < 10) {
					int tc = buf[p] >> 4, th = buf[p] & 15;
					if (tc >= 2 || th >= 4) break;
					jassert(tc >= 0 && tc < 2 && th >= 0 && th < 4);
					HUF* h = &huf[tc * 64 + th * 16]; // [tc][th][0]; 
					int val = p + 17;  // pointer to values
					int hval = tc * 1024 + th * 256;  // pointer to RS values in hbuf
					for (int j = 0; j < 256; ++j) // copy RS codes
						hbuf[hval + j] = buf[val + j];
					int code = 0;
					for (int j = 0; j < 16; ++j) {
						h[j].min = code;
						h[j].max = code += buf[p + j + 1];
						h[j].val = hval;
						val += buf[p + j + 1];
						hval += buf[p + j + 1];
						code *= 2;
					}
					p = val;
					jassert(hval >= 0 && hval < 2048);
				}
				jassert(p == end);
			}
			huffcode = huffbits = huffsize = 0, rs = -1;

			// Build Huffman table selection table (indexed by mcupos).
			// Get image width.
			if (!sof && sos) return next_jpeg;
			int ns = buf[sos + 4];
			int nf = buf[sof + 9];
			jassert(ns <= 4 && nf <= 4);
			mcusize = 0;  // blocks per MCU
			int hmax = 0;  // MCU horizontal dimension
			for (int i = 0; i < ns; ++i) {
				for (int j = 0; j<nf; ++j) {
					if (buf[sos + 2 * i + 5] == buf[sof + 3 * j + 10]) { // Cs == C ?
						int hv = buf[sof + 3 * j + 11];  // packed dimensions H x V
						if (hv >> 4>hmax) hmax = hv >> 4;
						hv = (hv & 15)*(hv >> 4);  // number of blocks in component C
						jassert(hv >= 1 && hv + mcusize <= 10);
						while (hv) {
							jassert(mcusize < 10);
							hufsel[0][mcusize] = buf[sos + 2 * i + 6] >> 4 & 15;
							hufsel[1][mcusize] = buf[sos + 2 * i + 6] & 15;
							jassert(hufsel[0][mcusize] < 4 && hufsel[1][mcusize] < 4);
							color[mcusize] = i;
							int tq = buf[sof + 3 * j + 12];  // quantization table index (0..3)
							jassert(tq >= 0 && tq < 4);
							qmap[mcusize] = tq; // Quantizazion table mapping
							--hv;
							++mcusize;
						}
					}
				}
			}
			jassert(hmax >= 1 && hmax <= 10);
			width = buf[sof + 7] * 256 + buf[sof + 8];  // in pixels
			int height = buf[sof + 5] * 256 + buf[sof + 6];
			printf("JPEG %dx%d ", width, height);
			width = (width - 1) / (hmax * 8) + 1;  // in MCU
			jassert(width > 0);
			mcusize *= 64;  // coefficients per MCU
			row = column = 0;
		}
	}


	// Decode Huffman
	{
		if (mcusize && buf(1 + (!bpos)) != FF) {  // skip stuffed byte
			jassert(huffbits <= 32);
			huffcode += huffcode + y;
			++huffbits;
			if (rs<0) {
				jassert(huffbits >= 1 && huffbits <= 16);
				const int ac = (mcupos & 63)>0;
				jassert(mcupos >= 0 && (mcupos >> 6) < 10);
				jassert(ac == 0 || ac == 1);
				const int sel = hufsel[ac][mcupos >> 6];
				jassert(sel >= 0 && sel < 4);
				const int i = huffbits - 1;
				jassert(i >= 0 && i < 16);
				const HUF *h = &huf[ac * 64 + sel * 16]; // [ac][sel];
				jassert(h[i].min <= h[i].max && h[i].val<2048 && huffbits>0);
				if (huffcode < h[i].max) {
					jassert(huffcode >= h[i].min);
					int k = h[i].val + huffcode - h[i].min;
					jassert(k >= 0 && k < 2048);
					rs = hbuf[k];
					huffsize = huffbits;
				}
			}
			if (rs >= 0) {
				if (huffsize + (rs & 15) == huffbits) { // done decoding
					huff4 = huff3;
					huff3 = huff2;
					huff2 = huff1;
					huff1 = hash(huffcode, huffbits);
					rs4 = rs3;
					rs3 = rs2;
					rs2 = rs1;
					rs1 = rs;
					int x = 0;  // decoded extra bits
					if (mcupos & 63) {  // AC
						if (rs == 0) { // EOB
							mcupos = mcupos + 63 & -64;
							jassert(mcupos >= 0 && mcupos <= mcusize && mcupos <= 640);
							while (cpos & 63) {
								cbuf2[cpos] = 0;
								cbuf[cpos++] = 0;
							}
						}
						else {  // rs = r zeros + s extra bits for the next nonzero value
							// If first extra bit is 0 then value is negative.
							jassert((rs & 15) <= 10);
							const int r = rs >> 4;
							const int s = rs & 15;
							jassert(mcupos >> 6 == mcupos + r >> 6);
							mcupos += r + 1;
							x = huffcode&(1 << s) - 1;
							if (s && !(x >> s - 1)) x -= (1 << s) - 1;
							for (int i = r; i >= 1; --i) {
								cbuf2[cpos] = 0;
								cbuf[cpos++] = i << 4 | s;
							}
							cbuf2[cpos] = x;
							cbuf[cpos++] = s << 4 | huffcode << 2 >> s & 3 | 12;
							ssum += s;
						}
					}
					else {  // DC: rs = 0S, s<12
						jassert(rs < 12);
						++mcupos;
						x = huffcode&(1 << rs) - 1;
						if (rs && !(x >> rs - 1)) x -= (1 << rs) - 1;
						jassert(mcupos >= 0 && mcupos >> 6 < 10);
						const int comp = color[mcupos >> 6];
						jassert(comp >= 0 && comp < 4);
						dc = pred[comp] += x;
						jassert((cpos & 63) == 0);
						cbuf2[cpos] = dc;
						cbuf[cpos++] = dc + 1023 >> 3;
						ssum4 = ssum3;
						ssum3 = ssum2;
						ssum2 = ssum1;
						ssum1 = ssum;
						ssum = rs;
					}
					jassert(mcupos >= 0 && mcupos <= mcusize);
					if (mcupos >= mcusize) {
						mcupos = 0;
						if (++column == width) column = 0, ++row;
					}
					huffcode = huffsize = huffbits = 0, rs = -1;

					// UPDATE_ADV_PRED !!!!
					{
						int i, zz = mcupos & 63, acomp = mcupos >> 6, q = 64 * qmap[acomp], cpos_dc = cpos - zz;
						int left_s = mcusize;
						for (i = 1; i < (mcusize >> 6); i++)
						if (color[(acomp + i) % (mcusize >> 6)] == color[acomp]) left_s = ((mcusize >> 6) - i) << 6;

						if (zz == 0) {
							for (i = 0; i < 16; i++) sum[i] = 0;
							for (i = 0; i < 64; i++) {
								sum[zzu[i]] += (zzv[i] ? 256 : 181) * (zzv[i] & 1 ? -1 : +1) * (qtab[q + i] + 1) * cbuf2[cpos_dc + i - mcusize*width];
								sum[8 + zzv[i]] += (zzu[i] ? 256 : 181) * (zzu[i] & 1 ? -1 : +1) * (qtab[q + i] + 1) * cbuf2[cpos_dc + i - left_s];
							}
						}
						else {
							sum[zzu[zz - 1]] -= (zzv[zz - 1] ? 256 : 181) * (qtab[q + zz - 1] + 1) * cbuf2[cpos - 1];
							sum[8 + zzv[zz - 1]] -= (zzu[zz - 1] ? 256 : 181) * (qtab[q + zz - 1] + 1) * cbuf2[cpos - 1];
						}

						for (int st = 0; st < 8; st++) {
							int zz2 = (st > 63 - zz ? 63 : zz + st);
							adv_pred[3 * st + 0] = sum[zzu[zz2]];
							adv_pred[3 * st + 1] = sum[8 + zzv[zz2]];
							adv_pred[3 * st + 2] = (adv_pred[3 * st + 0] + adv_pred[3 * st + 1]) / 2;
							for (i = 3 * st; i < 3 * st + 3; i++) {
								adv_pred[i] /= (qtab[q + zz2] + 1) * 181;
								if (zz2 == 0) adv_pred[i] -= cbuf2[cpos_dc - left_s];
								adv_pred[i] = (adv_pred[i] < 0 ? -1 : 1) * ilog(8 * abs(adv_pred[i]) + 1) / 8;
							}
						}
						for (i = 0; i < 3; i++) {
							int tmp = 0, j = 1;
							while (j < 8 && abs(tmp = adv_pred[i + 3 * j]) < abs(adv_pred[i]) + 2) j++;
							if (j != 8) adv_pred[i] += 128 * j + 64 * (tmp > 0);
						}
					} // !!!!

				}
			}
		}
	}

	// Estimate next bit probability
	if (!jpeg || !data) return next_jpeg;

	// Context model
	const int N = 19;  // size of t, number of contexts
	static BH<9> t(MEM);  // context hash -> bit history
	// As a cache optimization, the context does not include the last 1-2
	// bits of huffcode if the length (huffbits) is not a multiple of 3.
	// The 7 mapped values are for context+{"", 0, 00, 01, 1, 10, 11}.
	static newArray<U32> cxt(N);  // context hashes
	static newArray<U8*> cp(N);  // context pointers
	static StateMap sm[N];
	static Mixer m1(32, 800, 4);
	static APM a1(1024), a2(0x10000);


	// Update model
	if (cp[N - 1]) {
		for (int i = 0; i<N; ++i)
			*cp[i] = nex(*cp[i], y);
	}
	m1.update();

	// Update context
	const int comp = color[mcupos >> 6];
	const int coef = (mcupos & 63) | comp << 6;
	const int hc = huffcode | 1 << huffbits;
	static int hbcount = 2;
	if (++hbcount>2 || huffbits == 0) hbcount = 0;
	jassert(coef >= 0 && coef < 256);
	if (hbcount == 0) {
		const int mpos = mcupos >> 4 | !(mcupos&-64) << 7;
		int n = 0;
		cxt[0] = hash(++n, hc, mcupos >> 2, min(3, mcupos & 63));
		cxt[1] = hash(++n, hc, rs1, comp, adv_pred[0]);
		cxt[2] = hash(++n, hc, rs1, comp, adv_pred[1]);
		cxt[3] = hash(++n, hc, rs1, comp, adv_pred[2]);
		cxt[4] = hash(++n, hc, coef, column >> 3);
		cxt[5] = hash(++n, hc, coef, column >> 1);
		cxt[6] = hash(++n, hc, rs1, mpos);
		cxt[7] = hash(++n, hc, rs1, rs2);
		cxt[8] = hash(++n, hc, rs1, rs2, rs3);
		cxt[9] = hash(++n, hc, ssum >> 4, mcupos);
		cxt[10] = hash(++n, hc, mpos, cbuf[cpos - 1]);
		cxt[11] = hash(++n, hc, dc);
		cxt[12] = hash(++n, hc, rs1, coef);
		cxt[13] = hash(++n, hc, rs1, rs2, coef);
		cxt[14] = hash(++n, hc, mcupos >> 3, ssum3 >> 3);
		cxt[15] = hash(++n, hc, mpos >> 4, cbuf[cpos - width*mcusize]);
		cxt[16] = hash(++n, hc, coef, adv_pred[0]);
		cxt[17] = hash(++n, hc, coef, adv_pred[1]);
		cxt[18] = hash(++n, hc, coef, adv_pred[2]);
	}

	// Predict next bit
	m1.add(128);
	assert(hbcount <= 2);
	for (int i = 0; i < N; ++i) {
		if (hbcount == 0) cp[i] = t[cxt[i]] + 1;
		else if (hbcount == 1) cp[i] += 1 + (huffcode & 1) * 3;
		else cp[i] += 1 + (huffcode & 1);
		int sp = stretch(sm[i].p(*cp[i]));
		m1.add(sp);
	}
	m1.set(0, 1);
	m1.set(coef, 64);
	m1.set(hash((adv_pred[0] % 64) / 4, (adv_pred[1] % 64) / 4) % 640, 640);
	int pr = m1.p();
	pr = a1.p(pr, hc & 1023);
	pr = a2.p(pr, hc & 255 | coef << 8);
	m.add(stretch(pr));
	return 1;
}

//////////////////////////// exeModel /////////////////////////

// Model x86 code.  The contexts are sparse containing only those
// bits relevant to parsing (2 prefixes, opcode, and mod and r/m fields
// of modR/M byte).

// Get context at buf(i) relevant to parsing 32-bit x86 code
U32 execxt(int i, int x = 0)
{
	int prefix = (buf(i + 2) == 0x0f) + 2 * (buf(i + 2) == 0x66) + 3 * (buf(i + 2) == 0x67)
		+ 4 * (buf(i + 3) == 0x0f) + 8 * (buf(i + 3) == 0x66) + 12 * (buf(i + 3) == 0x67);
	int opcode = buf(i + 1);
	int modrm = i ? buf(i) & 0xc7 : 0;
	return prefix | opcode << 4 | modrm << 12 | x << 20;
}

void exeModel(Mixer& m)
{
	const int N = 12;
	static ContextMap cm(MEM, N);
	if (!bpos) {
		for (int i = 0; i<N; ++i)
			cm.set(execxt(i, buf(1)*(i>4)));
	}
	cm.mix(m);
}

//////////////////////////// indirectModel /////////////////////

// The context is a byte string history that occurs within a
// 1 or 2 byte context.

void indirectModel(Mixer& m)
{
	static ContextMap cm(MEM, 6);
	static U32 t1[256];
	static U16 t2[0x10000];

	if (!bpos) {
		U32 d = c4 & 0xffff, c = d & 255;
		U32& r1 = t1[d >> 8];
		r1 = r1 << 8 | c;
		U16& r2 = t2[c4 >> 8 & 0xffff];
		r2 = r2 << 8 | c;
		U32 t = c | t1[c] << 8;
		cm.set(t & 0xffff);
		cm.set(t & 0xffffff);
		cm.set(t);
		cm.set(t & 0xff00);
		t = d | t2[d] << 16;
		cm.set(t & 0xffffff);
		cm.set(t);

	}
	cm.mix(m);
}

//////////////////////////// dmcModel //////////////////////////

// Model using DMC.  The bitwise context is represented by a state graph,
// initilaized to a bytewise order 1 model as in 
// http://plg.uwaterloo.ca/~ftp/dmc/dmc.c but with the following difference:
// - It uses integer arithmetic.
// - The threshold for cloning a state increases as memory is used up.
// - Each state maintains both a 0,1 count and a bit history (as in a
//   context model).  The 0,1 count is best for stationary data, and the
//   bit history for nonstationary data.  The bit history is mapped to
//   a probability adaptively using a StateMap.  The two computed probabilities
//   are combined.
// - When memory is used up the state graph is reinitialized to a bytewise
//   order 1 context as in the original DMC.  However, the bit histories
//   are not cleared.

struct DMCNode {  // 12 bytes
	unsigned int nx[2];  // next pointers
	U8 state;  // bit history
	unsigned int c0 : 12, c1 : 12;  // counts * 256
};

void dmcModel(Mixer& m)
{
	static int top = 0, curr = 0;  // allocated, current node
	static newArray<DMCNode> t(MEM * 2);  // state graph
	static StateMap sm;
	static int threshold = 256;

	// clone next state
	if (top > 0 && top < t.size()) {
		int next = t[curr].nx[y];
		int n = y ? t[curr].c1 : t[curr].c0;
		int nn = t[next].c0 + t[next].c1;
		if (n >= threshold * 2 && nn - n >= threshold * 3) {
			int r = n * 4096 / nn;
			assert(r >= 0 && r <= 4096);
			t[next].c0 -= t[top].c0 = t[next].c0*r >> 12;
			t[next].c1 -= t[top].c1 = t[next].c1*r >> 12;
			t[top].nx[0] = t[next].nx[0];
			t[top].nx[1] = t[next].nx[1];
			t[top].state = t[next].state;
			t[curr].nx[y] = top;
			++top;
			if (top == MEM * 2) threshold = 512;
			if (top == MEM * 3) threshold = 768;
		}
	}

	// Initialize to a bytewise order 1 model at startup or when flushing memory
	if (top == t.size() && bpos == 1) top = 0;
	if (top == 0) {
		assert(t.size() >= 65536);
		for (int i = 0; i < 256; ++i) {
			for (int j = 0; j < 256; ++j) {
				if (i < 127) {
					t[j * 256 + i].nx[0] = j * 256 + i * 2 + 1;
					t[j * 256 + i].nx[1] = j * 256 + i * 2 + 2;
				}
				else {
					t[j * 256 + i].nx[0] = (i - 127) * 256;
					t[j * 256 + i].nx[1] = (i + 1) * 256;
				}
				t[j * 256 + i].c0 = 128;
				t[j * 256 + i].c1 = 128;
			}
		}
		top = 65536;
		curr = 0;
		threshold = 256;
	}

	// update count, state
	if (y) {
		if (t[curr].c1 < 3800) t[curr].c1 += 256;
	}
	else if (t[curr].c0 < 3800) t[curr].c0 += 256;
	t[curr].state = nex(t[curr].state, y);
	curr = t[curr].nx[y];

	// predict
	const int pr1 = sm.p(t[curr].state);
	const int n1 = t[curr].c1;
	const int n0 = t[curr].c0;
	const int pr2 = (n1 + 5) * 4096 / (n0 + n1 + 10);
	m.add(stretch(pr1));
	m.add(stretch(pr2));
}

#endif

#if 1  //picModel
//////////////////////////// picModel //////////////////////////

// Model a 1728 by 2376 2-color CCITT bitmap image, left to right scan,
// MSB first (216 bytes per row, 513216 bytes total).  Insert predictions
// into m.

void picModel(Mixer& m)
{
	static U32 r0, r1, r2, r3;  // last 4 rows, bit 8 is over current pixel
	static newArray<U8> t(0x10200);  // model: cxt -> state
	const int N = 3;  // number of contexts
	static int cxt[N];  // contexts
	static StateMap sm[N];

	// update the model
	for (int i = 0; i < N; ++i)
		t[cxt[i]] = nex(t[cxt[i]], y);

	// update the contexts (pixels surrounding the predicted one)
	r0 += r0 + y;
	r1 += r1 + ((buf(215) >> (7 - bpos)) & 1);
	r2 += r2 + ((buf(431) >> (7 - bpos)) & 1);
	r3 += r3 + ((buf(647) >> (7 - bpos)) & 1);
	cxt[0] = r0 & 0x7 | r1 >> 4 & 0x38 | r2 >> 3 & 0xc0;
	cxt[1] = 0x100 + (r0 & 1 | r1 >> 4 & 0x3e | r2 >> 2 & 0x40 | r3 >> 1 & 0x80);
	cxt[2] = 0x200 + (r0 & 0x3f ^ r1 & 0x3ffe ^ r2 << 2 & 0x7f00 ^ r3 << 5 & 0xf800);

	// predict
	for (int i = 0; i < N; ++i)
		m.add(stretch(sm[i].p(t[cxt[i]])));
}
#endif 

// This combines all the context models with a Mixer.

int contextModel2()
{
	static ContextMap cm(MEM * 32, 9);
	static RunContextMap rcm7(MEM), rcm9(MEM), rcm10(MEM);
	static Mixer m(800, 3088, 7, 128);
	static U32 cxt[16];  // order 0-11 contexts
	static Filetype filetype = DEFAULT;
	static int size = 0;  // bytes remaining in block
	//  static const char* typenames[4]={"", "jpeg ", "exe ", "text "};

	// Parse filetype and size
	if (bpos == 0) {
		--size;
		if (size == -1) filetype = (Filetype)buf(1);
		if (size == -5) {
			size = buf(4) << 24 | buf(3) << 16 | buf(2) << 8 | buf(1);
			//      if (filetype<=3) printf("(%s%d)", typenames[filetype], size);
			if (filetype == EXE) size += 8;
		}
	}

	m.update();
	m.add(256);
#if 0
		// Test for special file types
	int isjpeg = jpegModel(m);  // 1 if JPEG is detected, else 0
	int ismatch = ilog(matchModel(m));  // Length of longest matching context
	int isbmp = bmpModel(m);  // Image width (bytes) if BMP or TIFF detected, or 0

	if (isjpeg) {
	m.set(1, 8);
	m.set(c0, 256);
	m.set(buf(1), 256);
	return m.p();
	}
	else if (isbmp > 0) {
	static int col = 0;
	if (++col >= 24) col = 0;
	m.set(2, 8);
	m.set(col, 24);
	m.set(buf(isbmp) + buf(3) >> 4, 32);
	m.set(c0, 256);
	return m.p();
	}
#endif
	
	// Normal model
	if (bpos == 0) {
		for (int i = 15; i > 0; --i)  // update order 0-11 context hashes
			cxt[i] = cxt[i - 1] * 257 + (c4 & 255) + 1;
		for (int i = 0; i < 7; ++i)
			cm.set(cxt[i]);
		rcm7.set(cxt[7]);
		cm.set(cxt[8]);
		rcm9.set(cxt[10]);
		rcm10.set(cxt[12]);
		cm.set(cxt[14]);
	}
	int order = cm.mix(m);

	rcm7.mix(m);
	rcm9.mix(m);
	rcm10.mix(m);

#if 0
	if (LEVEL >= 4) {
	sparseModel(m, ismatch, order);
	distanceModel(m);

	recordModel(m);
	wordModel(m);
	indirectModel(m);
	dmcModel(m);
	if (filetype == EXE) exeModel(m);
	}
	
#if 1
	if (LEVEL >= 4) {
		picModel(m);
	}
#endif 

#endif 
	order = order - 2;
	if (order < 0) order = 0;

	U32 c1 = buf(1), c2 = buf(2), c3 = buf(3), c;

	m.set(c1 + 8, 264);
	m.set(c0, 256);
	m.set(order + 8 * (c4 >> 5 & 7) + 64 * (c1 == c2) + 128 * (filetype == EXE), 256);
	m.set(c2, 256);
	m.set(c3, 256);
//		m.set(ismatch, 256);

	if (bpos) {
		c = c0 << (8 - bpos); if (bpos == 1)c += c3 / 2;
		c = (min(bpos, 5)) * 256 + c1 / 32 + 8 * (c2 / 32) + (c & 192);
	}
	else c = c3 / 128 + (c4 >> 31) * 2 + 4 * (c2 / 64) + (c1 & 240);
	m.set(c, 1536);
	int pr = m.p();
	return pr;
}


Predictor::Predictor() : pr(2048)   //pr�ĳ�ʼֵΪ2048
{
}

void Predictor::update()
{
	static APM a(256), a1(0x10000), a2(0x10000), a3(0x10000),    //ox10000 16^4 = 2^16
		a4(0x10000), a5(0x10000), a6(0x10000);

	// Update global context: pos, bpos, c0, c4, buf
	c0 += c0 + y;
	if (c0 >= 256) {
		buf[pos++] = c0;
		c4 = (c4 << 8) + c0 - 256;
		c0 = 1;
	}
	bpos = (bpos + 1) & 7;

	// Filter the context model with APMs
	int pr0 = contextModel2();

	pr = a.p(pr0, c0);
	
	int pr1 = a1.p(pr0, c0 + 256 * buf(1));
	int pr2 = a2.p(pr0, c0^hash(buf(1), buf(2)) & 0xffff);
	int pr3 = a3.p(pr0, c0^hash(buf(1), buf(2), buf(3)) & 0xffff);
	pr0 = pr0 + pr1 + pr2 + pr3 + 2 >> 2;

	pr1 = a4.p(pr, c0 + 256 * buf(1));
	pr2 = a5.p(pr, c0^hash(buf(1), buf(2)) & 0xffff);
	pr3 = a6.p(pr, c0^hash(buf(1), buf(2), buf(3)) & 0xffff);
	pr = pr + pr1 + pr2 + pr3 + 2 >> 2;
	
	pr = pr + pr0 + 1 >> 1;
}

#endif 