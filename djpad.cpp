#include <cstdio>
#include <cstdint>
#include <cinttypes>
#include <cstdlib>
#include <vector>

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

void play_frequency(ControllerHandle_t handle,
                    int32_t pad,
                    double frequency,
                    double duration)
{
	double period = 1.0f / frequency;
	uint16_t pulse = (uint16_t)(period * 1000000);

	uint16_t repeat = (duration >= 0.0) ? (duration / period) : 0x7FFF;

	SteamController()->TriggerRepeatedHapticPulse(handle,
	                                              (ESteamControllerPad)pad,
	                                              pulse, pulse,
	                                              repeat, 0);
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

enum MidiFileEventType {
	MidiFileEventType_note_on,
	MidiFileEventType_note_off,
	MidiFileEventType_tempo
};

enum MidiDivisorType {
	MidiDivisorType_ppq,
	MidiDivisorType_smpte24,
	MidiDivisorType_smpte25,
	MidiDivisorType_smpte30_drop,
	MidiDivisorType_smpte30
};

enum MidiChunkType {
	MidiChunkType_unknown,
	MidiChunkType_header,
	MidiChunkType_track
};

struct MidiFileEvent {
	MidiFileEvent *next = nullptr;

	int64_t ticks;
	MidiFileEventType type;
	union {
		struct {
			uint8_t note;
			uint8_t velocity;
			uint8_t channel;
		} note;
		uint32_t tempo;
	};
};

struct MidiFile {
	uint16_t format;
	uint16_t ntracks;

	uint16_t divisor;
	MidiDivisorType divisor_type;

	MidiFileEvent *first_event;
	MidiFileEvent *last_evnt;
};

double midi_notes[128] = {
	8.1758,  8.66196, 9.17702, 9.72272, 10.3009, 10.9134, 11.5623, 12.2499, 12.9783, 13.75, 14.5676, 15.4339,
	16.3516, 17.3239, 18.354,  19.4454, 20.6017, 21.8268, 23.1247, 24.4997, 25.9565, 27.5,  29.1352, 30.8677,
	32.7032, 34.6478, 36.7081, 38.8909, 41.2034, 43.6535, 46.2493, 48.9994, 51.9131, 55,    58.2705, 61.7354,
	65.4064, 69.2957, 73.4162, 77.7817, 82.4069, 87.3071, 92.4986, 97.9989, 103.826, 110,   116.541, 123.471,
	130.813, 138.591, 146.832, 155.563, 164.814, 174.614, 184.997, 195.998, 207.652, 220,   233.082, 246.942,
	261.626, 277.183, 293.665, 311.127, 329.628, 349.228, 369.994, 391.995, 415.305, 440,   466.164, 493.883,
	523.251, 554.365, 587.33,  622.254, 659.255, 698.456, 739.989, 783.991, 830.609, 880,   932.328, 987.767,
	1046.5,  1108.73, 1174.66, 1244.51, 1318.51, 1396.91, 1479.98, 1567.98, 1661.22, 1760,  1864.66, 1975.53,
	2093,    2217.46, 2349.32, 2489.02, 2637.02, 2793.83, 2959.96, 3135.96, 3322.44, 3520,  3729.31, 3951.07,
	4186.01, 4434.92, 4698.64, 4978.03, 5274.04, 5587.65, 5919.91, 6271.93, 6644.88, 7040,  7458.62, 7902.13,
	8372.02, 8869.84, 9397.27, 9956.06, 10548.1, 11175.3, 11839.8, 12543.9
};

uint32_t read_variable_length(char **ptr)
{
	uint32_t value = 0;

	uint8_t c = *(*ptr)++;
	value = c;

	if (c & 0x80) {
		value &= 0x7f;

		do {
			c = *(*ptr)++;
			value = (value << 7) | (c & 0x7f);
		} while (c & 0x80);
	}

	return value;
}


MidiChunkType read_midi_chunk_type(char **ptr)
{
	uint8_t b0 = *(*ptr)++;
	uint8_t b1 = *(*ptr)++;
	uint8_t b2 = *(*ptr)++;
	uint8_t b3 = *(*ptr)++;

	if (b0 == 'M' && b1 == 'T') {
		if (b2 == 'h' && b3 == 'd') {
			return MidiChunkType_header;
		} else if (b2 == 'r' && b3 == 'k') {
			return MidiChunkType_track;
		}
	}

	return MidiChunkType_unknown;
}

uint16_t read_16bit(char **ptr)
{
	uint8_t b0 = *(*ptr)++;
	uint8_t b1 = *(*ptr)++;

	uint16_t value = ((b0 << 8) | (b1 & 0x00FF));
	return value;
}

uint16_t read_16bit(char *ptr)
{
	return ((ptr[0] << 8) | (ptr[1] & 0x00FF));
}

uint32_t read_24bit(char *ptr)
{
	return ((ptr[0] << 16)) |
	       ((ptr[1] << 8) & 0x00FF) |
	       ((ptr[2])      & 0x0000FF);
}

uint32_t read_32bit(char **ptr)
{
	uint8_t b0 = *(*ptr)++;
	uint8_t b1 = *(*ptr)++;
	uint8_t b2 = *(*ptr)++;
	uint8_t b3 = *(*ptr)++;

	uint32_t value = ((b0 << 24) & 0xFF000000) |
	                 ((b1 << 16) & 0x00FF0000) |
	                 ((b2 << 8)  & 0x0000FF00) |
	                 ((b3)       & 0x000000FF);
	return value;
}

uint32_t read_32bit(char *ptr)
{
	return ((ptr[0] << 24)) |
	       ((ptr[1 ]<< 16) & 0x00FF) |
	       ((ptr[2] << 8)  & 0x0000FF) |
	       ((ptr[3])       & 0x000000FF);
}

void insert_midi_event(MidiFile *midi, MidiFileEvent *event)
{
	if (midi->first_event == nullptr) {
		midi->first_event = event;
	} else {
		MidiFileEvent *it = midi->first_event;
		do {
			if (it->next == nullptr) {
				it->next = event;
				break;
			} else if (it->next->ticks > event->ticks) {
				event->next = it->next;
				it->next = event;
				break;
			}

			it = it->next;
		} while (it);
	}
}

int64_t
parse_midi_track(MidiFile *midi, int64_t torigin, char *ptr, char *end)
{
	int64_t ticks = torigin;
	uint8_t status, running_status = 0;
	bool end_of_track = false;

	while (ptr < end && !end_of_track) {
		uint32_t delta_ticks = read_variable_length(&ptr);
		ticks += delta_ticks;

		status = ptr[0];
		if ((status & 0x80) == 0x00) {
			status = running_status;
		} else {
			running_status = status;
			ptr++;
		}

		if (status >= 0x80 && status <= 0xEF) { // midi events
			switch (status & 0xF0) {
			case 0x80: {
				MidiFileEvent *event = new MidiFileEvent;

				event->ticks = ticks;
				event->type          = MidiFileEventType_note_off;
				event->note.note     = *ptr++;
				event->note.velocity = *ptr++;
				event->note.channel  = status & 0x0F;

				insert_midi_event(midi, event);
			} break;
			case 0x90: {
				MidiFileEvent *event = new MidiFileEvent;

				event->ticks = ticks;
				event->type          = MidiFileEventType_note_on;
				event->note.note     = *ptr++;
				event->note.velocity = *ptr++;
				event->note.channel  = status & 0x0F;

				insert_midi_event(midi, event);
			} break;
			default:
				DEBUG_LOGF("unimplemented midi event type: 0x%20x",
					       status & 0xF0);
				break;
			}
		} else if (status == 0xF0 || status == 0xF7) { // sysex events
			uint32_t length = read_variable_length(&ptr);
			ptr += length;
			// NOTE(jesper): double check this for the different kinds, might
			// have to be + 1
			DEBUG_BREAK();
		} else if (status == 0xFF) { // meta events
			uint8_t  type   = *ptr++;
			uint32_t length = read_variable_length(&ptr);

			switch (type) {
			case 0x01: // text
			case 0x02: // copyright
			case 0x03: // name
			case 0x04: // instrument
			case 0x05: // lyric
			case 0x06: // marker
			case 0x07: // cue point
			case 0x08: // program name
			case 0x09: // device name
				DEBUG_LOGF("text: %.*s", length, ptr);
				break;
			case 0x51: { // tempo
				MidiFileEvent *event = new MidiFileEvent;

				event->ticks = ticks;
				event->type  = MidiFileEventType_tempo;
				event->tempo = read_24bit(ptr);

				insert_midi_event(midi, event);
			} break;
			case 0x2F:
				end_of_track = true;
				break;
			default:
				DEBUG_LOGF("unimplemented meta event type: 0x%02x", status);
				break;
			}

			ptr += length;
		} else {
			DEBUG_LOGF("skipping over unknown status in track: 0x%02x", status);
		}
	}

	return ticks - torigin;
}

MidiFile read_midi_file(const char *path)
{
	MidiFile midi = {};

	size_t size;
	char *file = read_entire_file(path, &size);
	if (file == nullptr) {
		return midi;
	}


	MidiChunkType chunk_type = read_midi_chunk_type(&file);
	uint32_t chunk_length    = read_32bit(&file);
	DEBUG_ASSERT(chunk_type == MidiChunkType_header);

	char *ptr = file;
	midi.format  = read_16bit(&ptr);
	midi.ntracks = read_16bit(&ptr);

	uint8_t divisor    = *ptr++;
	uint8_t resolution = *ptr++;

	file += chunk_length;

	if (divisor & 0x80) {
		DEBUG_BREAK();
	} else {
		midi.divisor_type = MidiDivisorType_ppq;
		midi.divisor = (divisor << 8) | resolution;
	}

	int64_t torigin = 0;

	for (int32_t i = 0; i < (int32_t)midi.ntracks; ++i) {
		chunk_type   = read_midi_chunk_type(&file);
		chunk_length = read_32bit(&file);
		DEBUG_ASSERT(chunk_type == MidiChunkType_track);

		int64_t ticks = parse_midi_track(&midi, torigin,
		                                 file, file + chunk_length);

		if (midi.format == 2) { // series of tracks
			torigin += ticks;
		}

		file += chunk_length;
	}

	return midi;
}


int64_t midi_ticks_to_time(MidiFile *midi,
                           uint32_t tempo,
                           int64_t tempo_event_ticks,
                           int64_t ticks)
{
	int64_t microseconds = 0;

	switch (midi->divisor_type) {
	case MidiDivisorType_ppq: {
		float tempo_float = 120.0f;
		if (tempo > 0) {
			tempo_float = (float)(60000000.0 / tempo);
		}
		float seconds = (((float)(ticks - tempo_event_ticks)) / midi->divisor / (tempo_float / 60));
		microseconds = (int64_t)(seconds * 1000000);
	} break;
	default:
		DEBUG_BREAK();
		break;
	}

	return microseconds;
}

void play_midi_file(MidiFile *midi,
                    int32_t num_controllers,
                    ControllerHandle_t *controllers)
{
	// TODO(jesper): support more midi channels using multiple controllers
	VAR_UNUSED(num_controllers);

	uint32_t tempo = 0;
	int64_t tempo_event_ticks = 0;

	int64_t last_time = 0;
	for (MidiFileEvent *event = midi->first_event;
	     event != nullptr;
	     event = event->next)
	{
		int64_t time = midi_ticks_to_time(midi, tempo, tempo_event_ticks, event->ticks);
		int64_t duration = time - last_time;
		last_time = time;

		if (duration > 0) {
			sleep_for(duration);
		}

		switch (event->type) {
		case MidiFileEventType_note_on:
			DEBUG_LOGF("midi event { time: %d, note: %d ON, velocity: %d, channel: %d }",
				       event->ticks,
				       event->note.note,
				       event->note.velocity,
				       event->note.channel);

			if (event->note.channel == 0 || event->note.channel == 1) {
				play_frequency(controllers[0], event->note.channel, midi_notes[event->note.note], 0.25);
			}
			break;
		case MidiFileEventType_note_off:
			DEBUG_LOGF("midi event { time: %d, note: %d OFF, velocity: %d, channel: %d }",
				       event->ticks,
				       event->note.note,
				       event->note.velocity,
				       event->note.channel);
			if (event->note.channel == 0 || event->note.channel == 1) {
				play_frequency(controllers[0], event->note.channel, 0.0, 0.0);
			}
			break;
		case MidiFileEventType_tempo:
			DEBUG_LOGF("midi event { time: %d, tempo: %d",
			           event->ticks);
			tempo = event->tempo;
			tempo_event_ticks = event->ticks;
			break;
		}
	}

	play_frequency(controllers[0], 0, 0.0, 0);
	play_frequency(controllers[0], 1, 0.0, 0);
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

	MidiFile midi = read_midi_file("../bicycle-ride.mid");

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

	if (hwnd == nullptr) {
		quit();
	}

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
			play_midi_file(&midi, num_controllers, controller_handles);
			return 0;
		}
		SteamAPI_RunCallbacks();
	}

	return 0;
}
