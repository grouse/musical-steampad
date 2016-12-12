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

enum MidiChunkType {
	MidiChunkType_unknown,
	MidiChunkType_header,
	MidiChunkType_track
};

enum DeltaMode {
	DeltaMode_metrical,
	DeltaMode_timecode
};

struct MidiHeader {
	uint16_t format;
	uint16_t ntracks;
	uint16_t division;
	DeltaMode track_mode;
};

enum MidiEventType {
	MidiEventType_note_off,
	MidiEventType_note_on,
	MidiEventType_polyphonic_pressure,
	MidiEventType_controller,
	MidiEventType_program_change,
	MidiEventType_channel_pressure,
	MidiEventType_pitch_bend,

	MidiEventType_unknown
};

MidiEventType midi_event_type(uint8_t c)
{
	switch (c) {
	case 0x80: return MidiEventType_note_off;
	case 0x90: return MidiEventType_note_on;
	case 0xA0: return MidiEventType_polyphonic_pressure;
	case 0xB0: return MidiEventType_controller;
	case 0xC0: return MidiEventType_program_change;
	case 0xD0: return MidiEventType_channel_pressure;
	case 0xE0: return MidiEventType_pitch_bend;
	default:   return MidiEventType_unknown;
	}
}

struct MidiEvent {
	uint8_t       channel;
	MidiEventType type;

	union {
		struct {
			uint8_t note;
			uint8_t velocity;
		} note;

		struct {
			uint8_t note;
			uint8_t pressure;
		} polyphonic_pressure;

		struct {
			uint8_t controller;
			uint8_t value;
		} controller;

		uint8_t program_change;
		uint8_t channel_pressure;

		struct {
			union {
				struct {
					uint8_t lsb;
					uint8_t msb;
				} bytes;
				uint16_t value;
			};
		} pitch_bend;
	};
};

enum MidiMetaEventType {
	MidiMetaEventType_sequence_number,
	MidiMetaEventType_text,
	MidiMetaEventType_copyright,
	MidiMetaEventType_name,
	MidiMetaEventType_instrument,
	MidiMetaEventType_lyric,
	MidiMetaEventType_marker,
	MidiMetaEventType_cue_point,
	MidiMetaEventType_program_name,
	MidiMetaEventType_device_name,
	MidiMetaEventType_midi_channel_prefix,
	MidiMetaEventType_midi_port,
	MidiMetaEventType_end_of_track,
	MidiMetaEventType_tempo,
	MidiMetaEventType_smpte_offset,
	MidiMetaEventType_time_signature,
	MidiMetaEventType_key_signature,
	MidiMetaEventType_sequencer_event,

	MidiMetaEventType_unknown
};

MidiMetaEventType midi_meta_event_type(uint8_t c)
{
	switch (c) {
	case 0x00: return MidiMetaEventType_sequence_number;
	case 0x01: return MidiMetaEventType_text;
	case 0x02: return MidiMetaEventType_copyright;
	case 0x03: return MidiMetaEventType_name;
	case 0x04: return MidiMetaEventType_instrument;
	case 0x05: return MidiMetaEventType_lyric;
	case 0x06: return MidiMetaEventType_marker;
	case 0x07: return MidiMetaEventType_cue_point;
	case 0x08: return MidiMetaEventType_program_name;
	case 0x09: return MidiMetaEventType_device_name;
	case 0x20: return MidiMetaEventType_midi_channel_prefix;
	case 0x21: return MidiMetaEventType_midi_port;
	case 0x2F: return MidiMetaEventType_end_of_track;
	case 0x51: return MidiMetaEventType_tempo;
	case 0x54: return MidiMetaEventType_smpte_offset;
	case 0x58: return MidiMetaEventType_time_signature;
	case 0x59: return MidiMetaEventType_key_signature;
	case 0x7F: return MidiMetaEventType_sequencer_event;
	default:   return MidiMetaEventType_unknown;
	}
}

struct String {
	uint32_t length;
	const char *str;
};

struct MidiTrack {
	uint16_t sequence_number;
	uint8_t channel_prefix;
	uint8_t midi_port;
	uint32_t tempo;
	String instrument;
	String name;
	struct {
		uint8_t hours;
		uint8_t minutes;
		uint8_t seconds;
		uint8_t frames;
		uint8_t fractional_frames;
	} smpte_offset;
	struct {
		uint8_t numerator;
		uint8_t denominator;
		uint8_t num_clocks;
		uint8_t num_32nd_notes;
	} time_signature;
	struct {
		uint8_t flats_or_sharps;
		uint8_t major_or_minor;
	} key_signature;
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

double midi_notes[] = {
	16.35,   17.32,   18.35,   19.45,   20.60,   21.83,   23.12,   24.50,   25.96,   27.50,   29.14,   30.87,
	32.70,   34.65,   36.71,   38.89,   41.20,   43.65,   46.25,   49.00,   51.91,   55.00,   58.27,   61.74,
	65.41,   69.30,   73.42,   77.78,   82.41,   87.31,   92.50,   98.00,   103.83,  110.00,  116.54,  123.47,
	130.81,  138.59,  146.83,  155.56,  164.81,  174.61,  185.00,  196.00,  207.65,  220.00,  233.08,  246.94,
	261.63,  277.18,  293.66,  311.13,  329.63,  349.23,  369.99,  392.00,  415.30,  440.00,  466.16,  493.88,
	523.25,  554.37,  587.33,  622.25,  659.25,  698.46,  739.99,  783.99,  830.61,  880.00,  932.33,  987.77,
	1046.50, 1108.73, 1174.66, 1244.51, 1318.51, 1396.91, 1479.98, 1567.98, 1661.22, 1760.00, 1864.66, 1975.53,
	2093.00, 2217.46, 2349.32, 2489.02, 2637.02, 2793.83, 2959.96, 3135.96, 3322.44, 3520.00, 3729.31, 3951.07,
	4186.01, 4434.92, 4698.63, 4978.03, 5274.04, 5587.65, 5919.91, 6271.93, 6644.88, 7040.00, 7458.62, 7902.13
};

void parse_midi_track_chunk(MidiHeader &header,
                            MidiTrack &track,
                            ControllerHandle_t controller,
                            char *ptr,
                            char *end)
{
	VAR_UNUSED(header);


	bool end_of_track = false;

	do {
		uint32_t delta_time = read_variable_length(&ptr);
		DEBUG_LOGF("delta time: %d", delta_time);

		uint8_t status = *ptr++;

		if (status >= 0x80 && status <= 0xEF) { // midi events
			uint8_t midi_status = status & 0xF0;
			uint8_t channel     = status & 0x0F;
			uint8_t running     = 1;

			while (running) {
				MidiEvent event = {};
				event.channel = channel;
				event.type    = midi_event_type(midi_status);

				switch (event.type) {
				case MidiEventType_note_off: {
					event.note.note     = *ptr++;
					event.note.velocity = *ptr++;

					DEBUG_LOGF("channel: %d, note OFF: %d"
					           event.channel, event.note.note);
					play_frequency_sync(controller, 0.0f, 0);
				} break;
				case MidiEventType_note_on: {
					event.note.note     = *ptr++;
					event.note.velocity = *ptr++;

					DEBUG_LOGF("channel: %d, note ON: %d, duration: %f ms",
					           event.channel,
					           event.note.note,
					           track.tempo / 1000.0f);
					play_frequency_sync(controller,
					                    midi_notes[event.note.note - 12],
					                    track.tempo / 2);
				} break;
				case MidiEventType_polyphonic_pressure: {
					event.polyphonic_pressure.note     = *ptr++;
					event.polyphonic_pressure.pressure = *ptr++;
				} break;
				case MidiEventType_controller: {
					event.controller.controller = *ptr++;
					event.controller.value      = *ptr++;
				} break;
				case MidiEventType_program_change: {
					event.program_change = *ptr++;
				} break;
				case MidiEventType_channel_pressure: {
					event.channel_pressure = *ptr++;
				} break;
				case MidiEventType_pitch_bend: {
					event.pitch_bend.bytes.lsb = *ptr++;
					event.pitch_bend.bytes.msb = *ptr++;
				} break;
				default:
					DEBUG_BREAK();
					break;
				}

				if ((ptr[0] & 0x80) != 0) {
					running = 0;
				}
			}
		} else if (status == 0xF0) { // sysex events
			uint32_t length = read_variable_length(&ptr);

			ptr += length + 1; // read past the ending F7 byte
			//DEBUG_ASSERT(ptr[-1] == 0xF7);

			DEBUG_LOGF("unimplemented");
		} else if (status == 0xF7) { // escape sequences
			DEBUG_LOGF("unimplemented");
			DEBUG_BREAK();
		} else if (status == 0xFF) { // meta events
			MidiMetaEventType type = midi_meta_event_type(*ptr++);
			uint32_t event_length = read_variable_length(&ptr);

			switch (type) {
			case MidiMetaEventType_sequence_number: {
				track.sequence_number = *(uint16*)ptr;
			} break;
			case MidiMetaEventType_instrument: {
				track.instrument.length = event_length;
				track.instrument.str    = ptr;
				DEBUG_LOGF("instrument: %.*s", event_length, ptr);
			} break;
			case MidiMetaEventType_text:
			case MidiMetaEventType_copyright:
				DEBUG_LOGF("%.*s", event_length, ptr);
				break;
			case MidiMetaEventType_name: {
				track.name.length = event_length;
				track.name.str = ptr;
				DEBUG_LOGF("track name: %.*s", event_length, ptr);
			} break;
			case MidiMetaEventType_lyric:
			case MidiMetaEventType_marker:
			case MidiMetaEventType_cue_point:
			case MidiMetaEventType_program_name:
			case MidiMetaEventType_device_name: {
				DEBUG_LOGF("text: %.*s", event_length, ptr);
			} break;
			case MidiMetaEventType_midi_channel_prefix: {
				track.channel_prefix = ptr[0];
				DEBUG_LOGF("midi channel prefix: %d", track.channel_prefix);
			} break;
			case MidiMetaEventType_midi_port: {
				track.midi_port = ptr[0];
				DEBUG_LOGF("midi port: %d", track.midi_port);
			} break;
			case MidiMetaEventType_end_of_track: {
				end_of_track = true;
				DEBUG_LOGF("end of track");
			} break;
			case MidiMetaEventType_tempo: {
				uint8_t b0 = ptr[0];
				uint8_t b1 = ptr[1];
				uint8_t b2 = ptr[2];

				track.tempo = (b0 << 16) |
				              ((b1 << 8) & 0x00ff) |
				              (b2 & 0x0000ff);
				DEBUG_LOGF("tempo change: %d", track.tempo);
			} break;
			case MidiMetaEventType_smpte_offset: {
				track.smpte_offset.hours             = ptr[0];
				track.smpte_offset.minutes           = ptr[1];
				track.smpte_offset.seconds           = ptr[2];
				track.smpte_offset.frames            = ptr[3];
				track.smpte_offset.fractional_frames = ptr[4];
				DEBUG_LOGF("smpte offset: %d:%d:%d - "
				           "frames: %d, fractional frames: %d",
				           track.smpte_offset.hours,
				           track.smpte_offset.minutes,
				           track.smpte_offset.seconds,
				           track.smpte_offset.frames,
				           track.smpte_offset.fractional_frames);
			} break;
			case MidiMetaEventType_time_signature: {
				track.time_signature.numerator      = ptr[0];
				track.time_signature.denominator    = ptr[1];
				track.time_signature.num_clocks     = ptr[2];
				track.time_signature.num_32nd_notes = ptr[3];
				DEBUG_LOGF("time signature { numerator: %d, denominator: %d, "
				           "num_clocks: %d, num_32nd_notes: %d }",
				           track.time_signature.numerator,
				           track.time_signature.denominator,
				           track.time_signature.num_clocks,
				           track.time_signature.num_32nd_notes);
			} break;
			case MidiMetaEventType_key_signature: {
				track.key_signature.flats_or_sharps = ptr[0];
				track.key_signature.major_or_minor  = ptr[1];
				DEBUG_LOGF("key signature { flats/sharps: %d, major/minor: %d",
				           track.key_signature.flats_or_sharps,
				           track.key_signature.major_or_minor);
			} break;
			case MidiMetaEventType_sequencer_event: {
				DEBUG_LOGF("\tSequencer specific event ??");
			} break;
			default:
				DEBUG_LOGF("skipping over unknown meta event: 0x%02x", type);
				continue;
			}

			ptr += event_length;
		} else {
			DEBUG_LOGF("skipping over unknown status in track: 0x%02x", status);
		}
	} while(!end_of_track && ptr < end);
}

uint16_t read_16bit(char **ptr)
{
	uint8_t b0 = *(*ptr)++;
	uint8_t b1 = *(*ptr)++;

	uint16_t value = ((b0 << 8) | (b1 & 0x00FF));
	return value;
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

MidiHeader parse_midi_header(char *ptr, char *end)
{
	VAR_UNUSED(end);

	MidiHeader header = {};

	header.format   = read_16bit(&ptr);
	header.ntracks  = read_16bit(&ptr);
	header.division = read_16bit(&ptr);

	if (header.division & 0x8000) {
		header.track_mode = DeltaMode_timecode;
	} else {
		header.track_mode = DeltaMode_metrical;
	}

	return header;
}

void play_midi_file(const char *file, ControllerHandle_t controller)
{
	size_t size;
	char *midi = read_entire_file(file, &size);
	if (midi == nullptr) {
		return;
	}

	MidiTrack track = {};
	MidiHeader header = {};

	char *end = midi + size;
	char *ptr = midi;
	do {
		MidiChunkType chunk_type = read_midi_chunk_type(&ptr);
		DEBUG_ASSERT(chunk_type != MidiChunkType_unknown);

		uint32_t chunk_length = read_32bit(&ptr);

		switch (chunk_type) {
		case MidiChunkType_header: {
			header = parse_midi_header(ptr, end);
		} break;
		case MidiChunkType_track: {
			parse_midi_track_chunk(header, track, controller, ptr, ptr + chunk_length);
		} break;
		default: {
			DEBUG_LOGF("unknown midi chunk type: %.*s", 4, chunk_type);
		} break;
		}

		ptr += chunk_length;
	} while (ptr < end);
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
			play_midi_file("../opening.mid", controller_handles[0]);
			return 0;
		}
		SteamAPI_RunCallbacks();
	}

	return 0;
}
