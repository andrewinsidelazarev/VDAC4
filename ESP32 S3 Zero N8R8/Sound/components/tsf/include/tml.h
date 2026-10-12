/* TinyMidiLoader - v0.7 - Minimalistic midi parsing library - https://github.com/schellingb/TinySoundFont
   VDAC4: altered copy (see components/tsf/README.md); lines changed are marked "VDAC4:".
                                     no warranty implied; use at your own risk
   Do this:
      #define TML_IMPLEMENTATION
   before you include this file in *one* C or C++ file to create the implementation.
   // i.e. it should look like this:
   #include ...
   #include ...
   #define TML_IMPLEMENTATION
   #include "tml.h"

   [OPTIONAL] #define TML_NO_STDIO to remove stdio dependency
   [OPTIONAL] #define TML_MALLOC, TML_REALLOC, and TML_FREE to avoid stdlib.h
   [OPTIONAL] #define TML_MEMCPY to avoid string.h

   LICENSE (ZLIB)

   Copyright (C) 2017, 2018, 2020 Bernhard Schelling

   This software is provided 'as-is', without any express or implied
   warranty.  In no event will the authors be held liable for any damages
   arising from the use of this software.

   Permission is granted to anyone to use this software for any purpose,
   including commercial applications, and to alter it and redistribute it
   freely, subject to the following restrictions:

   1. The origin of this software must not be misrepresented; you must not
      claim that you wrote the original software. If you use this software
      in a product, an acknowledgment in the product documentation would be
      appreciated but is not required.
   2. Altered source versions must be plainly marked as such, and must not be
      misrepresented as being the original software.
   3. This notice may not be removed or altered from any source distribution.

*/

#ifndef TML_INCLUDE_TML_INL
#define TML_INCLUDE_TML_INL

#ifdef __cplusplus
extern "C" {
#endif

// Define this if you want the API functions to be static
#ifdef TML_STATIC
#define TMLDEF static
#else
#define TMLDEF extern
#endif

// Channel message type
enum TMLMessageType
{
	TML_NOTE_OFF = 0x80, TML_NOTE_ON = 0x90, TML_KEY_PRESSURE = 0xA0, TML_CONTROL_CHANGE = 0xB0, TML_PROGRAM_CHANGE = 0xC0, TML_CHANNEL_PRESSURE = 0xD0, TML_PITCH_BEND = 0xE0, TML_SET_TEMPO = 0x51,
	TML_SYSTEM_EXCLUSIVE = 0xF0 // VDAC4: узнанный SysEx (вид — в channel, TMLSysexKind)
};

// VDAC4: SysEx, которые разбор узнаёт (сообщение TML_SYSTEM_EXCLUSIVE, вид — в channel):
// параметр — в pitch_bend (громкость и точная настройка, 0..16383) или в key/velocity.
enum TMLSysexKind
{
	TML_SYSEX_GM_ON = 1,       // GM System On
	TML_SYSEX_GM2_ON,          // GM2 System On
	TML_SYSEX_GM_OFF,          // GM System Off
	TML_SYSEX_GS_RESET,        // Roland GS Reset
	TML_SYSEX_XG_ON,           // Yamaha XG System On
	TML_SYSEX_MASTER_VOLUME,   // общая громкость: pitch_bend 0..16383 (Universal, GS, XG)
	TML_SYSEX_MASTER_FINE,     // общая точная настройка: pitch_bend, 8192 — 0, ±1 полутон
	TML_SYSEX_MASTER_COARSE,   // общая грубая настройка: key — полутоны + 64
	TML_SYSEX_KEY_SHIFT,       // GS Master Key Shift: key — полутоны + 64 (мелодические партии)
	TML_SYSEX_GS_RHYTHM        // GS Use for Rhythm Part: key — канал, velocity — 0 (нет), 1, 2 (набор)
};

// Midi controller numbers
enum TMLController
{
	TML_BANK_SELECT_MSB,      TML_MODULATIONWHEEL_MSB, TML_BREATH_MSB,       TML_FOOT_MSB = 4,      TML_PORTAMENTO_TIME_MSB,   TML_DATA_ENTRY_MSB, TML_VOLUME_MSB,
	TML_BALANCE_MSB,          TML_PAN_MSB = 10,        TML_EXPRESSION_MSB,   TML_EFFECTS1_MSB,      TML_EFFECTS2_MSB,          TML_GPC1_MSB = 16, TML_GPC2_MSB, TML_GPC3_MSB, TML_GPC4_MSB,
	TML_BANK_SELECT_LSB = 32, TML_MODULATIONWHEEL_LSB, TML_BREATH_LSB,       TML_FOOT_LSB = 36,     TML_PORTAMENTO_TIME_LSB,   TML_DATA_ENTRY_LSB, TML_VOLUME_LSB,
	TML_BALANCE_LSB,          TML_PAN_LSB = 42,        TML_EXPRESSION_LSB,   TML_EFFECTS1_LSB,      TML_EFFECTS2_LSB,          TML_GPC1_LSB = 48, TML_GPC2_LSB, TML_GPC3_LSB, TML_GPC4_LSB,
	TML_SUSTAIN_SWITCH = 64,  TML_PORTAMENTO_SWITCH,   TML_SOSTENUTO_SWITCH, TML_SOFT_PEDAL_SWITCH, TML_LEGATO_SWITCH,         TML_HOLD2_SWITCH,
	TML_SOUND_CTRL1,          TML_SOUND_CTRL2,         TML_SOUND_CTRL3,      TML_SOUND_CTRL4,       TML_SOUND_CTRL5,           TML_SOUND_CTRL6,
	TML_SOUND_CTRL7,          TML_SOUND_CTRL8,         TML_SOUND_CTRL9,      TML_SOUND_CTRL10,      TML_GPC5, TML_GPC6,        TML_GPC7, TML_GPC8,
	TML_PORTAMENTO_CTRL,      TML_FX_REVERB = 91,      TML_FX_TREMOLO,       TML_FX_CHORUS,         TML_FX_CELESTE_DETUNE,     TML_FX_PHASER,
	TML_DATA_ENTRY_INCR,      TML_DATA_ENTRY_DECR,     TML_NRPN_LSB,         TML_NRPN_MSB,          TML_RPN_LSB,               TML_RPN_MSB,
	TML_ALL_SOUND_OFF = 120,  TML_ALL_CTRL_OFF,        TML_LOCAL_CONTROL,    TML_ALL_NOTES_OFF,     TML_OMNI_OFF, TML_OMNI_ON, TML_POLY_OFF, TML_POLY_ON
};

// A single MIDI message linked to the next message in time
typedef struct tml_message
{
	// Time of the message in milliseconds
	unsigned int time;

	// Type (see TMLMessageType) and channel number
	unsigned char type, channel;

	// 2 byte of parameter data based on the type:
	// - key, velocity for TML_NOTE_ON and TML_NOTE_OFF messages
	// - key, key_pressure for TML_KEY_PRESSURE messages
	// - control, control_value for TML_CONTROL_CHANGE messages (see TMLController)
	// - program for TML_PROGRAM_CHANGE messages
	// - channel_pressure for TML_CHANNEL_PRESSURE messages
	// - pitch_bend for TML_PITCH_BEND messages
	union
	{
		#ifdef _MSC_VER
		#pragma warning(push)
		#pragma warning(disable:4201) //nonstandard extension used: nameless struct/union
		#elif defined(__GNUC__)
		#pragma GCC diagnostic push
		#pragma GCC diagnostic ignored "-Wpedantic" //ISO C++ prohibits anonymous structs
		#endif

		struct { union { char key, control, program, channel_pressure; }; union { char velocity, key_pressure, control_value; }; };
		struct { unsigned short pitch_bend; };

		#ifdef _MSC_VER
		#pragma warning( pop )
		#elif defined(__GNUC__)
		#pragma GCC diagnostic pop
		#endif
	};

	// The pointer to the next message in time following this event
	struct tml_message* next;
} tml_message;

// The load functions will return a pointer to a struct tml_message.
// Normally the linked list gets traversed by following the next pointers.
// Make sure to keep the pointer to the first message to free the memory.
// On error the tml_load* functions will return NULL most likely due to an
// invalid MIDI stream (or if the file did not exist in tml_load_filename).

#ifndef TML_NO_STDIO
// Directly load a MIDI file from a .mid file path
TMLDEF tml_message* tml_load_filename(const char* filename);
#endif

// Load a MIDI file from a block of memory
TMLDEF tml_message* tml_load_memory(const void* buffer, int size);

// Get infos about this loaded MIDI file, returns the note count
// NULL can be passed for any output value pointer if not needed.
//   used_channels:   Will be set to how many channels play notes
//                    (i.e. 1 if channel 15 is used but no other)
//   used_programs:   Will be set to how many different programs are used
//   total_notes:     Will be set to the total number of note on messages
//   time_first_note: Will be set to the time of the first note on message
//   time_length:     Will be set to the total time in milliseconds
TMLDEF int tml_get_info(tml_message* first_message, int* used_channels, int* used_programs, int* total_notes, unsigned int* time_first_note, unsigned int* time_length);

// Read the tempo (microseconds per quarter note) value from a message with the type TML_SET_TEMPO
TMLDEF int tml_get_tempo_value(tml_message* set_tempo_message);

// Free all the memory of the linked message list (can also call free() manually)
TMLDEF void tml_free(tml_message* f);

// Stream structure for the generic loading
struct tml_stream
{
	// Custom data given to the functions as the first parameter
	void* data;

	// Function pointer will be called to read 'size' bytes into ptr (returns number of read bytes)
	int (*read)(void* data, void* ptr, unsigned int size);
};

// Generic Midi loading method using the stream structure above
TMLDEF tml_message* tml_load(struct tml_stream* stream);

// If this library is used together with TinySoundFont, tsf_stream (equivalent to tml_stream) can also be used
struct tsf_stream;
TMLDEF tml_message* tml_load_tsf_stream(struct tsf_stream* stream);

#ifdef __cplusplus
}
#endif

// end header
// ---------------------------------------------------------------------------------------------------------
#endif //TML_INCLUDE_TML_INL

#ifdef TML_IMPLEMENTATION

#if !defined(TML_MALLOC) || !defined(TML_FREE) || !defined(TML_REALLOC)
#  include <stdlib.h>
#  define TML_MALLOC  malloc
#  define TML_FREE    free
#  define TML_REALLOC realloc
#endif

#if !defined(TML_MEMCPY)
#  include <string.h>
#  define TML_MEMCPY  memcpy
#endif

#ifndef TML_NO_STDIO
#  include <stdio.h>
#endif

#define TML_NULL 0

////crash on errors and warnings to find broken midi files while debugging
//#define TML_ERROR(msg) *(int*)0 = 0xbad;
//#define TML_WARN(msg)  *(int*)0 = 0xf00d;

////print errors and warnings
//#define TML_ERROR(msg) printf("ERROR: %s\n", msg);
//#define TML_WARN(msg)  printf("WARNING: %s\n", msg);

#ifndef TML_ERROR
#define TML_ERROR(msg)
#endif

#ifndef TML_WARN
#define TML_WARN(msg)
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef TML_NO_STDIO
static int tml_stream_stdio_read(FILE* f, void* ptr, unsigned int size) { return (int)fread(ptr, 1, size, f); }
TMLDEF tml_message* tml_load_filename(const char* filename)
{
	struct tml_message* res;
	struct tml_stream stream = { TML_NULL, (int(*)(void*,void*,unsigned int))&tml_stream_stdio_read };
	#if __STDC_WANT_SECURE_LIB__
	FILE* f = TML_NULL; fopen_s(&f, filename, "rb");
	#else
	FILE* f = fopen(filename, "rb");
	#endif
	if (!f) { TML_ERROR("File not found"); return 0; }
	stream.data = f;
	res = tml_load(&stream);
	fclose(f);
	return res;
}
#endif

struct tml_stream_memory { const char* buffer; unsigned int total, pos; };
static int tml_stream_memory_read(struct tml_stream_memory* m, void* ptr, unsigned int size) { if (size > m->total - m->pos) size = m->total - m->pos; TML_MEMCPY(ptr, m->buffer+m->pos, size); m->pos += size; return size; }
TMLDEF struct tml_message* tml_load_memory(const void* buffer, int size)
{
	struct tml_stream stream = { TML_NULL, (int(*)(void*,void*,unsigned int))&tml_stream_memory_read };
	struct tml_stream_memory f = { 0, 0, 0 };
	f.buffer = (const char*)buffer;
	f.total = size;
	stream.data = &f;
	return tml_load(&stream);
}

struct tml_track
{
	unsigned int Idx, End;
	unsigned long long Ticks; // VDAC4: 64 бита — паузы до 2^28 тиков законны и складываются
};

struct tml_tempomsg
{
	unsigned int time;
	unsigned char type, Tempo[3];
	tml_message* next;
};

struct tml_parser
{
	unsigned char *buf, *buf_end;
	int last_status, message_array_size, message_count;
	int failed; // VDAC4: памяти не хватило — песня не грузится целиком (не обрывком)
	// VDAC4: SysEx, который продолжается пакетами F7 (F0 без F7 в конце): собранное, длина,
	// 1 — собирается, 2 — длиннее буфера (не узнаётся).
	unsigned char sysex[32];
	int sysex_len, sysex_state;
};

enum TMLSystemType
{
	TML_TEXT  = 0x01, TML_COPYRIGHT    = 0x02, TML_TRACK_NAME     = 0x03, TML_INST_NAME     = 0x04, TML_LYRIC           = 0x05, TML_MARKER       = 0x06, TML_CUE_POINT = 0x07,
	TML_EOT   = 0x2f, TML_SMPTE_OFFSET = 0x54, TML_TIME_SIGNATURE = 0x58, TML_KEY_SIGNATURE = 0x59, TML_SEQUENCER_EVENT = 0x7f,
	TML_SYSEX = 0xf0, TML_TIME_CODE    = 0xf1, TML_SONG_POSITION  = 0xf2, TML_SONG_SELECT   = 0xf3, TML_TUNE_REQUEST    = 0xf6, TML_EOX          = 0xf7, TML_SYNC      = 0xf8,
	TML_TICK  = 0xf9, TML_START        = 0xfa, TML_CONTINUE       = 0xfb, TML_STOP          = 0xfc, TML_ACTIVE_SENSING  = 0xfe, TML_SYSTEM_RESET = 0xff
};

// VDAC4: песня длиннее суток — испорченный файл (паузы больше не обнуляются, см.
// tml_parsemessage): отказ, а не часы тишины.
#ifndef TML_MAX_MSEC
#define TML_MAX_MSEC 86400000u
#endif

// VDAC4: уступить ядро другим задачам в долгом разборе (каждые 4096 событий); по умолчанию — ничего.
#ifndef TML_REST
#define TML_REST() ((void)0)
#endif

// VDAC4: вид узнанного SysEx (d — байты после F0 до F7 включительно; 0 — не узнан).
static int tml_sysex_kind(const unsigned char* d, int n, int* a, int* b)
{
	if (n < 2 || d[n - 1] != 0xF7) return 0;
	n--;
	if (n == 4 && d[0] == 0x7E && d[2] == 0x09)
	{
		if (d[3] == 0x01) return TML_SYSEX_GM_ON;
		if (d[3] == 0x03) return TML_SYSEX_GM2_ON;
		if (d[3] == 0x02) return TML_SYSEX_GM_OFF;
	}
	if (n == 6 && d[0] == 0x7F && d[2] == 0x04)
	{
		if (d[3] == 0x01) { *a = d[4] | (d[5] << 7); return TML_SYSEX_MASTER_VOLUME; }
		if (d[3] == 0x03) { *a = d[4] | (d[5] << 7); return TML_SYSEX_MASTER_FINE; }
		if (d[3] == 0x04) { *a = d[5]; return TML_SYSEX_MASTER_COARSE; }
	}
	if (n >= 8 && d[0] == 0x41 && d[2] == 0x42 && d[3] == 0x12 && d[4] == 0x40) // Roland GS, DT1
	{
		if (d[5] == 0x00 && d[6] == 0x7F && d[7] == 0x00) return TML_SYSEX_GS_RESET;
		if (d[5] == 0x00 && d[6] == 0x04) { *a = d[7] * 16383 / 127; return TML_SYSEX_MASTER_VOLUME; }
		if (d[5] == 0x00 && d[6] == 0x05) { *a = d[7]; return TML_SYSEX_KEY_SHIFT; }
		if ((d[5] & 0xF0) == 0x10 && d[6] == 0x15)
		{
			// Партия x: 0 — десятая (канал 9), 1..9 — каналы 0..8, A..F — каналы 10..15.
			int part = d[5] & 0x0F;
			*a = (part == 0 ? 9 : (part <= 9 ? part - 1 : part));
			*b = d[7];
			return TML_SYSEX_GS_RHYTHM;
		}
	}
	if (n == 7 && d[0] == 0x43 && (d[1] & 0xF0) == 0x10 && d[2] == 0x4C && d[3] == 0x00 && d[4] == 0x00) // Yamaha XG
	{
		if (d[5] == 0x7E && d[6] == 0x00) return TML_SYSEX_XG_ON;
		if (d[5] == 0x04) { *a = d[6] * 16383 / 127; return TML_SYSEX_MASTER_VOLUME; }
	}
	return 0;
}

static int tml_readbyte(struct tml_parser* p)
{
	return (p->buf == p->buf_end ? -1 : *(p->buf++));
}

static int tml_readvariablelength(struct tml_parser* p)
{
	unsigned int res = 0, i = 0;
	unsigned char c;
	for (; i != 4; i++)
	{
		if (p->buf == p->buf_end) { TML_WARN("Unexpected end of file"); return -1; }
		c = *(p->buf++);
		if (c & 0x80) res = ((res | (c & 0x7F)) << 7);
		else return (int)(res | c);
	}
	TML_WARN("Invalid variable length byte count"); return -1;
}

static int tml_parsemessage(tml_message** f, struct tml_parser* p)
{
	int deltatime = tml_readvariablelength(p), status = tml_readbyte(p);
	tml_message* evt;

	// VDAC4: большая пауза (до 2^28 − 1 тиков) законна и больше не обнуляется: при
	// PPQN 32767 пауза 0x100000 — это 16 с, а не ноль. Оборванная длина — конец дорожки.
	if (deltatime < 0) { TML_WARN("Unexpected end of file"); return -1; }
	if (status < 0) { TML_WARN("Unexpected end of file"); return -1; }
	if ((status & 0x80) == 0)
	{
		// Invalid, use same status as before
		// VDAC4: running status — только у сообщений каналов (80..EF); мета-событие и
		// sysex его отменяют, и байт данных после них — испорченная дорожка, а не
		// «мета-событие без статуса».
		if (p->last_status < 0x80 || p->last_status > 0xEF) { TML_WARN("Undefined status and invalid running status"); return -1; }
		p->buf--;
		status = p->last_status;
	}
	else p->last_status = (status <= 0xEF ? status : 0); // VDAC4

	if (p->message_array_size == p->message_count)
	{
		//start allocated memory size of message array at 64, double each time until 8192, then add 1024 entries until done
		// VDAC4: новый блок — во временный указатель: при отказе старый не теряется, а
		// загрузка отменяется целиком (прежде список продолжал жить через NULL).
		int new_size = p->message_array_size + (!p->message_array_size ? 64 : (p->message_array_size > 4096 ? 1024 : p->message_array_size));
		tml_message* grown = (tml_message*)TML_REALLOC(*f, new_size * sizeof(tml_message));
		if (!grown) { TML_ERROR("Out of memory"); p->failed = 1; return -1; }
		*f = grown;
		p->message_array_size = new_size;
	}
	evt = *f + p->message_count;

	//check what message we have
	if ((status == TML_SYSEX) || (status == TML_EOX)) //sysex
	{
		//sysex messages are not handled
		// VDAC4: длина — сначала в переменную. В «p->buf += tml_readvariablelength(p)»
		// порядок вычисления не задан: MSVC брал p->buf до чтения длины, пропуск выходил
		// на байт короче (на F7), и после каждого sysex время сбивалось — у песни XG
		// (Robert Miles, Children) ноты уехали на 2:37.
		int length = tml_readvariablelength(p), kind = 0, a = 0, b = 0;
		unsigned char* data = p->buf;
		if (length < 0) { TML_WARN("Unexpected end of file"); return -1; }
		if (length > (int)(p->buf_end - p->buf)) { TML_WARN("Unexpected end of file"); p->buf = p->buf_end; return -1; }
		p->buf += length;
		evt->type = 0;
		// VDAC4: SysEx из короткого списка (tml_sysex_kind) — событием TML_SYSTEM_EXCLUSIVE.
		// F0 без F7 в конце продолжается пакетами F7; пакет F7 вне такого SysEx — «escape»:
		// узнаётся, если внутри целый SysEx (F0 ... F7).
		if (status == TML_SYSEX) p->sysex_len = 0, p->sysex_state = 1;
		else if (!p->sysex_state)
		{
			if (length > 0 && data[0] == 0xF0) data++, length--, p->sysex_len = 0, p->sysex_state = 1;
			else length = 0;
		}
		if (p->sysex_state)
		{
			if (p->sysex_len + length > (int)sizeof(p->sysex)) p->sysex_state = 2;
			else { TML_MEMCPY(p->sysex + p->sysex_len, data, length); p->sysex_len += length; }
			if (length > 0 && data[length - 1] == 0xF7)
			{
				if (p->sysex_state == 1) kind = tml_sysex_kind(p->sysex, p->sysex_len, &a, &b);
				p->sysex_state = 0;
			}
		}
		if (kind)
		{
			evt->type = TML_SYSTEM_EXCLUSIVE;
			evt->channel = (unsigned char)kind;
			if (kind == TML_SYSEX_MASTER_VOLUME || kind == TML_SYSEX_MASTER_FINE) evt->pitch_bend = (unsigned short)a;
			else evt->key = (char)a, evt->velocity = (char)b;
		}
	}
	else if (status == 0xFF) //meta events
	{
		int meta_type = tml_readbyte(p), buflen = tml_readvariablelength(p);
		unsigned char* metadata = p->buf;
		if (meta_type < 0 || buflen < 0) { TML_WARN("Unexpected end of file"); return -1; }
		if (buflen > (int)(p->buf_end - p->buf)) { TML_WARN("Unexpected end of file"); p->buf = p->buf_end; return -1; }
		p->buf += buflen;

		switch (meta_type)
		{
			case TML_EOT:
				if (buflen != 0) { TML_WARN("Invalid length for EndOfTrack event"); return -1; }
				if (!deltatime) return TML_EOT; //no need to store this message
				evt->type = TML_EOT;
				break;

			case TML_SET_TEMPO:
				if (buflen != 3) { TML_WARN("Invalid length for SetTempo meta event"); return -1; }
				// VDAC4: темп 0 остановил бы время песни — такое событие не берётся.
				if (!metadata[0] && !metadata[1] && !metadata[2]) { evt->type = 0; break; }
				evt->type = TML_SET_TEMPO;
				((struct tml_tempomsg*)evt)->Tempo[0] = metadata[0];
				((struct tml_tempomsg*)evt)->Tempo[1] = metadata[1];
				((struct tml_tempomsg*)evt)->Tempo[2] = metadata[2];
				break;

			default:
				evt->type = 0;
		}
	}
	else if (status > 0xEF) // VDAC4: системные сообщения (F1..FE) в файле SMF не бывают
	{
		TML_WARN("System message in a track"); return -1;
	}
	else //channel message
	{
		int param;
		if ((param = tml_readbyte(p)) < 0) { TML_WARN("Unexpected end of file"); return -1; }
		evt->key = (param & 0x7f);
		evt->channel = (status & 0x0f);
		switch (evt->type = (status & 0xf0))
		{
			case TML_NOTE_OFF:
			case TML_NOTE_ON:
			case TML_KEY_PRESSURE:
			case TML_CONTROL_CHANGE:
				if ((param = tml_readbyte(p)) < 0) { TML_WARN("Unexpected end of file"); return -1; }
				evt->velocity = (param & 0x7f);
				break;

			case TML_PITCH_BEND:
				if ((param = tml_readbyte(p)) < 0) { TML_WARN("Unexpected end of file"); return -1; }
				evt->pitch_bend = ((param & 0x7f) << 7) | evt->key;
				break;

			case TML_PROGRAM_CHANGE:
			case TML_CHANNEL_PRESSURE:
				evt->velocity = 0;
				break;

			default: //ignore system/manufacture messages
				evt->type = 0;
				break;
		}
	}

	if (deltatime || evt->type)
	{
		evt->time = deltatime;
		p->message_count++;
	}
	return evt->type;
}

// VDAC4: сколько байтов осталось в потоке из памяти (UINT_MAX — поток другой, не узнать):
// длина дорожки больше остатка файла — не выделять под неё память.
static unsigned int tml_stream_left(struct tml_stream* stream)
{
	if (stream->read == (int(*)(void*,void*,unsigned int))&tml_stream_memory_read)
	{
		struct tml_stream_memory* m = (struct tml_stream_memory*)stream->data;
		return m->total - m->pos;
	}
	return 0xFFFFFFFFu;
}

// VDAC4: пропустить count байтов (чужой блок, лишние байты заголовка). 0 — файл кончился.
static int tml_stream_skip(struct tml_stream* stream, unsigned int count)
{
	unsigned char scratch[256];
	if (count > tml_stream_left(stream)) return 0;
	while (count)
	{
		unsigned int part = (count > sizeof(scratch) ? (unsigned int)sizeof(scratch) : count);
		if (stream->read(stream->data, scratch, part) != (int)part) return 0;
		count -= part;
	}
	return 1;
}

// VDAC4: длина переменной длины для подсчёта событий (-1 — оборвана или длиннее 4 байтов).
static int tml_count_vlq(const unsigned char** p, const unsigned char* end)
{
	unsigned int res = 0, i;
	for (i = 0; i != 4 && *p != end; i++)
	{
		unsigned char c = *(*p)++;
		res = (res << 7) | (c & 0x7F);
		if (!(c & 0x80)) return (int)res;
	}
	return -1;
}

// VDAC4: событий в первых num_tracks дорожках файла из памяти — не меньше, чем станет
// сообщений (каждое событие даёт не больше одного): массив сообщений выделяется сразу
// целиком. Прежде он рос по 1024 записи через realloc, старый и новый блоки жили вместе,
// а освобождённые оставались дырами: у песни в 23 тысячи событий (Robert Miles, Children,
// одна дорожка в 92 КБ) пик памяти разбора был в несколько раз больше массива, и на плате
// PSRAM не хватало. Обход — как у tml_parsemessage: чужие блоки мимо, дорожка за концом
// файла — конец, испорченное событие или End of Track — конец дорожки.
static unsigned int tml_count_events(const unsigned char* d, unsigned int size, int num_tracks)
{
	const unsigned char* file_end = d + size;
	unsigned int count = 0;
	while (num_tracks > 0 && file_end - d >= 8)
	{
		unsigned int length = ((unsigned int)d[4] << 24) | ((unsigned int)d[5] << 16) | ((unsigned int)d[6] << 8) | d[7];
		const unsigned char *p = d + 8, *end;
		int status = 0, n;
		if (length > (unsigned int)(file_end - p)) break;
		end = p + length;
		if (d[0] != 'M' || d[1] != 'T' || d[2] != 'r' || d[3] != 'k') { d = end; continue; }
		d = end;
		num_tracks--;
		while (p != end)
		{
			if (tml_count_vlq(&p, end) < 0 || p == end) break;
			count++;
			if (*p == 0xFF)
			{
				int type;
				if (++p == end) break;
				type = *p++;
				if ((n = tml_count_vlq(&p, end)) < 0 || n > end - p) break;
				p += n, status = 0;
				if (type == TML_EOT) break;
			}
			else if (*p == 0xF0 || *p == 0xF7)
			{
				p++;
				if ((n = tml_count_vlq(&p, end)) < 0 || n > end - p) break;
				p += n, status = 0;
			}
			else
			{
				if (*p & 0x80) status = *p++;
				if (status < 0x80 || status > 0xEF) break;
				n = ((status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0) ? 1 : 2;
				if (n > end - p) break;
				p += n;
			}
		}
	}
	return count;
}

TMLDEF tml_message* tml_load(struct tml_stream* stream)
{
	int num_tracks, division, format;
	unsigned int trackbufsize = 0, header_length;
	unsigned char midi_header[14], *trackbuf = TML_NULL, *trackdata;
	struct tml_message* messages = TML_NULL;
	// VDAC4: файл в памяти — дорожки читаются на месте, без копии в trackbuf
	struct tml_stream_memory* memory = (tml_stream_left(stream) != 0xFFFFFFFFu ? (struct tml_stream_memory*)stream->data : TML_NULL);
	struct tml_track *tracks, *t, *tracksEnd;
	struct tml_parser p = { TML_NULL, TML_NULL, 0, 0, 0, 0, { 0 }, 0, 0 };

	// Parse MIDI header
	// VDAC4: поля заголовка — целиком (прежде — младшие байты длины и формата), формат 2
	// (независимые последовательности) не берётся: он игрался бы как формат 1, все
	// последовательности разом с общим темпом. Деление 0 — отказ (было «&&» вместо «||»,
	// и PPQN 0 проходил до деления на ноль). Заголовок длиннее 6 байтов — лишнее пропускается.
	if (stream->read(stream->data, midi_header, 14) != 14) { TML_ERROR("Unexpected end of file"); return messages; }
	header_length = ((unsigned int)midi_header[4] << 24) | ((unsigned int)midi_header[5] << 16) | ((unsigned int)midi_header[6] << 8) | midi_header[7];
	format = (midi_header[8] << 8) | midi_header[9];
	if (midi_header[0] != 'M' || midi_header[1] != 'T' || midi_header[2] != 'h' || midi_header[3] != 'd' ||
	    header_length < 6) { TML_ERROR("Doesn't look like a MIDI file: invalid MThd header"); return messages; }
	if (format > 1) { TML_ERROR("Unsupported MIDI format (only 0 and 1)"); return messages; }
	if (midi_header[12] & 0x80) { TML_ERROR("File uses unsupported SMPTE timing"); return messages; }
	num_tracks = (int)(midi_header[10] << 8) | midi_header[11];
	division = (int)(midi_header[12] << 8) | midi_header[13]; //division is ticks per beat (quarter-note)
	if (num_tracks <= 0 || division <= 0) { TML_ERROR("Doesn't look like a MIDI file: invalid track or division values"); return messages; }
	if (header_length > 6 && !tml_stream_skip(stream, header_length - 6)) { TML_ERROR("Unexpected end of file"); return messages; }

	// Allocate temporary tracks array for parsing
	tracks = (struct tml_track*)TML_MALLOC(sizeof(struct tml_track) * num_tracks);
	if (!tracks) { TML_ERROR("Out of memory"); return messages; } // VDAC4
	tracksEnd = &tracks[num_tracks];
	for (t = tracks; t != tracksEnd; t++) t->Idx = t->End = 0, t->Ticks = 0;

	// VDAC4: файл в памяти — массив сообщений сразу на все события (tml_count_events)
	if (memory)
	{
		unsigned int events = tml_count_events((const unsigned char*)memory->buffer + memory->pos, memory->total - memory->pos, num_tracks);
		if (events)
		{
			messages = (tml_message*)TML_MALLOC(events * sizeof(tml_message));
			if (!messages) { TML_ERROR("Out of memory"); TML_FREE(tracks); return TML_NULL; }
			p.message_array_size = (int)events;
		}
	}

	// Read all messages for all tracks
	for (t = tracks; t != tracksEnd;)
	{
		unsigned char track_header[8];
		unsigned int track_length;
		if (stream->read(stream->data, track_header, 8) != 8) { TML_WARN("Unexpected end of file"); break; }

		// Get size of track data and read into buffer (allocate bigger buffer if needed)
		track_length = (unsigned int)track_header[7] | ((unsigned int)track_header[6] << 8) | ((unsigned int)track_header[5] << 16) | ((unsigned int)track_header[4] << 24);
		if (track_header[0] != 'M' || track_header[1] != 'T' || track_header[2] != 'r' || track_header[3] != 'k')
		{
			// VDAC4: чужой блок SMF допускает — он пропускается, дорожки дальше читаются
			if (!tml_stream_skip(stream, track_length)) { TML_WARN("Invalid MTrk header"); break; }
			continue;
		}
		// VDAC4: длина больше остатка файла — испорчено: память под неё не выделяется
		if (track_length > 0x7FFFFFFFu || track_length > tml_stream_left(stream)) { TML_WARN("Invalid MTrk header"); break; }
		if (memory)
		{
			// VDAC4: дорожка — прямо в файле (парсер её только читает)
			trackdata = (unsigned char*)memory->buffer + memory->pos;
			memory->pos += track_length;
		}
		else
		{
			if (trackbufsize < track_length)
			{
				TML_FREE(trackbuf);
				trackbuf = (unsigned char*)TML_MALLOC(track_length);
				if (!trackbuf) { TML_ERROR("Out of memory"); trackbufsize = 0; p.failed = 1; break; } // VDAC4
				trackbufsize = track_length;
			}
			if (stream->read(stream->data, trackbuf, track_length) != (int)track_length) { TML_WARN("Unexpected end of file"); break; }
			trackdata = trackbuf;
		}

		t->Idx = p.message_count;
		p.last_status = 0; // VDAC4: running status не переходит из дорожки в дорожку
		p.sysex_state = 0; // VDAC4: и SysEx не продолжается
		for (p.buf_end = (p.buf = trackdata) + track_length; p.buf != p.buf_end;)
		{
			int type = tml_parsemessage(&messages, &p);
			if (type == TML_EOT || type < 0) break; //file end or illegal data encountered
			if (!(p.message_count & 4095)) TML_REST(); // VDAC4
		}
		if (p.buf != p.buf_end) { TML_WARN( "Track length did not match data length"); }
		t->End = p.message_count;
		t++;
		if (p.failed) break; // VDAC4
	}
	TML_FREE(trackbuf);

	// Change message time signature from delta ticks to actual msec values and link messages ordered by time
	// VDAC4: время — точно: Σ тиков × темп копится целым (мкс × деление) от последней смены
	// темпа, мс — делением в конце (прежде мс смены темпа округлялись, и ошибка копилась
	// с каждой сменой: 9,6 с вместо 10). Первое по времени событие больше не меняется
	// местами с messages[0] во время слияния — тот был ещё не прочитанным событием дорожки 0,
	// и дорожка терялась; голова списка переносится в messages[0] после слияния.
	if (p.message_count && !p.failed)
	{
		tml_message *head = TML_NULL, *PrevMessage = TML_NULL, *Msg, *MsgEnd;
		unsigned long long ticks = 0, tempo_ticks = 0; //tick counter and value at last tempo change
		unsigned long long tempo_units = 0; //microseconds * division at last tempo change
		unsigned long long tempo = 500000;  //microseconds per quarter note
		unsigned long long units = 0, msec = 0;
		// VDAC4: дорожки — в куче по (тик следующего события, номер дорожки): прежде каждый шаг
		// времени обходил все дорожки, и файл с тысячами дорожек разбирался минутами. Порядок
		// тот же: в одном тике — дорожки по номерам, события дорожки — по порядку.
		int *heap = (int*)TML_MALLOC(sizeof(int) * num_tracks), heapNum = 0, merged = 0, i;
		#define TML_TRACK_TICK(n) (tracks[n].Ticks + messages[tracks[n].Idx].time)
		#define TML_TRACK_LESS(x, y) (TML_TRACK_TICK(x) < TML_TRACK_TICK(y) || (TML_TRACK_TICK(x) == TML_TRACK_TICK(y) && (x) < (y)))
		if (!heap) { TML_ERROR("Out of memory"); p.failed = 1; }
		for (i = 0; heap && i < num_tracks; i++)
		{
			int at = heapNum++;
			if (tracks[i].Idx == tracks[i].End) { heapNum--; continue; }
			heap[at] = i;
			while (at > 0 && TML_TRACK_LESS(heap[at], heap[(at - 1) / 2])) { int s = heap[at]; heap[at] = heap[(at - 1) / 2]; heap[(at - 1) / 2] = s; at = (at - 1) / 2; }
		}
		while (heapNum && !p.failed)
		{
			int top = heap[0], at = 0;
			unsigned long long next = TML_TRACK_TICK(top);
			if (next != ticks || !merged)
			{
				ticks = next;
				units = tempo_units + (ticks - tempo_ticks) * tempo;
				msec = units / ((unsigned long long)division * 1000u);
			}
			t = &tracks[top];
			for (Msg = &messages[t->Idx], MsgEnd = &messages[t->End]; Msg != MsgEnd && t->Ticks + Msg->time == ticks; Msg++, t->Idx++)
			{
				t->Ticks += Msg->time;
				if (Msg->type && msec > TML_MAX_MSEC) { TML_ERROR("Song is too long"); p.failed = 1; break; }
				if (Msg->type == TML_SET_TEMPO)
				{
					unsigned char* Tempo = ((struct tml_tempomsg*)Msg)->Tempo;
					tempo = (unsigned long long)((Tempo[0]<<16)|(Tempo[1]<<8)|Tempo[2]);
					tempo_units = units;
					tempo_ticks = ticks;
				}
				if (Msg->type)
				{
					Msg->time = (unsigned int)msec;
					if (PrevMessage) PrevMessage->next = Msg;
					else head = Msg;
					PrevMessage = Msg;
				}
				if (!(++merged & 4095)) TML_REST(); // VDAC4
			}
			merged |= 1;
			// Вершина: дорожка с новым тиком или последняя из кучи.
			if (t->Idx == t->End) heap[0] = heap[--heapNum];
			while (1)
			{
				int l = 2 * at + 1, r = l + 1, m = at;
				if (l < heapNum && TML_TRACK_LESS(heap[l], heap[m])) m = l;
				if (r < heapNum && TML_TRACK_LESS(heap[r], heap[m])) m = r;
				if (m == at) break;
				{ int s = heap[at]; heap[at] = heap[m]; heap[m] = s; }
				at = m;
			}
		}
		#undef TML_TRACK_LESS
		#undef TML_TRACK_TICK
		TML_FREE(heap);
		if (PrevMessage) PrevMessage->next = TML_NULL;
		else p.message_count = 0;

		// Голова — в messages[0]: по этому указателю список и освобождается (tml_free).
		if (head && head != messages && !p.failed)
		{
			tml_message *pred = TML_NULL, *walk, Swap;
			for (walk = head; walk; walk = walk->next) if (walk->next == messages) { pred = walk; break; }
			Swap = *head; *head = *messages; *messages = Swap;
			if (pred == head) messages->next = head;
			else if (pred) pred->next = head;
		}
	}
	TML_FREE(tracks);

	if (p.message_count == 0 || p.failed)
	{
		TML_FREE(messages);
		messages = TML_NULL;
	}

	return messages;
}

TMLDEF tml_message* tml_load_tsf_stream(struct tsf_stream* stream)
{
	return tml_load((struct tml_stream*)stream);
}

TMLDEF int tml_get_info(tml_message* Msg, int* out_used_channels, int* out_used_programs, int* out_total_notes, unsigned int* out_time_first_note, unsigned int* out_time_length)
{
	int used_programs = 0, used_channels = 0, total_notes = 0;
	unsigned int time_first_note = 0xffffffff, time_length = 0;
	unsigned char channels[16] = { 0 }, programs[128] = { 0 };
	for (;Msg; Msg = Msg->next)
	{
		time_length = Msg->time;
		if (Msg->type == TML_PROGRAM_CHANGE && !programs[(int)Msg->program]) { programs[(int)Msg->program] = 1; used_programs++; }
		if (Msg->type != TML_NOTE_ON) continue;
		if (time_first_note == 0xffffffff) time_first_note = time_length;
		if (!channels[Msg->channel]) { channels[Msg->channel] = 1; used_channels++; }
		total_notes++;
	}
	if (time_first_note == 0xffffffff) time_first_note = 0;
	if (out_used_channels  ) *out_used_channels   = used_channels;
	if (out_used_programs  ) *out_used_programs   = used_programs;
	if (out_total_notes    ) *out_total_notes     = total_notes;
	if (out_time_first_note) *out_time_first_note = time_first_note;
	if (out_time_length    ) *out_time_length     = time_length;
	return total_notes;
}

TMLDEF int tml_get_tempo_value(tml_message* msg)
{
	unsigned char* Tempo;
	if (!msg || msg->type != TML_SET_TEMPO) return 0;
	Tempo = ((struct tml_tempomsg*)msg)->Tempo;
	return ((Tempo[0]<<16)|(Tempo[1]<<8)|Tempo[2]);
}

TMLDEF void tml_free(tml_message* f)
{
	TML_FREE(f);
}

#ifdef __cplusplus
}
#endif

#endif //TML_IMPLEMENTATION
