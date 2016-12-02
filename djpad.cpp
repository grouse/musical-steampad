#include <cstdio>
#include <cstdint>
#include <cinttypes>
#include <cstdlib>

#include <Windows.h>
#include "steam/steam_api.h"

#define VAR_UNUSED(var) (void)var
#define DEBUG_BREAK()       __debugbreak()
#define DEBUG_ASSERT(condition) if (!(condition)) DEBUG_BREAK()

void quit() {
	SteamController()->Shutdown();
	SteamAPI_Shutdown();

	_exit(0);
}

LRESULT CALLBACK window_proc(HWND   hwnd,
                             UINT   message,
                             WPARAM wparam,
                             LPARAM lparam)
{
	switch (message) {
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;

	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hwnd, &ps);

		FillRect(hdc, &ps.rcPaint, (HBRUSH) (COLOR_WINDOW+1));

		EndPaint(hwnd, &ps);
		break;
	}
	case WM_KEYDOWN: {
		switch (wparam) {
		case VK_ESCAPE:
			quit();
			break;

		default: break;
		}
		break;
	}

	default: return DefWindowProc(hwnd, message, wparam, lparam);

	}

	return 0;
}

void debug_print(const char *func,
                 uint32_t   line,
                 const char *file,
                 const char *msg)
{
	const char *type_str = "steam";

	// TODO(jesper): add log to file if enabled
	char buffer[512];
	std::sprintf(buffer, "%s:%d: %s in %s: %s\n", file, line, type_str, func, msg);
	OutputDebugString(buffer);
}

void debug_printf(const char *func,
                  uint32_t   line,
                  const char *file,
                  const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);

	char msg[512];
	std::vsprintf(msg, fmt, args);

	va_end(args);

	debug_print(func, line, file, msg);
}

#define DEBUG_FILENAME (strrchr(__FILE__, '\\') ? strrchr(__FILE__, '\\') + 1 : __FILE__)
#define DEBUG_LOGF(format, ...)                                                              \
	debug_printf(__FUNCTION__, __LINE__, DEBUG_FILENAME, format, __VA_ARGS__)

#define STEAM_CONTROLLER_MAGIC_PERIOD_RATIO 495483.0

LARGE_INTEGER clock_frequency;

void sleep_for(int64_t microseconds)
{
	LARGE_INTEGER start;
	QueryPerformanceCounter(&start);

	LARGE_INTEGER elapsed;
	do {
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);

		elapsed.QuadPart = now.QuadPart - start.QuadPart;
		elapsed.QuadPart *= 1000000;
		elapsed.QuadPart /= clock_frequency.QuadPart;
	} while(elapsed.QuadPart < microseconds);

	if (elapsed.QuadPart > microseconds) {
		DEBUG_LOGF("overslept for %d", (int32_t)(elapsed.QuadPart - microseconds));
	}
}

int64_t get_time_microseconds()
{
	LARGE_INTEGER time;
	QueryPerformanceCounter(&time);

	time.QuadPart *= 1000000;
	time.QuadPart /= clock_frequency.QuadPart;
	return (int64_t)time.QuadPart;
}

void play_frequency(ControllerHandle_t handle, double frequency, int64_t microseconds)
{
	double period = 1.0f / frequency;
	uint16_t pulse = (uint16_t)(period * 1000000);

	uint16_t repeat = (uint16_t)(microseconds / period);

	SteamController()->TriggerRepeatedHapticPulse(handle, k_ESteamControllerPad_Left, pulse, pulse, repeat, 0);
}

void play_frequency_sync(ControllerHandle_t handle, double frequency, int64_t microseconds)
{
	play_frequency(handle, frequency, microseconds);
	sleep_for(microseconds);
}

char *read_entire_file(const char *path, size_t *out_size)
{
	FILE *file = fopen(path, "rb");
	if (file != nullptr) {
		fseek(file, 0, SEEK_END);
		size_t size = ftell(file);
		rewind(file);

		char *data = (char*)malloc(size);
		size_t result = fread(data, 1, size, file);

		if (result == size) {
			*out_size = size;
			return data;
		} else {
			free(data);
		}
	}

	return nullptr;
}

uint16_t swap_endian(uint16_t val)
{
	return ((val << 8) | (val >> 8));
}

uint32_t swap_endian(uint32_t val)
{
	return ((val << 24) |
	        ((val << 8) & 0x00ff0000) |
	        ((val >> 8) & 0x0000ff00) |
	        (val >> 24));
}

#pragma pack(1)
enum MidiChunkType {
	MidiChunkType_unknown,
	MidiChunkType_header,
	MidiChunkType_track
};

struct MidiChunk {
	uint8_t  type[4];
	uint32_t length;
};

struct MidiHeader {
	uint16_t format;
	uint16_t ntracks;
	uint16_t tickdiv;
};


enum DeltaMode {
	DeltaMode_metrical,
	DeltaMode_timecode
};


MidiChunkType get_midi_chunk_type(MidiChunk chunk)
{
	if (chunk.type[0] == 'M' && chunk.type[1] == 'T')
	{
		if (chunk.type[2] == 'h' && chunk.type[3] == 'd') {
			return MidiChunkType_header;
		} else if (chunk.type[2] == 'r' && chunk.type[3] == 'k') {
			return MidiChunkType_track;
		}
	}
	return MidiChunkType_unknown;
}

int APIENTRY WinMain(HINSTANCE hInstance,
                     HINSTANCE hPrevInstance,
                     LPSTR     lpCmdLine,
                     int       nCmdShow)
{
	VAR_UNUSED(hInstance);
	VAR_UNUSED(hPrevInstance);
	VAR_UNUSED(lpCmdLine);
	VAR_UNUSED(nCmdShow);

#if 0
	size_t size;
	char *midi = read_entire_file("../opening.mid", &size);
	if (midi == nullptr) {
		return -1;
	}

	DeltaMode track_mode;
	uint16_t divisor;

	char *end = midi + size;
	char *ptr = midi;
	do {
		MidiChunk chunk;
		memcpy(&chunk, ptr, sizeof(MidiChunk));
		ptr += sizeof(MidiChunk);

		chunk.length = swap_endian(chunk.length);

		MidiChunkType type = get_midi_chunk_type(chunk);
		switch (type) {
		case MidiChunkType_header: {
			MidiHeader header;
			memcpy(&header, ptr, sizeof(MidiHeader));
			ptr += sizeof(MidiHeader);

			header.format  = swap_endian(header.format);
			header.ntracks = swap_endian(header.ntracks);
			header.tickdiv = swap_endian(header.tickdiv);

			if (header.tickdiv & 0x8000) {
				track_mode = DeltaMode_timecode;
			} else {
				track_mode = DeltaMode_metrical;
				divisor = header.tickdiv >> 1;
			}
		} break;
		case MidiChunkType_track: {
			uint32_t delta_time = (*ptr & ~0x80);

			if ((*ptr) & 0x80) {
				DEBUG_LOGF("handle and verify variable length");
			}

			uint8_t status = *++ptr;

			if (status & 0x80) {
				if (status == 0xff) { // meta events
					uint8_t event_type    = *++ptr;
					uint32_t event_length = *++ptr & ~0x80;

					if ((*ptr & 0x80)) {
						DEBUG_LOGF("handle and verify variable length");
					}

					switch (event_type) {
					case 0x00: { // sequence number
						DEBUG_ASSERT(event_length == 4);
						ptr += event_length;
					} break;
					case 0x01: { // text
						ptr += event_length;
					} break;
					case 0x02: { // copyright
						ptr += event_length;
					} break;
					case 0x03: { // sequence/track name
						ptr += event_length+1;
					} break;
					case 0x04: { // instrument name
						ptr += event_length;
					} break;
					case 0x05: { // lyric
						ptr += event_length;
					} break;
					case 0x06: { // marker
						ptr += event_length;
					} break;
					case 0x07: { // cue point
						ptr += event_length;
					} break;
					case 0x08: { // program name
						ptr += event_length;
					} break;
					case 0x09: { // device name
						ptr += event_length;
					} break;
					case 0x20: { // midi channel prefix
						DEBUG_ASSERT(ptr[0] == 0x01);
						DEBUG_ASSERT(event_length == 3);
						ptr += event_length;
					} break;
					case 0x21: { // midi port
						DEBUG_ASSERT(ptr[0] == 0x01);
						DEBUG_ASSERT(event_length == 3);
						ptr += event_length;
					} break;
					case 0x2f: { // end of track
						DEBUG_ASSERT(ptr[0] == 0x00);
						DEBUG_ASSERT(event_length == 2);
						ptr += event_length;
					} break;
					case 0x51: { // tempo
						DEBUG_ASSERT(ptr[0] == 0x03);
						DEBUG_ASSERT(event_length == 5);
						ptr += event_length;
					} break;
					case 0x54: { // smpte offset
						DEBUG_ASSERT(ptr[0] == 0x05);
						DEBUG_ASSERT(event_length == 7);
						ptr += event_length;
					} break;
					case 0x58: { // time signature
						DEBUG_ASSERT(ptr[0] == 0x04);
						DEBUG_ASSERT(event_length == 6);
						ptr += event_length;
					} break;
					case 0x59: { // key signature
						DEBUG_ASSERT(ptr[0] == 0x02);
						DEBUG_ASSERT(event_length == 4);
						ptr += event_length;
					} break;
					case 0x7f: { // sequencer specific event
						ptr += event_length;
					} break;
					default:
						DEBUG_LOGF("unknown event type: %d", event_type);
						break;
					}

					DEBUG_LOGF("event type  : %d", event_type);
					DEBUG_LOGF("event length: %d", event_length);
				} else if (status == 0xf0) { // sysex events
					DEBUG_LOGF("unknown midi chunk type: %d", delta_time);
				} else { // midi events
					DEBUG_LOGF("unknown midi chunk type: %d", delta_time);
				}
			}


			DEBUG_LOGF("unknown midi chunk type: %.*s", 4, chunk.type);

		} break;
		default: {
			DEBUG_LOGF("unknown midi chunk type: %.*s", 4, chunk.type);
			return -1;
		} break;
		}
	} while (ptr < end);
#endif


	QueryPerformanceFrequency(&clock_frequency);

	WNDCLASS wc = {};
	wc.lpfnWndProc   = window_proc;
	wc.hInstance     = hInstance;;
	wc.lpszClassName = "leary";

	RegisterClass(&wc);

	HWND hwnd = CreateWindow("leary",
	                         "leary",
	                         WS_TILED | WS_VISIBLE,
	                         0, 0,
	                         1280, 720,
	                         nullptr,
	                         nullptr,
	                         hInstance,
	                         nullptr);

	enum Notes {
		Note_A,
		Note_B,
		Note_C,
		Note_D,
		Note_E,
		Note_F,
		Note_G
	};

	double frequencies[] = {
		1760.00, // A
		1975.53, // B
		2093.00, // C
		2349.32, // D
		2637.02, // E
		2793.83, // F
		3135.96, // G
	};

	if (hwnd == nullptr) {
		quit();
	}

#if 0
	if (!SteamAPI_IsSteamRunning()) {
		OutputDebugString("steam isn't running, start it\n");
		return -1;
	}
#endif
	if (!SteamAPI_Init() ) {
		OutputDebugString( "SteamAPI_Init() failed\n" );
		return -1;
	}

	if (!SteamController()->Init() ) {
		OutputDebugString( "SteamController()->Init failed.\n" );
		return -1;
	}

	int num_controllers = 0;
	ControllerHandle_t controller_handles[STEAM_CONTROLLER_MAX_COUNT];


	MSG msg;
	while(true) {
		while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}

		if (msg.message == WM_QUIT) {
			quit();
		}

		if (num_controllers == 0) {
			num_controllers = SteamController()->GetConnectedControllers(controller_handles);
		}

		if (num_controllers != 0) {
			sleep_for(2000000);

			play_frequency_sync(controller_handles[0], frequencies[Note_F], 125000);
			sleep_for(125000);
			play_frequency_sync(controller_handles[0], frequencies[Note_A], 125000);
			sleep_for(125000);
			play_frequency_sync(controller_handles[0], frequencies[Note_B], 125000);
			sleep_for(125000);

			play_frequency_sync(controller_handles[0], frequencies[Note_F], 125000);
			sleep_for(125000);
			play_frequency_sync(controller_handles[0], frequencies[Note_A], 125000);
			sleep_for(125000);
			play_frequency_sync(controller_handles[0], frequencies[Note_B], 125000);
			sleep_for(125000);

			play_frequency_sync(controller_handles[0], frequencies[Note_F], 125000);
			sleep_for(125000);
			play_frequency_sync(controller_handles[0], frequencies[Note_A], 125000);
			sleep_for(125000);
			play_frequency_sync(controller_handles[0], frequencies[Note_B], 125000);
			sleep_for(125000);

			play_frequency_sync(controller_handles[0], frequencies[Note_E], 125000);
			sleep_for(125000);
			play_frequency_sync(controller_handles[0], frequencies[Note_D], 125000);
			sleep_for(125000);
			play_frequency_sync(controller_handles[0], frequencies[Note_B], 125000);
			sleep_for(125000);
			play_frequency_sync(controller_handles[0], frequencies[Note_C], 125000);
			sleep_for(125000);
			play_frequency_sync(controller_handles[0], frequencies[Note_B], 125000);
			sleep_for(125000);
			play_frequency_sync(controller_handles[0], frequencies[Note_G], 125000);
			sleep_for(125000);
			play_frequency_sync(controller_handles[0], frequencies[Note_E], 125000);
			sleep_for(125000);
			play_frequency_sync(controller_handles[0], frequencies[Note_D], 125000);
			sleep_for(125000);
			play_frequency_sync(controller_handles[0], frequencies[Note_E], 125000);
			sleep_for(125000);
			play_frequency_sync(controller_handles[0], frequencies[Note_G], 125000);
			sleep_for(125000);
			play_frequency_sync(controller_handles[0], frequencies[Note_E], 125000);
			sleep_for(125000);

			sleep_for(2000000);
			return 0;
		}
		SteamAPI_RunCallbacks();
	}

	return 0;
}
