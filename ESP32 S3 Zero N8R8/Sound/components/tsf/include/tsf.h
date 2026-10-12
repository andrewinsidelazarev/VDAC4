/* TinySoundFont - v0.9 - SoundFont2 synthesizer - https://github.com/schellingb/TinySoundFont
   VDAC4: altered copy (see components/tsf/README.md): 16-bit samples kept in place,
   fixed-point voice position, cubic interpolation, channel effect sends (CC91/93/94),
   portamento (CC5/65/84), mod wheel vibrato (CC1), brightness (CC74), bank checks
   and the MIDI audit fixes of MIDI.ELF 1.6 and 1.7 (SF2 modulators, envelope ramps, voice
   stealing with fades, filter resonance gain, pedals, GS/XG NRPN). Lines changed are marked "VDAC4:".
                                     no warranty implied; use at your own risk
   Do this:
      #define TSF_IMPLEMENTATION
   before you include this file in *one* C or C++ file to create the implementation.
   // i.e. it should look like this:
   #include ...
   #include ...
   #define TSF_IMPLEMENTATION
   #include "tsf.h"

   [OPTIONAL] #define TSF_NO_STDIO to remove stdio dependency
   [OPTIONAL] #define TSF_MALLOC, TSF_REALLOC, and TSF_FREE to avoid stdlib.h
   [OPTIONAL] #define TSF_MEMCPY, TSF_MEMSET to avoid string.h
   [OPTIONAL] #define TSF_POW, TSF_POWF, TSF_EXPF, TSF_LOG, TSF_TAN, TSF_LOG10, TSF_SQRT to avoid math.h

   NOT YET IMPLEMENTED
     - Support for ChorusEffectsSend and ReverbEffectsSend generators
     - Better low-pass filter without lowering performance too much
     - Support for modulators

   LICENSE (MIT)

   Copyright (C) 2017-2025 Bernhard Schelling
   Based on SFZero, Copyright (C) 2012 Steve Folta (https://github.com/stevefolta/SFZero)

   Permission is hereby granted, free of charge, to any person obtaining a copy of this
   software and associated documentation files (the "Software"), to deal in the Software
   without restriction, including without limitation the rights to use, copy, modify, merge,
   publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons
   to whom the Software is furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in all
   copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
   INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
   PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
   LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
   USE OR OTHER DEALINGS IN THE SOFTWARE.

*/

#ifndef TSF_INCLUDE_TSF_INL
#define TSF_INCLUDE_TSF_INL

#ifdef __cplusplus
extern "C" {
#  define CPP_DEFAULT0 = 0
#else
#  define CPP_DEFAULT0
#endif

//define this if you want the API functions to be static
#ifdef TSF_STATIC
#define TSFDEF static
#else
#define TSFDEF extern
#endif

// The load functions will return a pointer to a struct tsf which all functions
// thereafter take as the first parameter.
// On error the tsf_load* functions will return NULL most likely due to invalid
// data (or if the file did not exist in tsf_load_filename).
typedef struct tsf tsf;

#ifndef TSF_NO_STDIO
// Directly load a SoundFont from a .sf2 file path
TSFDEF tsf* tsf_load_filename(const char* filename);
#endif

// Load a SoundFont from a block of memory
TSFDEF tsf* tsf_load_memory(const void* buffer, int size);

// Stream structure for the generic loading
struct tsf_stream
{
	// Custom data given to the functions as the first parameter
	void* data;

	// Function pointer will be called to read 'size' bytes into ptr (returns number of read bytes)
	int (*read)(void* data, void* ptr, unsigned int size);

	// Function pointer will be called to skip ahead over 'count' bytes (returns 1 on success, 0 on error)
	int (*skip)(void* data, unsigned int count);
};

// Generic SoundFont loading method using the stream structure above
TSFDEF tsf* tsf_load(struct tsf_stream* stream);

// Copy a tsf instance from an existing one, use tsf_close to close it as well.
// All copied tsf instances and their original instance are linked, and share the underlying soundfont.
// This allows loading a soundfont only once, but using it for multiple independent playbacks.
// (This function isn't thread-safe without locking.)
TSFDEF tsf* tsf_copy(tsf* f);

// Free the memory related to this tsf instance
TSFDEF void tsf_close(tsf* f);

// Stop all playing notes immediately and reset all channel parameters
TSFDEF void tsf_reset(tsf* f);

// Returns the preset index from a bank and preset number, or -1 if it does not exist in the loaded SoundFont
TSFDEF int tsf_get_presetindex(const tsf* f, int bank, int preset_number);

// Returns the number of presets in the loaded SoundFont
TSFDEF int tsf_get_presetcount(const tsf* f);

// Returns the name of a preset index >= 0 and < tsf_get_presetcount()
TSFDEF const char* tsf_get_presetname(const tsf* f, int preset_index);

// Returns the name of a preset by bank and preset number
TSFDEF const char* tsf_bank_get_presetname(const tsf* f, int bank, int preset_number);

// Supported output modes by the render methods
enum TSFOutputMode
{
	// Two channels with single left/right samples one after another
	TSF_STEREO_INTERLEAVED,
	// Two channels with all samples for the left channel first then right
	TSF_STEREO_UNWEAVED,
	// A single channel (stereo instruments are mixed into center)
	TSF_MONO
};

// Thread safety:
//
// 1. Rendering / voices:
//
// Your audio output which calls the tsf_render* functions will most likely
// run on a different thread than where the playback tsf_note* functions
// are called. In which case some sort of concurrency control like a
// mutex needs to be used so they are not called at the same time.
// Alternatively, you can pre-allocate a maximum number of voices that can
// play simultaneously by calling tsf_set_max_voices after loading.
// That way memory re-allocation will not happen during tsf_note_on and
// TSF should become mostly thread safe.
// There is a theoretical chance that ending notes would negatively influence
// a voice that is rendering at the time but it is hard to say.
// Also be aware, this has not been tested much.
//
// 2. Channels:
//
// Calls to tsf_channel_set_... functions may allocate new channels
// if no channel with that number was previously used. Make sure to
// create all channels at the beginning as required if you call tsf_render*
// from a different thread.

// Setup the parameters for the voice render methods
//   outputmode: if mono or stereo and how stereo channel data is ordered
//   samplerate: the number of samples per second (output frequency)
//   global_gain_db: volume gain in decibels (>0 means higher, <0 means lower)
TSFDEF void tsf_set_output(tsf* f, enum TSFOutputMode outputmode, int samplerate, float global_gain_db CPP_DEFAULT0);

// Set the global gain as a volume factor
//   global_gain: the desired volume where 1.0 is 100%
TSFDEF void tsf_set_volume(tsf* f, float global_gain);

// Set the maximum number of voices to play simultaneously
// Depending on the soundfond, one note can cause many new voices to be started,
// so don't keep this number too low or otherwise sounds may not play.
//   max_voices: maximum number to pre-allocate and set the limit to
//   (tsf_set_max_voices returns 0 if allocation failed, otherwise 1)
TSFDEF int tsf_set_max_voices(tsf* f, int max_voices);

// Start playing a note
//   preset_index: preset index >= 0 and < tsf_get_presetcount()
//   key: note value between 0 and 127 (60 being middle C)
//   vel: velocity as a float between 0.0 (equal to note off) and 1.0 (full)
//   bank: instrument bank number (alternative to preset_index)
//   preset_number: preset number (alternative to preset_index)
//   (tsf_note_on returns 0 if the allocation of a new voice failed, otherwise 1)
//   (tsf_bank_note_on returns 0 if preset does not exist or allocation failed, otherwise 1)
TSFDEF int tsf_note_on(tsf* f, int preset_index, int key, float vel);
TSFDEF int tsf_bank_note_on(tsf* f, int bank, int preset_number, int key, float vel);

// Stop playing a note
//   (bank_note_off returns 0 if preset does not exist, otherwise 1)
TSFDEF void tsf_note_off(tsf* f, int preset_index, int key);
TSFDEF int  tsf_bank_note_off(tsf* f, int bank, int preset_number, int key);

// Stop playing all notes (end with sustain and release)
TSFDEF void tsf_note_off_all(tsf* f);

// Returns the number of active voices
TSFDEF int tsf_active_voice_count(tsf* f);

// Render output samples into a buffer
// You can either render as signed 16-bit values (tsf_render_short) or
// as 32-bit float values (tsf_render_float)
//   buffer: target buffer of size samples * output_channels * sizeof(type)
//   samples: number of samples to render
//   flag_mixing: if 0 clear the buffer first, otherwise mix into existing data
TSFDEF void tsf_render_short(tsf* f, short* buffer, int samples, int flag_mixing CPP_DEFAULT0);
TSFDEF void tsf_render_float(tsf* f, float* buffer, int samples, int flag_mixing CPP_DEFAULT0);
// VDAC4: as tsf_render_float (replaces buffer), and adds the voices' mono effect sends
// to sends: reverb at [0..samples), chorus at [samples..2*samples), delay at
// [2*samples..3*samples) — scaled by the channel's CC91, CC93, CC94.
TSFDEF void tsf_render_float_sends(tsf* f, float* buffer, float* sends, int samples);
// VDAC4: то же для голосов [first, last) массива голосов: два ядра считают разные голоса
// одного куска одновременно (события тем временем не применяются, у каждого ядра свои
// buffer и sends). buffer заменяется, посылы прибавляются к sends (шаг samples).
TSFDEF void tsf_render_float_sends_range(tsf* f, float* buffer, float* sends, int samples, int first, int last);
// VDAC4: место в массиве голосов сразу за первыми count звучащими (не больше
// tsf_voice_slots) — граница голосов двух ядер.
TSFDEF int tsf_voice_split(tsf* f, int count);
// VDAC4: мест в массиве голосов (после tsf_set_max_voices — столько и есть).
TSFDEF int tsf_voice_slots(tsf* f);
// VDAC4: нот, которым не нашлось голоса: все заняты, а гаснущего (отпущенного) на
// смену нет — нота не звучит.
TSFDEF unsigned int tsf_voices_refused(tsf* f);
// VDAC4: предел звучащих голосов (0 — сколько мест): сверх него новой ноте отдаётся голос —
// отпущенный, под педалью или самый тихий из прошедших атаку (взятые только что не
// трогаются); tsf_voices_trim гасит быстро самые тихие голоса сверх предела и говорит, сколько.
TSFDEF void tsf_set_voice_cap(tsf* f, int cap);
TSFDEF int tsf_voices_trim(tsf* f);
// VDAC4: все голоса — сразу в тишину (перемотка): быстрое отпускание (tsf_reset) ещё
// 10 мс звучало бы прежним местом на новом и будило бы эффекты.
TSFDEF void tsf_voices_kill(tsf* f);
// VDAC4: звучащих голосов без гаснущих быстро (в счёт предела tsf_set_voice_cap).
TSFDEF int tsf_voices_sounding(tsf* f);

// Higher level channel based functions, set up channel parameters
//   channel: channel number
//   preset_index: preset index >= 0 and < tsf_get_presetcount()
//   preset_number: preset number (alternative to preset_index)
//   flag_mididrums: 0 for normal channels, otherwise apply MIDI drum channel rules
//   bank: instrument bank number (alternative to preset_index)
//   pan: stereo panning value from 0.0 (left) to 1.0 (right) (default 0.5 center)
//   volume: linear volume scale factor (default 1.0 full)
//   pitch_wheel: pitch wheel position 0 to 16383 (default 8192 unpitched)
//   pitch_range: range of the pitch wheel in semitones (default 2.0, total +/- 2 semitones)
//   tuning: tuning of all playing voices in semitones (default 0.0, standard (A440) tuning)
//   flag_sustain: 0 to end notes that were held sustained and disable holding sustain otherwise enable it
//   (tsf_set_preset_number and set_bank_preset return 0 if preset does not exist, otherwise 1)
//   (tsf_channel_set_... return 0 if a new channel needed allocation and that failed, otherwise 1)
TSFDEF int tsf_channel_set_presetindex(tsf* f, int channel, int preset_index);
TSFDEF int tsf_channel_set_presetnumber(tsf* f, int channel, int preset_number, int flag_mididrums CPP_DEFAULT0);
TSFDEF int tsf_channel_set_bank(tsf* f, int channel, int bank);
TSFDEF int tsf_channel_set_bank_preset(tsf* f, int channel, int bank, int preset_number);
TSFDEF int tsf_channel_set_pan(tsf* f, int channel, float pan);
TSFDEF int tsf_channel_set_volume(tsf* f, int channel, float volume);
TSFDEF int tsf_channel_set_pitchwheel(tsf* f, int channel, int pitch_wheel);
TSFDEF int tsf_channel_set_pitchrange(tsf* f, int channel, float pitch_range);
TSFDEF int tsf_channel_set_tuning(tsf* f, int channel, float tuning);
TSFDEF int tsf_channel_set_sustain(tsf* f, int channel, int flag_sustain);
// VDAC4: давление на канал и на клавишу (0..127) — источники модуляторов SF2; общая
// настройка канала в полутонах (SysEx: Master Fine/Coarse Tuning) — к колесу и RPN.
TSFDEF int tsf_channel_set_pressure(tsf* f, int channel, int pressure);
TSFDEF int tsf_channel_set_key_pressure(tsf* f, int channel, int key, int pressure);
TSFDEF int tsf_channel_set_master_tuning(tsf* f, int channel, float semitones);

// Start or stop playing notes on a channel (needs channel preset to be set)
//   channel: channel number
//   key: note value between 0 and 127 (60 being middle C)
//   vel: velocity as a float between 0.0 (equal to note off) and 1.0 (full)
//   (tsf_channel_note_on returns 0 on allocation failure of new voice, otherwise 1)
TSFDEF int tsf_channel_note_on(tsf* f, int channel, int key, float vel);
TSFDEF void tsf_channel_note_off(tsf* f, int channel, int key);
TSFDEF void tsf_channel_note_off_all(tsf* f, int channel); //end with sustain and release
TSFDEF void tsf_channel_sounds_off_all(tsf* f, int channel); //end immediately

// Apply a MIDI control change to the channel (not all controllers are supported!)
//    (tsf_channel_midi_control returns 0 on allocation failure of new channel, otherwise 1)
TSFDEF int tsf_channel_midi_control(tsf* f, int channel, int controller, int control_value);

// Get current values set on the channels
TSFDEF int tsf_channel_get_preset_index(tsf* f, int channel);
TSFDEF int tsf_channel_get_preset_bank(tsf* f, int channel);
TSFDEF int tsf_channel_get_preset_number(tsf* f, int channel);
TSFDEF float tsf_channel_get_pan(tsf* f, int channel);
TSFDEF float tsf_channel_get_volume(tsf* f, int channel);
TSFDEF int tsf_channel_get_pitchwheel(tsf* f, int channel);
TSFDEF float tsf_channel_get_pitchrange(tsf* f, int channel);
TSFDEF float tsf_channel_get_tuning(tsf* f, int channel);

#ifdef __cplusplus
#  undef CPP_DEFAULT0
}
#endif

// end header
// ---------------------------------------------------------------------------------------------------------
#endif //TSF_INCLUDE_TSF_INL

#ifdef TSF_IMPLEMENTATION
#undef TSF_IMPLEMENTATION

// The lower this block size is the more accurate the effects are.
// Increasing the value significantly lowers the CPU usage of the voice rendering.
// If LFO affects the low-pass filter it can be hearable even as low as 8.
#ifndef TSF_RENDER_EFFECTSAMPLEBLOCK
#define TSF_RENDER_EFFECTSAMPLEBLOCK 64
#endif

// When using tsf_render_short, to do the conversion a buffer of a fixed size is
// allocated on the stack. On low memory platforms this could be made smaller.
// Increasing this above 512 should not have a significant impact on performance.
// The value should be a multiple of TSF_RENDER_EFFECTSAMPLEBLOCK.
#ifndef TSF_RENDER_SHORTBUFFERBLOCK
#define TSF_RENDER_SHORTBUFFERBLOCK 512
#endif

// Grace release time for quick voice off (avoid clicking noise)
#define TSF_FASTRELEASETIME 0.01f

// VDAC4: уступить ядро другим задачам в долгой работе (загрузка банка); по умолчанию — ничего.
#ifndef TSF_REST
#define TSF_REST() ((void)0)
#endif

#if !defined(TSF_MALLOC) || !defined(TSF_FREE) || !defined(TSF_REALLOC)
#  include <stdlib.h>
#  define TSF_MALLOC  malloc
#  define TSF_FREE    free
#  define TSF_REALLOC realloc
#endif

#if !defined(TSF_MEMCPY) || !defined(TSF_MEMSET)
#  include <string.h>
#  define TSF_MEMCPY  memcpy
#  define TSF_MEMSET  memset
#endif
#ifndef TSF_MEMCMP
#  include <string.h>
#  define TSF_MEMCMP  memcmp // VDAC4: сравнение списков модуляторов
#endif

#if !defined(TSF_POW) || !defined(TSF_POWF) || !defined(TSF_EXPF) || !defined(TSF_LOG) || !defined(TSF_TAN) || !defined(TSF_LOG10) || !defined(TSF_SQRT)
#  include <math.h>
#  if !defined(__cplusplus) && !defined(NAN) && !defined(powf) && !defined(expf) && !defined(sqrtf)
#    define powf (float)pow // deal with old math.h
#    define expf (float)exp // files that come without
#    define sqrtf (float)sqrt // powf, expf and sqrtf
#  endif
#  define TSF_POW     pow
#  define TSF_POWF    powf
#  define TSF_EXPF    expf
#  define TSF_LOG     log
#  define TSF_TAN     tan
#  define TSF_LOG10   log10
#  define TSF_SQRTF   sqrtf
#endif
#ifndef TSF_TANF
#  define TSF_TANF    tanf // VDAC4: фильтр голоса в одинарной точности
#endif
#ifndef TSF_LOGF
#  define TSF_LOGF    logf // VDAC4: огибающая и громкость в одинарной точности
#  define TSF_LOG10F  log10f
#endif

#ifndef TSF_NO_STDIO
#  include <stdio.h>
#endif

#define TSF_TRUE 1
#define TSF_FALSE 0
#define TSF_BOOL unsigned char
#define TSF_PI 3.14159265358979323846264338327950288
#define TSF_NULL 0

#ifdef __cplusplus
extern "C" {
#endif

typedef char tsf_fourcc[4];
typedef signed char tsf_s8;
typedef unsigned char tsf_u8;
typedef unsigned short tsf_u16;
typedef signed short tsf_s16;
typedef unsigned int tsf_u32;
typedef char tsf_char20[20];

#define TSF_FourCCEquals(value1, value2) (value1[0] == value2[0] && value1[1] == value2[1] && value1[2] == value2[2] && value1[3] == value2[3])

struct tsf
{
	struct tsf_preset* presets;
	const tsf_s16* fontSamples; // VDAC4: 16-bit samples instead of float
	unsigned int fontSampleCount; // VDAC4: for interpolation bounds
	int fontSamplesOwned; // VDAC4: 0 when samples live in the caller's buffer (tsf_load_memory)
	struct tsf_voice* voices;
	struct tsf_channels* channels;

	int presetNum;
	int voiceNum;
	int maxVoiceNum;
	unsigned int voicePlayIndex;

	enum TSFOutputMode outputmode;
	float outSampleRate;
	float globalGainDB;
	int* refCount;
	float* sendBuffer; // VDAC4: effect sends of the current render (tsf_render_float_sends)
	int sendStride;    // VDAC4
	unsigned int voicesRefused; // VDAC4: нот без голоса (tsf_voices_refused)
	int voiceCap;               // VDAC4: предел звучащих голосов (tsf_set_voice_cap), 0 — все места
};

#ifndef TSF_NO_STDIO
static int tsf_stream_stdio_read(FILE* f, void* ptr, unsigned int size) { return (int)fread(ptr, 1, size, f); }
static int tsf_stream_stdio_skip(FILE* f, unsigned int count) { return !fseek(f, count, SEEK_CUR); }
TSFDEF tsf* tsf_load_filename(const char* filename)
{
	tsf* res;
	struct tsf_stream stream = { TSF_NULL, (int(*)(void*,void*,unsigned int))&tsf_stream_stdio_read, (int(*)(void*,unsigned int))&tsf_stream_stdio_skip };
	#if __STDC_WANT_SECURE_LIB__
	FILE* f = TSF_NULL; fopen_s(&f, filename, "rb");
	#else
	FILE* f = fopen(filename, "rb");
	#endif
	if (!f)
	{
		//if (e) *e = TSF_FILENOTFOUND;
		return TSF_NULL;
	}
	stream.data = f;
	res = tsf_load(&stream);
	fclose(f);
	return res;
}
#endif

struct tsf_stream_memory { const char* buffer; unsigned int total, pos; };
static int tsf_stream_memory_read(struct tsf_stream_memory* m, void* ptr, unsigned int size) { if (size > m->total - m->pos) size = m->total - m->pos; TSF_MEMCPY(ptr, m->buffer+m->pos, size); m->pos += size; return size; }
static int tsf_stream_memory_skip(struct tsf_stream_memory* m, unsigned int count) { if (m->pos + count > m->total) return 0; m->pos += count; return 1; }
TSFDEF tsf* tsf_load_memory(const void* buffer, int size)
{
	struct tsf_stream stream = { TSF_NULL, (int(*)(void*,void*,unsigned int))&tsf_stream_memory_read, (int(*)(void*,unsigned int))&tsf_stream_memory_skip };
	struct tsf_stream_memory f = { 0, 0, 0 };
	f.buffer = (const char*)buffer;
	f.total = size;
	stream.data = &f;
	return tsf_load(&stream);
}

enum { TSF_LOOPMODE_NONE, TSF_LOOPMODE_CONTINUOUS, TSF_LOOPMODE_SUSTAIN };

enum { TSF_SEGMENT_NONE, TSF_SEGMENT_DELAY, TSF_SEGMENT_ATTACK, TSF_SEGMENT_HOLD, TSF_SEGMENT_DECAY, TSF_SEGMENT_SUSTAIN, TSF_SEGMENT_RELEASE, TSF_SEGMENT_DONE };

struct tsf_hydra
{
	struct tsf_hydra_phdr *phdrs; struct tsf_hydra_pbag *pbags; struct tsf_hydra_pmod *pmods;
	struct tsf_hydra_pgen *pgens; struct tsf_hydra_inst *insts; struct tsf_hydra_ibag *ibags;
	struct tsf_hydra_imod *imods; struct tsf_hydra_igen *igens; struct tsf_hydra_shdr *shdrs;
	int phdrNum, pbagNum, pmodNum, pgenNum, instNum, ibagNum, imodNum, igenNum, shdrNum;
};

union tsf_hydra_genamount { struct { tsf_u8 lo, hi; } range; tsf_s16 shortAmount; tsf_u16 wordAmount; };
struct tsf_hydra_phdr { tsf_char20 presetName; tsf_u16 preset, bank, presetBagNdx; tsf_u32 library, genre, morphology; };
struct tsf_hydra_pbag { tsf_u16 genNdx, modNdx; };
struct tsf_hydra_pmod { tsf_u16 modSrcOper, modDestOper; tsf_s16 modAmount; tsf_u16 modAmtSrcOper, modTransOper; };
struct tsf_hydra_pgen { tsf_u16 genOper; union tsf_hydra_genamount genAmount; };
struct tsf_hydra_inst { tsf_char20 instName; tsf_u16 instBagNdx; };
struct tsf_hydra_ibag { tsf_u16 instGenNdx, instModNdx; };
struct tsf_hydra_imod { tsf_u16 modSrcOper, modDestOper; tsf_s16 modAmount; tsf_u16 modAmtSrcOper, modTransOper; };
struct tsf_hydra_igen { tsf_u16 genOper; union tsf_hydra_genamount genAmount; };
struct tsf_hydra_shdr { tsf_char20 sampleName; tsf_u32 start, end, startLoop, endLoop, sampleRate; tsf_u8 originalPitch; tsf_s8 pitchCorrection; tsf_u16 sampleLink, sampleType; };

// VDAC4: каждое чтение проверяется целиком — обрезанный pdta давал записи с мусором.
#define TSFR(FIELD) ok &= (stream->read(stream->data, &i->FIELD, sizeof(i->FIELD)) == (int)sizeof(i->FIELD));
static int tsf_hydra_read_phdr(struct tsf_hydra_phdr* i, struct tsf_stream* stream) { int ok = 1; TSFR(presetName) TSFR(preset) TSFR(bank) TSFR(presetBagNdx) TSFR(library) TSFR(genre) TSFR(morphology) return ok; }
static int tsf_hydra_read_pbag(struct tsf_hydra_pbag* i, struct tsf_stream* stream) { int ok = 1; TSFR(genNdx) TSFR(modNdx) return ok; }
static int tsf_hydra_read_pmod(struct tsf_hydra_pmod* i, struct tsf_stream* stream) { int ok = 1; TSFR(modSrcOper) TSFR(modDestOper) TSFR(modAmount) TSFR(modAmtSrcOper) TSFR(modTransOper) return ok; }
static int tsf_hydra_read_pgen(struct tsf_hydra_pgen* i, struct tsf_stream* stream) { int ok = 1; TSFR(genOper) TSFR(genAmount) return ok; }
static int tsf_hydra_read_inst(struct tsf_hydra_inst* i, struct tsf_stream* stream) { int ok = 1; TSFR(instName) TSFR(instBagNdx) return ok; }
static int tsf_hydra_read_ibag(struct tsf_hydra_ibag* i, struct tsf_stream* stream) { int ok = 1; TSFR(instGenNdx) TSFR(instModNdx) return ok; }
static int tsf_hydra_read_imod(struct tsf_hydra_imod* i, struct tsf_stream* stream) { int ok = 1; TSFR(modSrcOper) TSFR(modDestOper) TSFR(modAmount) TSFR(modAmtSrcOper) TSFR(modTransOper) return ok; }
static int tsf_hydra_read_igen(struct tsf_hydra_igen* i, struct tsf_stream* stream) { int ok = 1; TSFR(genOper) TSFR(genAmount) return ok; }
static int tsf_hydra_read_shdr(struct tsf_hydra_shdr* i, struct tsf_stream* stream) { int ok = 1; TSFR(sampleName) TSFR(start) TSFR(end) TSFR(startLoop) TSFR(endLoop) TSFR(sampleRate) TSFR(originalPitch) TSFR(pitchCorrection) TSFR(sampleLink) TSFR(sampleType) return ok; }
#undef TSFR

// VDAC4: связи phdr → pbag → pgen и inst → ibag → igen возрастают и не выходят за
// массивы, терминальные записи есть. Загрузчик ходит по ним без проверок (ppbag[1],
// pinst[1], pibag[1]): индекс за массивом читал чужую память.
static int tsf_hydra_valid(const struct tsf_hydra* h)
{
	int i;
	if (h->phdrNum < 2 || h->pbagNum < 1 || h->pgenNum < 1 || h->instNum < 2 || h->ibagNum < 1 || h->igenNum < 1 || h->shdrNum < 2) return 0;
	for (i = 0; i + 1 < h->phdrNum; i++) if (h->phdrs[i].presetBagNdx > h->phdrs[i + 1].presetBagNdx) return 0;
	if (h->phdrs[h->phdrNum - 1].presetBagNdx > h->pbagNum - 1) return 0;
	for (i = 0; i + 1 < h->pbagNum; i++) if (h->pbags[i].genNdx > h->pbags[i + 1].genNdx) return 0;
	if (h->pbags[h->pbagNum - 1].genNdx > h->pgenNum) return 0;
	for (i = 0; i + 1 < h->instNum; i++) if (h->insts[i].instBagNdx > h->insts[i + 1].instBagNdx) return 0;
	if (h->insts[h->instNum - 1].instBagNdx > h->ibagNum - 1) return 0;
	for (i = 0; i + 1 < h->ibagNum; i++) if (h->ibags[i].instGenNdx > h->ibags[i + 1].instGenNdx) return 0;
	if (h->ibags[h->ibagNum - 1].instGenNdx > h->igenNum) return 0;
	return 1;
}

struct tsf_riffchunk { tsf_fourcc id; tsf_u32 size; };
struct tsf_envelope { float delay, attack, hold, decay, sustain, release, keynumToHold, keynumToDecay; };
struct tsf_voice_envelope { unsigned char segment, segmentIsExponential : 1, isAmpEnv : 1; short midiVelocity; float level, slope; int samplesUntilNextSegment; struct tsf_envelope parameters; };
struct tsf_voice_lowpass { float QInv, a0, a1, b1, b2, z1, z2; TSF_BOOL active; }; // VDAC4: float — у ESP32-S3 double только программный
struct tsf_voice_lfo { int samplesUntil; float level, delta; };

// VDAC4: модулятор SF2 после слияния (по умолчанию, инструмент, пресет). Источник — 16 бит:
// индекс (7), CC (бит 7), от максимума к минимуму (бит 8), биполярный (бит 9), кривая
// (биты 10..15: 0 — линейная, 1 — вогнутая, 2 — выпуклая, 3 — переключатель).
struct tsf_modulator { tsf_u16 src, dst, amtSrc, trans; short amount; };

struct tsf_region
{
	int loop_mode;
	unsigned int sample_rate;
	unsigned char lokey, hikey, lovel, hivel;
	unsigned int group, offset, end, loop_start, loop_end;
	int transpose, tune, pitch_keycenter, pitch_keytrack;
	float attenuation, pan;
	struct tsf_envelope ampenv, modenv;
	int initialFilterQ, initialFilterFc;
	int modEnvToPitch, modEnvToFilterFc, modLfoToFilterFc, modLfoToVolume;
	float delayModLFO;
	int freqModLFO, modLfoToPitch;
	float delayVibLFO;
	int freqVibLFO, vibLfoToPitch;
	// VDAC4: постоянные клавиша и скорость зоны (генераторы 46, 47; −1 — нет), посылы
	// хоруса и реверберации (15, 16; 0,1 %), модуляторы зоны (в блоке пресета).
	int keynum, velocity;
	int chorusSend, reverbSend;
	struct tsf_modulator* mods;
	int modNum;
};

struct tsf_preset
{
	tsf_char20 presetName;
	tsf_u16 preset, bank;
	struct tsf_region* regions;
	int regionNum;
	struct tsf_modulator* mods; // VDAC4: модуляторы всех регионов пресета одним блоком
};

struct tsf_voice
{
	int playingPreset, playingKey, playingChannel, heldSustain;
	struct tsf_region* region;
	float pitchInputTimecents, pitchOutputFactor; // VDAC4: float (double у ESP32-S3 программный)
	unsigned int sourcePosInt, sourcePosFrac; // VDAC4: позиция 32.32 вместо double
	float  noteGainDB, panFactorLeft, panFactorRight;
	unsigned int playIndex, loopStart, loopEnd;
	struct tsf_voice_envelope ampenv, modenv;
	struct tsf_voice_lowpass lowpass;
	struct tsf_voice_lfo modlfo, viblfo;
	float portaCents, portaStep; // VDAC4: portamento — pitch offset gliding to 0, cents per sample
	unsigned char looped; // VDAC4: позиция уже возвращалась на начало петли
	// VDAC4: клавиша и скорость для высоты, огибающих и модуляторов (генераторы 46, 47
	// подменяют настоящие), давление на клавишу, педаль sostenuto (взята при нажатой
	// клавише / держит отпущенную), фильтр пересчитать (срез или резонанс сменились).
	unsigned char effKey, effVel, polyPressure, sostenutoLatched, heldSostenuto, lowpassDirty;
	// VDAC4: параметры региона с вкладом модуляторов (tsf_voice_update) — их читает синтез.
	float vibLfoToPitch, modLfoToPitch, modEnvToPitch, initialFilterFc, filterQ;
	float modLfoToFilterFc, modEnvToFilterFc, modLfoToVolume;
	float reverbSend, chorusSend, modTuneCents;
};

struct tsf_channel
{
	unsigned short presetIndex, bank, pitchWheel, midiPan, midiVolume, midiExpression, midiRPN, midiData : 14, sustain : 1;
	float panOffset, gainDB, pitchRange, tuning; // VDAC4: panOffset и gainDB — только вызовы API, CC7/10/11 — модуляторы
	float delaySend; // VDAC4: посыл эха (CC94, в SF2 его нет) — у канала
	short portaLastKey; unsigned char portamento, portaTime;
	// VDAC4: выбор банка — MSB (CC0) и LSB (CC32) порознь; настройка RPN — грубая (RPN 2,
	// полутоны) и точная (RPN 1, доли полутона) порознь, tuning — их сумма.
	unsigned char bankMSB, bankLSB;
	signed char tuningCoarse; float tuningFine;
	// VDAC4: значения контроллеров (источники модуляторов SF2), давление на канал, педаль
	// sostenuto, одноголосный режим (CC126), выбран NRPN (а не RPN) и его номер, глубина
	// вибрато колесом модуляции (RPN 5, центы), общая настройка (SysEx, полутоны).
	unsigned char cc[128];
	unsigned char channelPressure, sostenuto, mono, nrpnSelected;
	unsigned short midiNRPN;
	float modRangeCents, masterTuning;
};

struct tsf_channels
{
	void (*setupVoice)(tsf* f, struct tsf_voice* voice);
	int channelNum, activeChannel;
	struct tsf_channel channels[1];
};

static double tsf_timecents2Secsd(double timecents) { return TSF_POW(2.0, timecents / 1200.0); }
static float tsf_timecents2Secsf(float timecents) { return TSF_POWF(2.0f, timecents / 1200.0f); }
static float tsf_cents2Hertz(float cents) { return 8.176f * TSF_POWF(2.0f, cents / 1200.0f); }
static float tsf_decibelsToGain(float db) { return (db > -100.f ? TSF_POWF(10.0f, db * 0.05f) : 0); }
static float tsf_gainToDecibels(float gain) { return (gain <= .00001f ? -100.f : 20.0f * TSF_LOG10F(gain)); } // VDAC4: log10f

static TSF_BOOL tsf_riffchunk_read(struct tsf_riffchunk* parent, struct tsf_riffchunk* chunk, struct tsf_stream* stream)
{
	TSF_BOOL IsRiff, IsList;
	if (parent && sizeof(tsf_fourcc) + sizeof(tsf_u32) > parent->size) return TSF_FALSE;
	if (!stream->read(stream->data, &chunk->id, sizeof(tsf_fourcc)) || *chunk->id <= ' ' || *chunk->id >= 'z') return TSF_FALSE;
	if (!stream->read(stream->data, &chunk->size, sizeof(tsf_u32))) return TSF_FALSE;
	if (parent && sizeof(tsf_fourcc) + sizeof(tsf_u32) + chunk->size > parent->size) return TSF_FALSE;
	if (parent) parent->size -= sizeof(tsf_fourcc) + sizeof(tsf_u32) + chunk->size;
	IsRiff = TSF_FourCCEquals(chunk->id, "RIFF"), IsList = TSF_FourCCEquals(chunk->id, "LIST");
	if (IsRiff && parent) return TSF_FALSE; //not allowed
	if (!IsRiff && !IsList) return TSF_TRUE; //custom type without sub type
	if (!stream->read(stream->data, &chunk->id, sizeof(tsf_fourcc)) || *chunk->id <= ' ' || *chunk->id >= 'z') return TSF_FALSE;
	chunk->size -= sizeof(tsf_fourcc);
	return TSF_TRUE;
}

static void tsf_region_clear(struct tsf_region* i, TSF_BOOL for_relative)
{
	TSF_MEMSET(i, 0, sizeof(struct tsf_region));
	i->hikey = i->hivel = 127;
	i->pitch_keycenter = 60; // C4
	if (for_relative) return;

	i->pitch_keytrack = 100;
	i->keynum = i->velocity = -1; // VDAC4: постоянных клавиши и скорости нет

	i->pitch_keycenter = -1;

	// SF2 defaults in timecents.
	i->ampenv.delay = i->ampenv.attack = i->ampenv.hold = i->ampenv.decay = i->ampenv.release = -12000.0f;
	i->modenv.delay = i->modenv.attack = i->modenv.hold = i->modenv.decay = i->modenv.release = -12000.0f;

	i->initialFilterFc = 13500;

	i->delayModLFO = -12000.0f;
	i->delayVibLFO = -12000.0f;
}

static void tsf_region_operator(struct tsf_region* region, tsf_u16 genOper, union tsf_hydra_genamount* amount, struct tsf_region* merge_region)
{
	enum
	{
		_GEN_TYPE_MASK       = 0x0F,
		GEN_FLOAT            = 0x01,
		GEN_INT              = 0x02,
		GEN_UINT_ADD         = 0x03,
		GEN_UINT_ADD15       = 0x04,
		GEN_KEYRANGE         = 0x05,
		GEN_VELRANGE         = 0x06,
		GEN_LOOPMODE         = 0x07,
		GEN_GROUP            = 0x08,
		GEN_KEYCENTER        = 0x09,
		GEN_FIXEDKEY         = 0x0A, // VDAC4: Keynum (46) — только у инструмента, не складывается
		GEN_FIXEDVEL         = 0x0B, // VDAC4: Velocity (47)

		_GEN_LIMIT_MASK      = 0xF0,
		GEN_INT_LIMIT12K     = 0x10, //min -12000, max 12000
		GEN_INT_LIMITFC      = 0x20, //min 1500, max 13500
		GEN_INT_LIMITQ       = 0x30, //min 0, max 960
		GEN_INT_LIMIT960     = 0x40, //min -960, max 960
		GEN_INT_LIMIT16K4500 = 0x50, //min -16000, max 4500
		GEN_FLOAT_LIMIT12K5K = 0x60, //min -12000, max 5000
		GEN_FLOAT_LIMIT12K8K = 0x70, //min -12000, max 8000
		GEN_FLOAT_LIMIT1200  = 0x80, //min -1200, max 1200
		GEN_FLOAT_LIMITPAN   = 0x90, //* .001f, min -.5f, max .5f,
		GEN_FLOAT_LIMITATTN  = 0xA0, //* .04f, min 0, max 57.6 (VDAC4: шкала EMU10K1 и FluidSynth 1.x)
		GEN_FLOAT_MAX1000    = 0xB0, //min 0, max 1000
		GEN_FLOAT_MAX1440    = 0xC0, //min 0, max 1440
		GEN_INT_LIMIT1000    = 0xD0, //min 0, max 1000 (VDAC4: посылы хоруса и реверберации, 0,1 %)

		_GEN_MAX = 59
	};
	#define _TSFREGIONOFFSET(TYPE, FIELD) (unsigned char)(((TYPE*)&((struct tsf_region*)0)->FIELD) - (TYPE*)0)
	#define _TSFREGIONENVOFFSET(TYPE, ENV, FIELD) (unsigned char)(((TYPE*)&((&(((struct tsf_region*)0)->ENV))->FIELD)) - (TYPE*)0)
	static const struct { unsigned char mode, offset; } genMetas[_GEN_MAX] =
	{
		{ GEN_UINT_ADD                     , _TSFREGIONOFFSET(unsigned int, offset               ) }, // 0 StartAddrsOffset
		{ GEN_UINT_ADD                     , _TSFREGIONOFFSET(unsigned int, end                  ) }, // 1 EndAddrsOffset
		{ GEN_UINT_ADD                     , _TSFREGIONOFFSET(unsigned int, loop_start           ) }, // 2 StartloopAddrsOffset
		{ GEN_UINT_ADD                     , _TSFREGIONOFFSET(unsigned int, loop_end             ) }, // 3 EndloopAddrsOffset
		{ GEN_UINT_ADD15                   , _TSFREGIONOFFSET(unsigned int, offset               ) }, // 4 StartAddrsCoarseOffset
		{ GEN_INT   | GEN_INT_LIMIT12K     , _TSFREGIONOFFSET(         int, modLfoToPitch        ) }, // 5 ModLfoToPitch
		{ GEN_INT   | GEN_INT_LIMIT12K     , _TSFREGIONOFFSET(         int, vibLfoToPitch        ) }, // 6 VibLfoToPitch
		{ GEN_INT   | GEN_INT_LIMIT12K     , _TSFREGIONOFFSET(         int, modEnvToPitch        ) }, // 7 ModEnvToPitch
		{ GEN_INT   | GEN_INT_LIMITFC      , _TSFREGIONOFFSET(         int, initialFilterFc      ) }, // 8 InitialFilterFc
		{ GEN_INT   | GEN_INT_LIMITQ       , _TSFREGIONOFFSET(         int, initialFilterQ       ) }, // 9 InitialFilterQ
		{ GEN_INT   | GEN_INT_LIMIT12K     , _TSFREGIONOFFSET(         int, modLfoToFilterFc     ) }, //10 ModLfoToFilterFc
		{ GEN_INT   | GEN_INT_LIMIT12K     , _TSFREGIONOFFSET(         int, modEnvToFilterFc     ) }, //11 ModEnvToFilterFc
		{ GEN_UINT_ADD15                   , _TSFREGIONOFFSET(unsigned int, end                  ) }, //12 EndAddrsCoarseOffset
		{ GEN_INT   | GEN_INT_LIMIT960     , _TSFREGIONOFFSET(         int, modLfoToVolume       ) }, //13 ModLfoToVolume
		{ 0                                , (0                                                  ) }, //   Unused
		{ GEN_INT   | GEN_INT_LIMIT1000    , _TSFREGIONOFFSET(         int, chorusSend           ) }, //15 ChorusEffectsSend (VDAC4)
		{ GEN_INT   | GEN_INT_LIMIT1000    , _TSFREGIONOFFSET(         int, reverbSend           ) }, //16 ReverbEffectsSend (VDAC4)
		{ GEN_FLOAT | GEN_FLOAT_LIMITPAN   , _TSFREGIONOFFSET(       float, pan                  ) }, //17 Pan
		{ 0                                , (0                                                  ) }, //   Unused
		{ 0                                , (0                                                  ) }, //   Unused
		{ 0                                , (0                                                  ) }, //   Unused
		{ GEN_FLOAT | GEN_FLOAT_LIMIT12K5K , _TSFREGIONOFFSET(       float, delayModLFO          ) }, //21 DelayModLFO
		{ GEN_INT   | GEN_INT_LIMIT16K4500 , _TSFREGIONOFFSET(         int, freqModLFO           ) }, //22 FreqModLFO
		{ GEN_FLOAT | GEN_FLOAT_LIMIT12K5K , _TSFREGIONOFFSET(       float, delayVibLFO          ) }, //23 DelayVibLFO
		{ GEN_INT   | GEN_INT_LIMIT16K4500 , _TSFREGIONOFFSET(         int, freqVibLFO           ) }, //24 FreqVibLFO
		{ GEN_FLOAT | GEN_FLOAT_LIMIT12K5K , _TSFREGIONENVOFFSET(    float, modenv, delay        ) }, //25 DelayModEnv
		{ GEN_FLOAT | GEN_FLOAT_LIMIT12K8K , _TSFREGIONENVOFFSET(    float, modenv, attack       ) }, //26 AttackModEnv
		{ GEN_FLOAT | GEN_FLOAT_LIMIT12K5K , _TSFREGIONENVOFFSET(    float, modenv, hold         ) }, //27 HoldModEnv
		{ GEN_FLOAT | GEN_FLOAT_LIMIT12K8K , _TSFREGIONENVOFFSET(    float, modenv, decay        ) }, //28 DecayModEnv
		{ GEN_FLOAT | GEN_FLOAT_MAX1000    , _TSFREGIONENVOFFSET(    float, modenv, sustain      ) }, //29 SustainModEnv
		{ GEN_FLOAT | GEN_FLOAT_LIMIT12K8K , _TSFREGIONENVOFFSET(    float, modenv, release      ) }, //30 ReleaseModEnv
		{ GEN_FLOAT | GEN_FLOAT_LIMIT1200  , _TSFREGIONENVOFFSET(    float, modenv, keynumToHold ) }, //31 KeynumToModEnvHold
		{ GEN_FLOAT | GEN_FLOAT_LIMIT1200  , _TSFREGIONENVOFFSET(    float, modenv, keynumToDecay) }, //32 KeynumToModEnvDecay
		{ GEN_FLOAT | GEN_FLOAT_LIMIT12K5K , _TSFREGIONENVOFFSET(    float, ampenv, delay        ) }, //33 DelayVolEnv
		{ GEN_FLOAT | GEN_FLOAT_LIMIT12K8K , _TSFREGIONENVOFFSET(    float, ampenv, attack       ) }, //34 AttackVolEnv
		{ GEN_FLOAT | GEN_FLOAT_LIMIT12K5K , _TSFREGIONENVOFFSET(    float, ampenv, hold         ) }, //35 HoldVolEnv
		{ GEN_FLOAT | GEN_FLOAT_LIMIT12K8K , _TSFREGIONENVOFFSET(    float, ampenv, decay        ) }, //36 DecayVolEnv
		{ GEN_FLOAT | GEN_FLOAT_MAX1440    , _TSFREGIONENVOFFSET(    float, ampenv, sustain      ) }, //37 SustainVolEnv
		{ GEN_FLOAT | GEN_FLOAT_LIMIT12K8K , _TSFREGIONENVOFFSET(    float, ampenv, release      ) }, //38 ReleaseVolEnv
		{ GEN_FLOAT | GEN_FLOAT_LIMIT1200  , _TSFREGIONENVOFFSET(    float, ampenv, keynumToHold ) }, //39 KeynumToVolEnvHold
		{ GEN_FLOAT | GEN_FLOAT_LIMIT1200  , _TSFREGIONENVOFFSET(    float, ampenv, keynumToDecay) }, //40 KeynumToVolEnvDecay
		{ 0                                , (0                                                  ) }, //   Instrument (special)
		{ 0                                , (0                                                  ) }, //   Reserved
		{ GEN_KEYRANGE                     , (0                                                  ) }, //43 KeyRange
		{ GEN_VELRANGE                     , (0                                                  ) }, //44 VelRange
		{ GEN_UINT_ADD15                   , _TSFREGIONOFFSET(unsigned int, loop_start           ) }, //45 StartloopAddrsCoarseOffset
		{ GEN_FIXEDKEY                     , (0                                                  ) }, //46 Keynum (VDAC4)
		{ GEN_FIXEDVEL                     , (0                                                  ) }, //47 Velocity (VDAC4)
		{ GEN_FLOAT | GEN_FLOAT_LIMITATTN  , _TSFREGIONOFFSET(       float, attenuation          ) }, //48 InitialAttenuation
		{ 0                                , (0                                                  ) }, //   Reserved
		{ GEN_UINT_ADD15                   , _TSFREGIONOFFSET(unsigned int, loop_end             ) }, //50 EndloopAddrsCoarseOffset
		{ GEN_INT                          , _TSFREGIONOFFSET(         int, transpose            ) }, //51 CoarseTune
		{ GEN_INT                          , _TSFREGIONOFFSET(         int, tune                 ) }, //52 FineTune
		{ 0                                , (0                                                  ) }, //   SampleID (special)
		{ GEN_LOOPMODE                     , _TSFREGIONOFFSET(         int, loop_mode            ) }, //54 SampleModes
		{ 0                                , (0                                                  ) }, //   Reserved
		{ GEN_INT                          , _TSFREGIONOFFSET(         int, pitch_keytrack       ) }, //56 ScaleTuning
		{ GEN_GROUP                        , _TSFREGIONOFFSET(unsigned int, group                ) }, //57 ExclusiveClass
		{ GEN_KEYCENTER                    , _TSFREGIONOFFSET(         int, pitch_keycenter      ) }, //58 OverridingRootKey
	};
	#undef _TSFREGIONOFFSET
	#undef _TSFREGIONENVOFFSET
	if (amount)
	{
		int offset;
		if (genOper >= _GEN_MAX) return;
		offset = genMetas[genOper].offset;
		switch (genMetas[genOper].mode & _GEN_TYPE_MASK)
		{
			case GEN_FLOAT:      ((       float*)region)[offset]  = amount->shortAmount;     return;
			case GEN_INT:        ((         int*)region)[offset]  = amount->shortAmount;     return;
			case GEN_UINT_ADD:   ((unsigned int*)region)[offset] += amount->shortAmount;     return;
			case GEN_UINT_ADD15: ((unsigned int*)region)[offset] += amount->shortAmount<<15; return;
			case GEN_KEYRANGE:   region->lokey = amount->range.lo; region->hikey = amount->range.hi; return;
			case GEN_VELRANGE:   region->lovel = amount->range.lo; region->hivel = amount->range.hi; return;
			case GEN_LOOPMODE:   region->loop_mode       = ((amount->wordAmount&3) == 3 ? TSF_LOOPMODE_SUSTAIN : ((amount->wordAmount&3) == 1 ? TSF_LOOPMODE_CONTINUOUS : TSF_LOOPMODE_NONE)); return;
			case GEN_GROUP:      region->group           = amount->wordAmount;  return;
			case GEN_KEYCENTER:  region->pitch_keycenter = amount->shortAmount; return;
			case GEN_FIXEDKEY:   region->keynum          = amount->shortAmount; return; // VDAC4
			case GEN_FIXEDVEL:   region->velocity        = amount->shortAmount; return; // VDAC4
		}
	}
	else //merge regions and clamp values
	{
		for (genOper = 0; genOper != _GEN_MAX; genOper++)
		{
			int offset = genMetas[genOper].offset;
			switch (genMetas[genOper].mode & _GEN_TYPE_MASK)
			{
				case GEN_FLOAT:
				{
					float *val = &((float*)region)[offset], vfactor, vmin, vmax;
					*val += ((float*)merge_region)[offset];
					switch (genMetas[genOper].mode & _GEN_LIMIT_MASK)
					{
						case GEN_FLOAT_LIMIT12K5K: vfactor =   1.0f; vmin = -12000.0f; vmax = 5000.0f; break;
						case GEN_FLOAT_LIMIT12K8K: vfactor =   1.0f; vmin = -12000.0f; vmax = 8000.0f; break;
						case GEN_FLOAT_LIMIT1200:  vfactor =   1.0f; vmin =  -1200.0f; vmax = 1200.0f; break;
						case GEN_FLOAT_LIMITPAN:   vfactor = 0.001f; vmin =     -0.5f; vmax =    0.5f; break;
						// VDAC4: затухание SF2 — в сантибелах (0,1 дБ), но банки той поры (TimGM6mb
						// тоже) сведены под EMU10K1 и FluidSynth 1.x, где генератор берётся с
						// множителем 0,4 (ALT_ATTENUATION_SCALE): 0,04 дБ на единицу; FluidSynth 2.x
						// берёт его по спецификации. Прежде было 0,01 — вчетверо слабее; по
						// спецификации (0,1) зоны TimGM6mb стихли бы на 3–20 дБ, с 600 — замолчали.
						// Модуляторы (скорость, CC7, CC11) — по спецификации, без множителя.
						case GEN_FLOAT_LIMITATTN:  vfactor =  0.04f; vmin =      0.0f; vmax =   57.6f; break;
						case GEN_FLOAT_MAX1000:    vfactor =   1.0f; vmin =      0.0f; vmax = 1000.0f; break;
						case GEN_FLOAT_MAX1440:    vfactor =   1.0f; vmin =      0.0f; vmax = 1440.0f; break;
						default: continue;
					}
					*val *= vfactor;
					if      (*val < vmin) *val = vmin;
					else if (*val > vmax) *val = vmax;
					continue;
				}
				case GEN_INT:
				{
					int *val = &((int*)region)[offset], vmin, vmax;
					*val += ((int*)merge_region)[offset];
					switch (genMetas[genOper].mode & _GEN_LIMIT_MASK)
					{
						case GEN_INT_LIMIT12K:     vmin = -12000; vmax = 12000; break;
						case GEN_INT_LIMITFC:      vmin =   1500; vmax = 13500; break;
						case GEN_INT_LIMITQ:       vmin =      0; vmax =   960; break;
						case GEN_INT_LIMIT960:     vmin =   -960; vmax =   960; break;
						case GEN_INT_LIMIT16K4500: vmin = -16000; vmax =  4500; break;
						case GEN_INT_LIMIT1000:    vmin =      0; vmax =  1000; break; // VDAC4
						default: continue;
					}
					if      (*val < vmin) *val = vmin;
					else if (*val > vmax) *val = vmax;
					continue;
				}
				case GEN_UINT_ADD:
				{
					((unsigned int*)region)[offset] += ((unsigned int*)merge_region)[offset];
					continue;
				}
			}
		}
	}
}

static void tsf_region_envtosecs(struct tsf_envelope* p, TSF_BOOL sustainIsGain)
{
	// EG times need to be converted from timecents to seconds.
	// Pin very short EG segments.  Timecents don't get to zero, and our EG is
	// happier with zero values.
	p->delay   = (p->delay   < -11950.0f ? 0.0f : tsf_timecents2Secsf(p->delay));
	p->attack  = (p->attack  < -11950.0f ? 0.0f : tsf_timecents2Secsf(p->attack));
	p->release = (p->release < -11950.0f ? 0.0f : tsf_timecents2Secsf(p->release));

	// If we have dynamic hold or decay times depending on key number we need
	// to keep the values in timecents so we can calculate it during startNote
	if (!p->keynumToHold)  p->hold  = (p->hold  < -11950.0f ? 0.0f : tsf_timecents2Secsf(p->hold));
	if (!p->keynumToDecay) p->decay = (p->decay < -11950.0f ? 0.0f : tsf_timecents2Secsf(p->decay));

	if (p->sustain < 0.0f) p->sustain = 0.0f;
	else if (sustainIsGain) p->sustain = tsf_decibelsToGain(-p->sustain / 10.0f);
	else p->sustain = 1.0f - (p->sustain / 1000.0f);
}

// VDAC4: модуляторы SF2 — приёмники, которые синтезатор умеет (индекс итога в голосе).
enum { TSF_MD_ATTEN, TSF_MD_PAN, TSF_MD_VIB, TSF_MD_MODLFO_PITCH, TSF_MD_MODENV_PITCH, TSF_MD_FC, TSF_MD_Q,
	TSF_MD_MODLFO_FC, TSF_MD_MODENV_FC, TSF_MD_MODLFO_VOL, TSF_MD_CHORUS, TSF_MD_REVERB, TSF_MD_COARSE, TSF_MD_FINE,
	TSF_MD_ATTACK, TSF_MD_DECAY, TSF_MD_RELEASE, TSF_MD_COUNT };

static int tsf_mod_dest(tsf_u16 dst)
{
	switch (dst)
	{
		case 48: return TSF_MD_ATTEN;        case 17: return TSF_MD_PAN;          case  6: return TSF_MD_VIB;
		case  5: return TSF_MD_MODLFO_PITCH; case  7: return TSF_MD_MODENV_PITCH; case  8: return TSF_MD_FC;
		case  9: return TSF_MD_Q;            case 10: return TSF_MD_MODLFO_FC;    case 11: return TSF_MD_MODENV_FC;
		case 13: return TSF_MD_MODLFO_VOL;   case 15: return TSF_MD_CHORUS;       case 16: return TSF_MD_REVERB;
		case 51: return TSF_MD_COARSE;       case 52: return TSF_MD_FINE;         case 34: return TSF_MD_ATTACK;
		case 36: return TSF_MD_DECAY;        case 38: return TSF_MD_RELEASE;
		default: return -1;
	}
}

// VDAC4: источник модулятора, который синтезатор умеет: контроллеры (кроме 0, 6, 32..63,
// 98..101, 120..127 — их SF2 запрещает), «нет контроллера», скорость, клавиша, давление на
// клавишу и на канал, колесо высоты и его диапазон; кривые 0..3. Связанные — нет.
static int tsf_mod_source_valid(tsf_u16 src)
{
	int index = src & 0x7F;
	if ((src >> 10) > 3) return 0;
	if (src & 0x80) return !(index == 0 || index == 6 || (index >= 32 && index <= 63) || (index >= 98 && index <= 101) || index >= 120);
	return index == 0 || index == 2 || index == 3 || index == 10 || index == 13 || index == 14 || index == 16;
}

// VDAC4: модуляторы по умолчанию — девять из SF2 2.04 (без «скорость → срез», как у
// FluidSynth и SpessaSynth, и без колеса высоты: его считает канал) и шесть звуковых
// контроллеров GM2, как у SpessaSynth: CC73 — атака, CC72 — отпускание, CC75 — спад
// огибающей громкости, CC74 — срез, CC71 — резонанс, CC67 (левая педаль) — срез на октаву ниже.
// CC91 и CC93 → посылы — 1000, а не 200 из SF2 (MIDI.ELF 1.7.3): эффекты MIDI.ELF настроены на
// посыл CC/127 (так было в 1.6), а с 200 посылы 1.7..1.7.2 были впятеро (−14 дБ) тише —
// звук без эффектов. Модуляторы банка с 1000 (у TimGM6mb такие есть) дают то же; посылы зон
// (у TimGM6mb — 7 % реверберации почти везде) — по шкале SF2, как у FluidSynth: песня с
// CC91 0 остаётся почти сухой (общий множитель ×5 сделал бы из 7 % 35 %).
// Таблица кончается записью-терминатором, и цикл по ней идёт до неё: цикл «до конца
// массива» компилятор делает сравнением с адресом за массивом, а если таблица лежит
// последней в .rodata модуля, этот адрес — конец секции, и загрузчик ELF прошивок
// до 0.1.40 переводит его в 0 (цикл бежал по памяти до ошибки кэша, MIDI 1.7..1.7.1).
static const struct tsf_modulator tsf_default_mods[] =
{
	{ 0x0502, 48, 0, 0,   960 }, // скорость → затухание (вогнутая, от максимума): громкость ~ скорость²
	{ 0x0081,  6, 0, 0,    50 }, // CC1 → глубина вибрато (RPN 5 её меняет)
	{ 0x0587, 48, 0, 0,   960 }, // CC7 → затухание
	{ 0x000D,  6, 0, 0,    50 }, // давление на канал → глубина вибрато
	{ 0x028A, 17, 0, 0,   500 }, // CC10 → панорама
	{ 0x058B, 48, 0, 0,   960 }, // CC11 → затухание
	{ 0x00DB, 16, 0, 0,  1000 }, // CC91 → посыл реверберации (SF2: 200 — см. выше)
	{ 0x00DD, 15, 0, 0,  1000 }, // CC93 → посыл хоруса (SF2: 200)
	{ 0x0AC9, 34, 0, 0,  6000 }, // CC73 → атака (выпуклая, биполярная)
	{ 0x02C8, 38, 0, 0,  3600 }, // CC72 → отпускание
	{ 0x02CB, 36, 0, 0,  3600 }, // CC75 → спад
	{ 0x02CA,  8, 0, 0,  9600 }, // CC74 → срез
	{ 0x02C7,  9, 0, 0,   250 }, // CC71 → резонанс
	{ 0x0CC3,  8, 0, 0, -1200 }, // CC67 → срез (переключатель)
	{ 0, 0, 0, 0, 0 },           // терминатор (приёмник 0 у модуляторов по умолчанию не бывает)
};
#define TSF_MODS_MAX 32 // модуляторов у региона не больше (лишние не берутся)

// VDAC4: модулятор в список: такой же (источник, приёмник, источник величины) заменяется
// (add == 0: зона инструмента поверх умолчаний, своя поверх глобальной) или складывается
// с ним (add: пресет к инструменту), иначе добавляется.
static int tsf_mods_add(struct tsf_modulator* list, int count, const struct tsf_modulator* m, int add)
{
	int i;
	for (i = 0; i < count; i++)
		if (list[i].src == m->src && list[i].dst == m->dst && list[i].amtSrc == m->amtSrc)
		{
			if (add)
			{
				int sum = list[i].amount + m->amount;
				list[i].amount = (short)(sum > 32767 ? 32767 : (sum < -32768 ? -32768 : sum));
			}
			else list[i] = *m;
			return count;
		}
	if (count < TSF_MODS_MAX) list[count++] = *m;
	return count;
}

// VDAC4: записи pmod/imod [begin, end) в список (неумелые — мимо; границы — в пределах массива).
static int tsf_mods_take(struct tsf_modulator* list, int count, const struct tsf_hydra_imod* raw, int num, int begin, int end, int add)
{
	if (end > num) end = num;
	for (; begin < end; begin++)
	{
		struct tsf_modulator m;
		m.src = raw[begin].modSrcOper, m.dst = raw[begin].modDestOper, m.amtSrc = raw[begin].modAmtSrcOper;
		m.trans = raw[begin].modTransOper, m.amount = raw[begin].modAmount;
		if (tsf_mod_dest(m.dst) < 0 || !tsf_mod_source_valid(m.src) || !tsf_mod_source_valid(m.amtSrc) || (m.trans != 0 && m.trans != 2)) continue;
		count = tsf_mods_add(list, count, &m, add);
	}
	return count;
}

// VDAC4: модуляторы региона по SF2 2.04 (9.5): умолчания, поверх — глобальной и своей зоны
// инструмента; модуляторы пресета (своя зона поверх глобальной) складываются с ними.
static int tsf_region_mods(struct tsf_hydra* h, const int* ig, const int* il, const int* pg, const int* pl, struct tsf_modulator* out)
{
	struct tsf_modulator list[TSF_MODS_MAX], preset[TSF_MODS_MAX];
	const struct tsf_modulator* d;
	int count = 0, pcount = 0, i;
	for (d = tsf_default_mods; d->dst; d++) count = tsf_mods_add(list, count, d, 0);
	count = tsf_mods_take(list, count, h->imods, h->imodNum, ig[0], ig[1], 0);
	count = tsf_mods_take(list, count, h->imods, h->imodNum, il[0], il[1], 0);
	pcount = tsf_mods_take(preset, pcount, (const struct tsf_hydra_imod*)h->pmods, h->pmodNum, pg[0], pg[1], 0);
	pcount = tsf_mods_take(preset, pcount, (const struct tsf_hydra_imod*)h->pmods, h->pmodNum, pl[0], pl[1], 0);
	for (i = 0; i < pcount; i++) count = tsf_mods_add(list, count, &preset[i], 1);
	if (out) TSF_MEMCPY(out, list, count * sizeof(struct tsf_modulator));
	return count;
}

// VDAC4: зоны одного пресета — одним и тем же кодом для счёта (out == TSF_NULL) и для
// заполнения: прежде счёт брал диапазоны клавиш и скоростей только своих зон, без
// глобальных, и регионов насчитывалось больше, чем заполнялось, — хвост массива оставался
// неинициализированным, и нота могла взять такой регион. Пишется не больше capacity.
// Ссылки на инструмент и сэмпл — только на настоящие записи (терминальная — нет); зона с
// сэмплом, отсеянная диапазонами пресета, не становится глобальной зоной инструмента;
// сэмплы ПЗУ (тип 0x8000) и сжатые (SF3, 0x10) не играются. Модуляторы регионов — в mods
// (не больше modCapacity), всего их — в *modCount; список, такой же, как у прошлого
// записанного (зоны одного инструмента), — общий: у TimGM6mb 29040 модуляторов на 2063
// региона были почти одни умолчания (290 КБ PSRAM).
static int tsf_preset_regions(struct tsf_hydra *hydra, struct tsf_hydra_phdr* pphdr, unsigned int fontSampleCount, struct tsf_region* out, int capacity,
	struct tsf_modulator* mods, int modCapacity, int* modCount)
{
	enum { GenInstrument = 41, GenSampleID = 53 };
	struct tsf_hydra_pbag *ppbag, *ppbagEnd;
	struct tsf_region globalRegion;
	int count = 0, pg[2] = { 0, 0 }, prevNum = -1;
	struct tsf_modulator prevList[TSF_MODS_MAX], *prevMods = TSF_NULL; // VDAC4: прошлый записанный список
	*modCount = 0;
	tsf_region_clear(&globalRegion, TSF_TRUE);

	// Zones.
	for (ppbag = hydra->pbags + pphdr->presetBagNdx, ppbagEnd = hydra->pbags + pphdr[1].presetBagNdx; ppbag != ppbagEnd; ppbag++)
	{
		struct tsf_hydra_pgen *ppgen, *ppgenEnd; struct tsf_hydra_inst *pinst; struct tsf_hydra_ibag *pibag, *pibagEnd; struct tsf_hydra_igen *pigen, *pigenEnd;
		struct tsf_region presetRegion = globalRegion;
		int hadGenInstrument = 0, pl[2];
		pl[0] = ppbag->modNdx, pl[1] = ppbag[1].modNdx;

		// Generators.
		for (ppgen = hydra->pgens + ppbag->genNdx, ppgenEnd = hydra->pgens + ppbag[1].genNdx; ppgen != ppgenEnd; ppgen++)
		{
			// Instrument.
			if (ppgen->genOper == GenInstrument)
			{
				struct tsf_region instRegion;
				tsf_u16 whichInst = ppgen->genAmount.wordAmount;
				int ig[2] = { 0, 0 };
				hadGenInstrument = 1;
				if (whichInst >= hydra->instNum - 1) continue; // VDAC4: терминальная запись — не инструмент

				tsf_region_clear(&instRegion, TSF_FALSE);
				pinst = &hydra->insts[whichInst];
				for (pibag = hydra->ibags + pinst->instBagNdx, pibagEnd = hydra->ibags + pinst[1].instBagNdx; pibag != pibagEnd; pibag++)
				{
					// Generators.
					struct tsf_region zoneRegion = instRegion;
					int hadSampleID = 0, il[2];
					il[0] = pibag->instModNdx, il[1] = pibag[1].instModNdx;
					for (pigen = hydra->igens + pibag->instGenNdx, pigenEnd = hydra->igens + pibag[1].instGenNdx; pigen != pigenEnd; pigen++)
					{
						if (pigen->genOper == GenSampleID)
						{
							struct tsf_hydra_shdr* pshdr;
							hadSampleID = 1;

							if (pigen->genAmount.wordAmount >= hydra->shdrNum - 1) continue; // VDAC4
							pshdr = &hydra->shdrs[pigen->genAmount.wordAmount];
							if (pshdr->sampleType & 0x8010) continue; // VDAC4: ПЗУ или сжатый — отсчётов нет

							//preset region key and vel ranges are a filter for the zone regions
							if (zoneRegion.hikey < presetRegion.lokey || zoneRegion.lokey > presetRegion.hikey) continue;
							if (zoneRegion.hivel < presetRegion.lovel || zoneRegion.lovel > presetRegion.hivel) continue;
							if (presetRegion.lokey > zoneRegion.lokey) zoneRegion.lokey = presetRegion.lokey;
							if (presetRegion.hikey < zoneRegion.hikey) zoneRegion.hikey = presetRegion.hikey;
							if (presetRegion.lovel > zoneRegion.lovel) zoneRegion.lovel = presetRegion.lovel;
							if (presetRegion.hivel < zoneRegion.hivel) zoneRegion.hivel = presetRegion.hivel;

							if (out && count < capacity)
							{
								struct tsf_region region = zoneRegion;

								//sum regions
								tsf_region_operator(&region, 0, TSF_NULL, &presetRegion);

								// EG times need to be converted from timecents to seconds.
								tsf_region_envtosecs(&region.ampenv, TSF_TRUE);
								tsf_region_envtosecs(&region.modenv, TSF_FALSE);

								// LFO times need to be converted from timecents to seconds.
								region.delayModLFO = (region.delayModLFO < -11950.0f ? 0.0f : tsf_timecents2Secsf(region.delayModLFO));
								region.delayVibLFO = (region.delayVibLFO < -11950.0f ? 0.0f : tsf_timecents2Secsf(region.delayVibLFO));

								// Fixup sample positions
								region.offset += pshdr->start;
								region.end += pshdr->end;
								region.loop_start += pshdr->startLoop;
								region.loop_end += pshdr->endLoop;
								if (pshdr->endLoop > 0) region.loop_end -= 1;
								// VDAC4: конец петли — номер отсчёта внутри данных (интерполяция читает
								// input[loopEnd]); прежде мог быть равен их длине — отсчёт за массивом.
								if (region.loop_end >= fontSampleCount) region.loop_end = (fontSampleCount ? fontSampleCount - 1 : 0);
								// VDAC4: корневая клавиша 255 — сэмпл без высоты (SF2), 128..254 — неверна:
								// такие — как 60, а не «нота 255» с шагом во много октав вниз.
								if (region.pitch_keycenter < 0 || region.pitch_keycenter > 127) region.pitch_keycenter = (pshdr->originalPitch <= 127 ? pshdr->originalPitch : 60);
								region.tune += pshdr->pitchCorrection;
								region.sample_rate = (pshdr->sampleRate ? pshdr->sampleRate : 44100); // VDAC4: частота 0 — голос стоял бы на месте
								if (region.end && region.end < fontSampleCount) region.end++;
								else region.end = fontSampleCount;

								// VDAC4: модуляторы региона — в блок пресета; такой же список, как у
								// прошлого записанного, — общий.
								{
									struct tsf_modulator list[TSF_MODS_MAX];
									int n = tsf_region_mods(hydra, ig, il, pg, pl, list);
									if (prevMods && n == prevNum && !TSF_MEMCMP(list, prevMods, n * sizeof(struct tsf_modulator))) region.mods = prevMods;
									else if (*modCount + n <= modCapacity)
									{
										region.mods = mods + *modCount;
										TSF_MEMCPY(region.mods, list, n * sizeof(struct tsf_modulator));
										*modCount += n;
										prevMods = region.mods, prevNum = n;
									}
									else region.mods = TSF_NULL, n = 0;
									region.modNum = n;
								}

								out[count] = region;
							}
							else if (!out)
							{
								// VDAC4: счёт — с тем же правилом общих списков.
								struct tsf_modulator list[TSF_MODS_MAX];
								int n = tsf_region_mods(hydra, ig, il, pg, pl, list);
								if (n != prevNum || TSF_MEMCMP(list, prevList, n * sizeof(struct tsf_modulator)))
								{
									TSF_MEMCPY(prevList, list, n * sizeof(struct tsf_modulator));
									prevNum = n;
									*modCount += n;
								}
							}
							count++;
						}
						else tsf_region_operator(&zoneRegion, pigen->genOper, &pigen->genAmount, TSF_NULL);
					}

					// Handle instrument's global zone.
					if (pibag == hydra->ibags + pinst->instBagNdx && !hadSampleID)
						instRegion = zoneRegion, ig[0] = il[0], ig[1] = il[1];
				}
			}
			else tsf_region_operator(&presetRegion, ppgen->genOper, &ppgen->genAmount, TSF_NULL);
		}

		// Handle preset's global zone.
		if (ppbag == hydra->pbags + pphdr->presetBagNdx && !hadGenInstrument)
			globalRegion = presetRegion, pg[0] = pl[0], pg[1] = pl[1];
	}
	return count;
}

static int tsf_load_presets(tsf* res, struct tsf_hydra *hydra, unsigned int fontSampleCount)
{
	// Read each preset.
	struct tsf_hydra_phdr *pphdr, *pphdrMax;
	res->presetNum = hydra->phdrNum - 1;
	res->presets = (struct tsf_preset*)TSF_MALLOC(res->presetNum * sizeof(struct tsf_preset));
	if (!res->presets) return 0;
	else { int i; for (i = 0; i != res->presetNum; i++) res->presets[i].regions = TSF_NULL, res->presets[i].mods = TSF_NULL; }
	for (pphdr = hydra->phdrs, pphdrMax = pphdr + hydra->phdrNum - 1; pphdr != pphdrMax; pphdr++)
	{
		int sortedIndex = 0, regionNum, modNum;
		struct tsf_hydra_phdr *otherphdr;
		struct tsf_preset* preset;
		TSF_REST(); // VDAC4: большой банк — не держать ядро
		for (otherphdr = hydra->phdrs; otherphdr != pphdrMax; otherphdr++)
		{
			if (otherphdr == pphdr || otherphdr->bank > pphdr->bank) continue;
			else if (otherphdr->bank < pphdr->bank) sortedIndex++;
			else if (otherphdr->preset > pphdr->preset) continue;
			else if (otherphdr->preset < pphdr->preset) sortedIndex++;
			else if (otherphdr < pphdr) sortedIndex++;
		}

		preset = &res->presets[sortedIndex];
		TSF_MEMCPY(preset->presetName, pphdr->presetName, sizeof(preset->presetName));
		preset->presetName[sizeof(preset->presetName)-1] = '\0'; //should be zero terminated in source file but make sure
		preset->bank = pphdr->bank;
		preset->preset = pphdr->preset;
		preset->regionNum = 0;

		//count regions covered by this preset
		regionNum = tsf_preset_regions(hydra, pphdr, fontSampleCount, TSF_NULL, 0, TSF_NULL, 0, &modNum);
		if (!regionNum) continue; // VDAC4: пресет без регионов — не нехватка памяти

		preset->regions = (struct tsf_region*)TSF_MALLOC(regionNum * sizeof(struct tsf_region));
		preset->mods = (struct tsf_modulator*)TSF_MALLOC((modNum + TSF_MODS_MAX) * sizeof(struct tsf_modulator));
		if (!preset->regions || !preset->mods)
		{
			int i; for (i = 0; i != res->presetNum; i++) TSF_FREE(res->presets[i].regions), TSF_FREE(res->presets[i].mods);
			TSF_FREE(res->presets);
			res->presets = TSF_NULL;
			return 0;
		}
		regionNum = tsf_preset_regions(hydra, pphdr, fontSampleCount, preset->regions, regionNum, preset->mods, modNum + TSF_MODS_MAX, &modNum);
		preset->regionNum = regionNum; // заполнено столько же, сколько насчитано
	}
	return 1;
}

#ifdef STB_VORBIS_INCLUDE_STB_VORBIS_H
static int tsf_decode_ogg(const tsf_u8 *pSmpl, const tsf_u8 *pSmplEnd, float** pRes, tsf_u32* pResNum, tsf_u32* pResMax, tsf_u32 resInitial)
{
	float *res = *pRes, *oldres; tsf_u32 resNum = *pResNum; tsf_u32 resMax = *pResMax; stb_vorbis *v;

	// Use whatever stb_vorbis API that is available (either pull or push)
	#if !defined(STB_VORBIS_NO_PULLDATA_API) && !defined(STB_VORBIS_NO_FROMMEMORY)
	v = stb_vorbis_open_memory(pSmpl, (int)(pSmplEnd - pSmpl), TSF_NULL, TSF_NULL);
	#else
	{ int use, err; v = stb_vorbis_open_pushdata(pSmpl, (int)(pSmplEnd - pSmpl), &use, &err, TSF_NULL); pSmpl += use; }
	#endif
	if (v == TSF_NULL) return 0;

	for (;;)
	{
		float** outputs; int n_samples;

		// Decode one frame of vorbis samples with whatever stb_vorbis API that is available
		#if !defined(STB_VORBIS_NO_PULLDATA_API) && !defined(STB_VORBIS_NO_FROMMEMORY)
		n_samples = stb_vorbis_get_frame_float(v, TSF_NULL, &outputs);
		if (!n_samples) break;
		#else
		if (pSmpl >= pSmplEnd) break;
		{ int use = stb_vorbis_decode_frame_pushdata(v, pSmpl, (int)(pSmplEnd - pSmpl), TSF_NULL, &outputs, &n_samples); pSmpl += use; }
		if (!n_samples) continue;
		#endif

		// Expand our output buffer if necessary then copy over the decoded frame samples
		resNum += n_samples;
		if (resNum > resMax)
		{
			do { resMax += (resMax ? (resMax < 1048576 ? resMax : 1048576) : resInitial); } while (resNum > resMax);
			oldres = res;
			res = (float*)TSF_REALLOC(res, resMax * sizeof(float));
			if (!res) { TSF_FREE(oldres); stb_vorbis_close(v); return 0; }
		}
		TSF_MEMCPY(res + resNum - n_samples, outputs[0], n_samples * sizeof(float));
	}
	stb_vorbis_close(v);
	*pRes = res; *pResNum = resNum; *pResMax = resMax;
	return 1;
}

static int tsf_decode_sf3_samples(const void* rawBuffer, float** pFloatBuffer, unsigned int* pSmplCount, struct tsf_hydra *hydra)
{
	const tsf_u8* smplBuffer = (const tsf_u8*)rawBuffer;
	tsf_u32 smplLength = *pSmplCount, resNum = 0, resMax = 0, resInitial = (smplLength > 0x100000 ? (smplLength & ~0xFFFFF) : 65536);
	float *res = TSF_NULL, *oldres;
	int i, shdrLast = hydra->shdrNum - 1, is_sf3 = 0;
	for (i = 0; i <= shdrLast; i++)
	{
		struct tsf_hydra_shdr *shdr = &hydra->shdrs[i];
		if (shdr->sampleType & 0x30) // compression flags (sometimes Vorbis flag)
		{
			const tsf_u8 *pSmpl = smplBuffer + shdr->start, *pSmplEnd = smplBuffer + shdr->end;
			if (pSmpl + 4 > pSmplEnd || !TSF_FourCCEquals(pSmpl, "OggS"))
			{
				shdr->start = shdr->end = shdr->startLoop = shdr->endLoop = 0;
				continue;
			}

			// Fix up sample indices in shdr (end index is set after decoding)
			shdr->start = resNum;
			shdr->startLoop += resNum;
			shdr->endLoop += resNum;
			if (!tsf_decode_ogg(pSmpl, pSmplEnd, &res, &resNum, &resMax, resInitial)) { TSF_FREE(res); return 0; }
			shdr->end = resNum;
			is_sf3 = 1;
		}
		else // raw PCM sample
		{
			float *out; short *in = (short*)smplBuffer + resNum, *inEnd; tsf_u32 oldResNum = resNum;
			if (is_sf3) // Fix up sample indices in shdr
			{
				tsf_u32 fix_offset = resNum - shdr->start;
				in -= fix_offset;
				shdr->start = resNum;
				shdr->end += fix_offset;
				shdr->startLoop += fix_offset;
				shdr->endLoop += fix_offset;
			}
			inEnd = in + ((shdr->end >= shdr->endLoop ? shdr->end : shdr->endLoop) - resNum);
			if (i == shdrLast || (tsf_u8*)inEnd > (smplBuffer + smplLength)) inEnd = (short*)(smplBuffer + smplLength);
			if (inEnd <= in) continue;

			// expand our output buffer if necessary then convert the PCM data from short to float
			resNum += (tsf_u32)(inEnd - in);
			if (resNum > resMax)
			{
				do { resMax += (resMax ? (resMax < 1048576 ? resMax : 1048576) : resInitial); } while (resNum > resMax);
				oldres = res;
				res = (float*)TSF_REALLOC(res, resMax * sizeof(float));
				if (!res) { TSF_FREE(oldres); return 0; }
			}

			// Convert the samples from short to float
			for (out = res + oldResNum; in < inEnd;)
				*(out++) = (float)(*(in++) / 32767.0);
		}
	}

	// Trim the sample buffer down then return success (unless out of memory)
	if (!(*pFloatBuffer = (float*)TSF_REALLOC(res, resNum * sizeof(float)))) *pFloatBuffer = res;
	*pSmplCount = resNum;
	return (res ? 1 : 0);
}
#endif

// VDAC4: samples stay 16-bit. From a memory stream they are not copied at all:
// *pInPlace is set and *pShortBuffer points into the caller's buffer.
static int tsf_stream_memory_read(struct tsf_stream_memory* m, void* ptr, unsigned int size);
static int tsf_load_samples_s16(tsf_s16** pShortBuffer, int* pInPlace, unsigned int* pSmplCount, struct tsf_riffchunk *chunkSmpl, struct tsf_stream* stream)
{
	*pSmplCount = chunkSmpl->size / (unsigned int)sizeof(short);
	*pInPlace = 0;
	if (stream->read == (int(*)(void*,void*,unsigned int))&tsf_stream_memory_read)
	{
		struct tsf_stream_memory* m = (struct tsf_stream_memory*)stream->data;
		const char* start = m->buffer + m->pos;
		if (!((size_t)start & 1) && chunkSmpl->size <= m->total - m->pos)
		{
			*pShortBuffer = (tsf_s16*)start;
			*pInPlace = 1;
			return stream->skip(stream->data, chunkSmpl->size);
		}
	}
	*pShortBuffer = (tsf_s16*)TSF_MALLOC(chunkSmpl->size);
	return (*pShortBuffer && stream->read(stream->data, *pShortBuffer, chunkSmpl->size) == (int)chunkSmpl->size); // VDAC4: целиком
}

static int tsf_load_samples(void** pRawBuffer, float** pFloatBuffer, unsigned int* pSmplCount, struct tsf_riffchunk *chunkSmpl, struct tsf_stream* stream)
{
	#ifdef STB_VORBIS_INCLUDE_STB_VORBIS_H
	// With OGG Vorbis support we cannot pre-allocate the memory for tsf_decode_sf3_samples
	tsf_u32 resNum, resMax; float* oldres;
	*pSmplCount = chunkSmpl->size;
	*pRawBuffer = (void*)TSF_MALLOC(*pSmplCount);
	if (!*pRawBuffer || !stream->read(stream->data, *pRawBuffer, chunkSmpl->size)) return 0;
	if (chunkSmpl->id[3] != 'o') return 1;

	// Decode custom .sfo 'smpo' format where all samples are in a single ogg stream
	resNum = resMax = 0;
	if (!tsf_decode_ogg((tsf_u8*)*pRawBuffer, (tsf_u8*)*pRawBuffer + chunkSmpl->size, pFloatBuffer, &resNum, &resMax, 65536)) return 0;
	oldres = *pFloatBuffer;
	if (!(*pFloatBuffer = (float*)TSF_REALLOC(*pFloatBuffer, resNum * sizeof(float)))) *pFloatBuffer = oldres;
	*pSmplCount = resNum;
	return (*pFloatBuffer ? 1 : 0);
	#else
	// Inline convert the samples from short to float
	float *res, *out; const short *in;
	(void)pRawBuffer;
	*pSmplCount = chunkSmpl->size / (unsigned int)sizeof(short);
	*pFloatBuffer = (float*)TSF_MALLOC(*pSmplCount * sizeof(float));
	if (!*pFloatBuffer || !stream->read(stream->data, *pFloatBuffer, chunkSmpl->size)) return 0;
	for (res = *pFloatBuffer, out = res + *pSmplCount, in = (short*)res + *pSmplCount; out != res;)
		*(--out) = (float)(*(--in) / 32767.0);
	return 1;
	#endif
}

static int tsf_voice_envelope_release_samples(struct tsf_voice_envelope* e, float outSampleRate)
{
	return (int)((e->parameters.release <= 0 ? TSF_FASTRELEASETIME : e->parameters.release) * outSampleRate);
}

static void tsf_voice_envelope_nextsegment(struct tsf_voice_envelope* e, short active_segment, float outSampleRate)
{
	switch (active_segment)
	{
		case TSF_SEGMENT_NONE:
			e->samplesUntilNextSegment = (int)(e->parameters.delay * outSampleRate);
			if (e->samplesUntilNextSegment > 0)
			{
				e->segment = TSF_SEGMENT_DELAY;
				e->segmentIsExponential = TSF_FALSE;
				e->level = 0.0;
				e->slope = 0.0;
				return;
			}
			/* fall through */
		case TSF_SEGMENT_DELAY:
			e->samplesUntilNextSegment = (int)(e->parameters.attack * outSampleRate);
			if (e->samplesUntilNextSegment > 0)
			{
				if (!e->isAmpEnv)
				{
					//mod env attack duration scales with velocity (velocity of 1 is full duration, max velocity is 0.125 times duration)
					e->samplesUntilNextSegment = (int)(e->parameters.attack * ((145 - e->midiVelocity) / 144.0f) * outSampleRate);
				}
				e->segment = TSF_SEGMENT_ATTACK;
				e->segmentIsExponential = TSF_FALSE;
				e->level = 0.0f;
				e->slope = 1.0f / e->samplesUntilNextSegment;
				return;
			}
			/* fall through */
		case TSF_SEGMENT_ATTACK:
			e->samplesUntilNextSegment = (int)(e->parameters.hold * outSampleRate);
			if (e->samplesUntilNextSegment > 0)
			{
				e->segment = TSF_SEGMENT_HOLD;
				e->segmentIsExponential = TSF_FALSE;
				e->level = 1.0f;
				e->slope = 0.0f;
				return;
			}
			/* fall through */
		case TSF_SEGMENT_HOLD:
			e->samplesUntilNextSegment = (int)(e->parameters.decay * outSampleRate);
			if (e->samplesUntilNextSegment > 0)
			{
				e->segment = TSF_SEGMENT_DECAY;
				e->level = 1.0f;
				if (e->isAmpEnv)
				{
					// I don't truly understand this; just following what LinuxSampler does.
					float mysterySlope = -9.226f / e->samplesUntilNextSegment;
					e->slope = TSF_EXPF(mysterySlope);
					e->segmentIsExponential = TSF_TRUE;
					if (e->parameters.sustain > 0.0f)
					{
						// Again, this is following LinuxSampler's example, which is similar to
						// SF2-style decay, where "decay" specifies the time it would take to
						// get to zero, not to the sustain level.  The SFZ spec is not that
						// specific about what "decay" means, so perhaps it's really supposed
						// to specify the time to reach the sustain level.
						e->samplesUntilNextSegment = (int)(TSF_LOGF(e->parameters.sustain) / mysterySlope); // VDAC4: logf
					}
				}
				else
				{
					e->slope = -1.0f / e->samplesUntilNextSegment;
					e->samplesUntilNextSegment = (int)(e->parameters.decay * (1.0f - e->parameters.sustain) * outSampleRate);
					e->segmentIsExponential = TSF_FALSE;
				}
				return;
			}
			/* fall through */
		case TSF_SEGMENT_DECAY:
			e->segment = TSF_SEGMENT_SUSTAIN;
			e->level = e->parameters.sustain;
			e->slope = 0.0f;
			e->samplesUntilNextSegment = 0x7FFFFFFF;
			e->segmentIsExponential = TSF_FALSE;
			return;
		case TSF_SEGMENT_SUSTAIN:
			e->segment = TSF_SEGMENT_RELEASE;
			e->samplesUntilNextSegment = tsf_voice_envelope_release_samples(e, outSampleRate);
			if (e->isAmpEnv)
			{
				// I don't truly understand this; just following what LinuxSampler does.
				float mysterySlope = -9.226f / e->samplesUntilNextSegment;
				e->slope = TSF_EXPF(mysterySlope);
				e->segmentIsExponential = TSF_TRUE;
			}
			else
			{
				e->slope = -e->level / e->samplesUntilNextSegment;
				e->segmentIsExponential = TSF_FALSE;
			}
			return;
		case TSF_SEGMENT_RELEASE:
		default:
			e->segment = TSF_SEGMENT_DONE;
			e->segmentIsExponential = TSF_FALSE;
			e->level = e->slope = 0.0f;
			e->samplesUntilNextSegment = 0x7FFFFFF;
	}
}

static void tsf_voice_envelope_setup(struct tsf_voice_envelope* e, struct tsf_envelope* new_parameters, int midiNoteNumber, short midiVelocity, TSF_BOOL isAmpEnv, float outSampleRate)
{
	e->parameters = *new_parameters;
	// VDAC4: порог «мгновенно» — −11950 тайм-центов, как у остальных времён (было −10000).
	if (e->parameters.keynumToHold)
	{
		e->parameters.hold += e->parameters.keynumToHold * (60.0f - midiNoteNumber);
		e->parameters.hold = (e->parameters.hold < -11950.0f ? 0.0f : tsf_timecents2Secsf(e->parameters.hold));
	}
	if (e->parameters.keynumToDecay)
	{
		e->parameters.decay += e->parameters.keynumToDecay * (60.0f - midiNoteNumber);
		e->parameters.decay = (e->parameters.decay < -11950.0f ? 0.0f : tsf_timecents2Secsf(e->parameters.decay));
	}
	e->midiVelocity = midiVelocity;
	e->isAmpEnv = isAmpEnv;
	tsf_voice_envelope_nextsegment(e, TSF_SEGMENT_NONE, outSampleRate);
}

// VDAC4: остаток куска после смены стадии идёт уже в следующую стадию (и дальше, если и та
// короче): прежде он терялся, и короткие атака, удержание и спад растягивались до куска.
static void tsf_voice_envelope_process(struct tsf_voice_envelope* e, int numSamples, float outSampleRate)
{
	int guard = 8; // стадий за кусок не больше, чем их есть
	while (numSamples > 0 && guard--)
	{
		int step = (e->samplesUntilNextSegment < numSamples ? e->samplesUntilNextSegment : numSamples);
		if (step > 0)
		{
			if (e->slope)
			{
				if (e->segmentIsExponential) e->level *= TSF_POWF(e->slope, (float)step);
				else e->level += (e->slope * step);
			}
			e->samplesUntilNextSegment -= step;
			numSamples -= step;
		}
		if (e->samplesUntilNextSegment <= 0)
			tsf_voice_envelope_nextsegment(e, e->segment, outSampleRate);
	}
}

static void tsf_voice_lowpass_setup(struct tsf_voice_lowpass* e, float Fc)
{
	// Lowpass filter from http://www.earlevel.com/main/2012/11/26/biquad-c-source-code/
	// VDAC4: в одинарной точности — double здесь считался программно на каждом куске.
	// VDAC4: по SF2 усиление на постоянном токе ниже на половину резонанса в дБ (резонанс Q дБ
	// — множитель 10^(−Q/40) = sqrt(QInv)), как у SpessaSynth: резонансный бас не громче прочих.
	float K = TSF_TANF((float)TSF_PI * Fc), KK = K * K;
	float norm = 1.0f / (1.0f + K * e->QInv + KK);
	e->a0 = KK * norm * TSF_SQRTF(e->QInv);
	e->a1 = 2.0f * e->a0;
	e->b1 = 2.0f * (KK - 1.0f) * norm;
	e->b2 = (1.0f - K * e->QInv + KK) * norm;
}

static float tsf_voice_lowpass_process(struct tsf_voice_lowpass* e, float In)
{
	// VDAC4: float — в double это стоило сотни тактов на каждый отсчёт каждого голоса.
	float Out = In * e->a0 + e->z1; e->z1 = In * e->a1 + e->z2 - e->b1 * Out; e->z2 = In * e->a0 - e->b2 * Out; return Out;
}

static void tsf_voice_lfo_setup(struct tsf_voice_lfo* e, float delay, int freqCents, float outSampleRate)
{
	e->samplesUntil = (int)(delay * outSampleRate);
	e->delta = (4.0f * tsf_cents2Hertz((float)freqCents) / outSampleRate);
	e->level = 0;
}

static void tsf_voice_lfo_process(struct tsf_voice_lfo* e, int blockSamples)
{
	if (e->samplesUntil > blockSamples) { e->samplesUntil -= blockSamples; return; }
	e->level += e->delta * blockSamples;
	if      (e->level >  1.0f) { e->delta = -e->delta; e->level =  2.0f - e->level; }
	else if (e->level < -1.0f) { e->delta = -e->delta; e->level = -2.0f - e->level; }
}

static void tsf_voice_kill(struct tsf_voice* v)
{
	v->playingPreset = -1;
}

static void tsf_voice_end(tsf* f, struct tsf_voice* v)
{
	// if maxVoiceNum is set, assume that voice rendering and note queuing are on separate threads
	// so to minimize the chance that voice rendering would advance the segment at the same time
	// we just do it twice here and hope that it sticks
	int repeats = (f->maxVoiceNum ? 2 : 1);
	while (repeats--)
	{
		tsf_voice_envelope_nextsegment(&v->ampenv, TSF_SEGMENT_SUSTAIN, f->outSampleRate);
		tsf_voice_envelope_nextsegment(&v->modenv, TSF_SEGMENT_SUSTAIN, f->outSampleRate);
		if (v->region->loop_mode == TSF_LOOPMODE_SUSTAIN)
		{
			// Continue playing, but stop looping.
			v->loopEnd = v->loopStart;
		}
	}
}

static void tsf_voice_endquick(tsf* f, struct tsf_voice* v)
{
	// if maxVoiceNum is set, assume that voice rendering and note queuing are on separate threads
	// so to minimize the chance that voice rendering would advance the segment at the same time
	// we just do it twice here and hope that it sticks
	int repeats = (f->maxVoiceNum ? 2 : 1);
	while (repeats--)
	{
		v->ampenv.parameters.release = 0.0f; tsf_voice_envelope_nextsegment(&v->ampenv, TSF_SEGMENT_SUSTAIN, f->outSampleRate);
		v->modenv.parameters.release = 0.0f; tsf_voice_envelope_nextsegment(&v->modenv, TSF_SEGMENT_SUSTAIN, f->outSampleRate);
	}
}

// VDAC4: голос гаснет быстро (tsf_voice_endquick): отпускание нулевой длины.
static TSF_BOOL tsf_voice_quick_ending(const struct tsf_voice* v)
{
	return v->ampenv.segment >= TSF_SEGMENT_RELEASE && v->ampenv.parameters.release <= 0.0f;
}

// VDAC4: голосов в счёт предела — звучащие, кроме гаснущих быстро.
static int tsf_voices_counted(tsf* f)
{
	int count = 0;
	struct tsf_voice *v = f->voices, *vEnd = v + f->voiceNum;
	for (; v != vEnd; v++) if (v->playingPreset != -1 && !tsf_voice_quick_ending(v)) count++;
	return count;
}

// VDAC4: кривые модуляторов — как у FluidSynth 2 и SpessaSynth: вогнутая c(i) =
// −(400/960)·log10((127 − i)/127), выпуклая — 1 − c(127 − i); таблица считается при загрузке.
static float tsf_concave_tab[128];
static void tsf_mod_tables(void)
{
	int i;
	if (tsf_concave_tab[127] == 1.0f) return;
	for (i = 1; i < 127; i++) tsf_concave_tab[i] = -(400.0f / 960.0f) * TSF_LOG10F((127.0f - (float)i) / 127.0f);
	tsf_concave_tab[0] = 0.0f;
	tsf_concave_tab[127] = 1.0f;
}
static float tsf_concave(float v) { return (v < 0.0f ? 0.0f : (v >= 127.0f ? 1.0f : tsf_concave_tab[(int)v])); }
static float tsf_convex(float v) { return (v < 0.0f ? 0.0f : (v >= 127.0f ? 1.0f : 1.0f - tsf_concave_tab[127 - (int)v])); }

// VDAC4: начальные значения контроллеров (GM): громкость 100, экспрессия 127, панорама и
// звуковые контроллеры 70..79 — 64, реверберация 40 (GM2), прочие — 0.
static unsigned char tsf_cc_initial(int cc)
{
	return (unsigned char)(cc == 7 ? 100 : (cc == 11 ? 127 : ((cc == 10 || (cc >= 70 && cc <= 79)) ? 64 : (cc == 91 ? 40 : 0))));
}

// VDAC4: значение источника модулятора после кривой: однополярный 0..1, биполярный −1..1
// (нормировка и кривые — как у FluidSynth 2: контроллер / 128, колесо высоты / 16384).
static float tsf_mod_source(const struct tsf_channel* c, const struct tsf_voice* v, tsf_u16 src)
{
	int index = src & 0x7F, type = src >> 10;
	float x, range = 128.0f, val;
	if (src & 0x80) val = (float)(c ? c->cc[index] : tsf_cc_initial(index));
	else switch (index)
	{
		case  0: val = range; break;                                     // нет контроллера — 1
		case  2: val = v->effVel; break;
		case  3: val = v->effKey; break;
		case 10: val = v->polyPressure; break;
		case 13: val = (float)(c ? c->channelPressure : 0); break;
		case 14: val = (float)(c ? c->pitchWheel : 8192); range = 16384.0f; break;
		case 16: val = (c ? c->pitchRange : 2.0f); break;
		default: return 0.0f;
	}
	x = val / range;
	if (src & 0x100) x = 1.0f - x;
	if (!(src & 0x200))
		switch (type)
		{
			case 0: return x;
			case 1: return tsf_concave(127.0f * x);
			case 2: return tsf_convex(127.0f * x);
			default: return (x >= 0.5f ? 1.0f : 0.0f);
		}
	switch (type)
	{
		case 0: return 2.0f * x - 1.0f;
		case 1: return (x > 0.5f ? tsf_concave(127.0f * (2.0f * x - 1.0f)) : -tsf_concave(127.0f * (1.0f - 2.0f * x)));
		case 2: return (x > 0.5f ? tsf_convex(127.0f * (2.0f * x - 1.0f)) : -tsf_convex(127.0f * (1.0f - 2.0f * x)));
		default: return (x >= 0.5f ? 1.0f : -1.0f);
	}
}

static struct tsf_channel* tsf_voice_channel(tsf* f, const struct tsf_voice* v)
{
	return (f->channels && v->playingChannel >= 0 && v->playingChannel < f->channels->channelNum ? &f->channels->channels[v->playingChannel] : TSF_NULL);
}

// VDAC4: время стадии огибающей (с) со сдвигом в тайм-центах; «мгновенно» (0) — как −12000.
static float tsf_env_time(float seconds, float timecents)
{
	if (timecents == 0.0f) return seconds;
	return (seconds > 0.0f ? seconds : 0.0009765625f) * TSF_POWF(2.0f, timecents / 1200.0f);
}

// VDAC4: модуляторы SF2 голоса — громкость ноты (затухание региона по шкале EMU10K1 плюс
// модуляторы в сантибелах: скорость, CC7, CC11), панорама, вибрато, срез и резонанс фильтра,
// посылы, настройка, отпускание (пока голос не отпущен). Зовётся при нажатии и при смене
// источника (контроллер, давление); acc — итоги по приёмникам (может быть TSF_NULL).
static void tsf_voice_update(tsf* f, struct tsf_voice* v, float* acc)
{
	const struct tsf_region* r = v->region;
	struct tsf_channel* c = tsf_voice_channel(f, v);
	float sum[TSF_MD_COUNT], attenDB, pan, fc, q;
	int i;
	if (!acc) acc = sum;
	for (i = 0; i < TSF_MD_COUNT; i++) acc[i] = 0.0f;
	for (i = 0; i < r->modNum; i++)
	{
		const struct tsf_modulator* m = &r->mods[i];
		int dest = tsf_mod_dest(m->dst);
		float value;
		if (dest < 0 || !m->amount) continue;
		value = m->amount * tsf_mod_source(c, v, m->src);
		if (value != 0.0f && m->amtSrc) value *= tsf_mod_source(c, v, m->amtSrc);
		if (m->trans == 2 && value < 0.0f) value = -value;
		if (c && m->src == 0x0081 && m->dst == 6) value *= c->modRangeCents * (1.0f / 50.0f); // RPN 5
		acc[dest] += value;
	}
	attenDB = r->attenuation + acc[TSF_MD_ATTEN] * 0.1f;
	if (attenDB < 0.0f) attenDB = 0.0f; else if (attenDB > 144.0f) attenDB = 144.0f;
	v->noteGainDB = f->globalGainDB - attenDB + (c ? c->gainDB : 0.0f);
	pan = r->pan + acc[TSF_MD_PAN] * 0.001f + (c ? c->panOffset : 0.0f);
	if      (pan <= -0.5f) { v->panFactorLeft = 1.0f; v->panFactorRight = 0.0f; }
	else if (pan >=  0.5f) { v->panFactorLeft = 0.0f; v->panFactorRight = 1.0f; }
	else { v->panFactorLeft = TSF_SQRTF(0.5f - pan); v->panFactorRight = TSF_SQRTF(0.5f + pan); }
	v->vibLfoToPitch = r->vibLfoToPitch + acc[TSF_MD_VIB];
	v->modLfoToPitch = r->modLfoToPitch + acc[TSF_MD_MODLFO_PITCH];
	v->modEnvToPitch = r->modEnvToPitch + acc[TSF_MD_MODENV_PITCH];
	v->modLfoToFilterFc = r->modLfoToFilterFc + acc[TSF_MD_MODLFO_FC];
	v->modEnvToFilterFc = r->modEnvToFilterFc + acc[TSF_MD_MODENV_FC];
	v->modLfoToVolume = r->modLfoToVolume + acc[TSF_MD_MODLFO_VOL];
	fc = r->initialFilterFc + acc[TSF_MD_FC];
	if (fc < 1500.0f) fc = 1500.0f; else if (fc > 13500.0f) fc = 13500.0f;
	q = r->initialFilterQ + acc[TSF_MD_Q];
	if (q < 0.0f) q = 0.0f; else if (q > 960.0f) q = 960.0f;
	if (fc != v->initialFilterFc || q != v->filterQ)
	{
		v->initialFilterFc = fc, v->filterQ = q;
		v->lowpass.QInv = 1.0f / TSF_POWF(10.0f, q / 200.0f);
		v->lowpassDirty = 1;
	}
	v->reverbSend = (r->reverbSend + acc[TSF_MD_REVERB]) * 0.001f;
	if (v->reverbSend < 0.0f) v->reverbSend = 0.0f; else if (v->reverbSend > 1.0f) v->reverbSend = 1.0f;
	v->chorusSend = (r->chorusSend + acc[TSF_MD_CHORUS]) * 0.001f;
	if (v->chorusSend < 0.0f) v->chorusSend = 0.0f; else if (v->chorusSend > 1.0f) v->chorusSend = 1.0f;
	v->modTuneCents = acc[TSF_MD_COARSE] * 100.0f + acc[TSF_MD_FINE];
	if (v->ampenv.segment < TSF_SEGMENT_RELEASE) v->ampenv.parameters.release = tsf_env_time(r->ampenv.release, acc[TSF_MD_RELEASE]);
}

static void tsf_voice_calcpitchratio(struct tsf_voice* v, float pitchShift, float outSampleRate)
{
	// VDAC4: в одинарной точности (каждая нота и каждое событие колеса высоты).
	// ScaleTuning масштабирует только расстояние клавиши от корневой, CoarseTune и FineTune
	// прибавляются после: прежде они умножались вместе с клавишей, и у зоны ударных с
	// ScaleTuning 0 транспонирование пропадало. Клавиша — постоянная зоны (генератор 46),
	// если есть; настройка модуляторами — в modTuneCents.
	float adjustedPitch = v->region->pitch_keycenter + (v->effKey - v->region->pitch_keycenter) * (v->region->pitch_keytrack / 100.0f)
		+ v->region->transpose + v->region->tune / 100.0f + v->modTuneCents / 100.0f;
	if (pitchShift) adjustedPitch += pitchShift;
	v->pitchInputTimecents = adjustedPitch * 100.0f;
	v->pitchOutputFactor = v->region->sample_rate / (tsf_timecents2Secsf(v->region->pitch_keycenter * 100.0f) * outSampleRate);
}

// VDAC4: шаг позиции 32.32 из отношения частот во float (24 значащих бита хватает с
// запасом); дробь не доходит до 2^32 — иначе перевод в unsigned переполнился бы.
static void tsf_voice_ratio_step(float ratio, unsigned int* stepInt, unsigned int* stepFrac)
{
	unsigned int whole = (unsigned int)ratio;
	float frac = (ratio - (float)whole) * 4294967296.0f;
	*stepInt = whole;
	*stepFrac = (frac >= 4294967040.0f ? 4294967040u : (unsigned int)frac);
}

// VDAC4: 4-point Catmull-Rom interpolation of 16-bit samples. Neighbours follow the
// loop (next of loopEnd is loopStart) and are clamped to the sample data. Once the voice
// has wrapped (looped), the previous neighbour of loopStart is loopEnd, not the attack
// sample before the loop: otherwise each pass of the loop got a click at its seam.
static float tsf_voice_cubic(const tsf_s16* input, unsigned int last, unsigned int pos, float t,
	TSF_BOOL isLooping, unsigned int loopStart, unsigned int loopEnd, TSF_BOOL looped)
{
	unsigned int p0 = (isLooping && looped && pos == loopStart ? loopEnd : (pos ? pos - 1 : 0)), p2, p3;
	float s0, s1, s2, s3;
	p2 = (isLooping && pos >= loopEnd ? loopStart : pos + 1);
	p3 = (isLooping && p2 >= loopEnd ? loopStart : p2 + 1);
	if (p2 > last) p2 = last;
	if (p3 > last) p3 = last;
	s0 = input[p0], s1 = input[pos], s2 = input[p2], s3 = input[p3];
	return s1 + 0.5f * t * (s2 - s0 + t * (2.0f * s0 - 5.0f * s1 + 4.0f * s2 - s3 + t * (3.0f * (s1 - s2) + s3 - s0)));
}

// VDAC4: run отсчётов голоса без проверок краёв (быстрый путь tsf_voice_render): та же
// арифметика, что у общего пути (tsf_voice_cubic, tsf_voice_lowpass_process), поэтому звук
// тот же отсчёт в отсчёт. LOWPASS и SENDS — постоянные: четыре варианта цикла без ветвлений.
// Громкость внутри куска идёт линейно от начала к концу (с 1.7): прежде огибающая стояла
// ступенью на весь кусок (64 отсчёта), и быстрые атака и спад звучали ступеньками.
#define TSF_VDAC4_RUN(LOWPASS, SENDS) \
	for (; run; run--) \
	{ \
		const tsf_s16* s = input + posInt; \
		float t = (float)(posFrac >> 8) * (1.0f / 16777216.0f); \
		float s0 = s[-1], s1 = s[0], s2 = s[1], s3 = s[2]; \
		float val = s1 + 0.5f * t * (s2 - s0 + t * (2.0f * s0 - 5.0f * s1 + 4.0f * s2 - s3 + t * (3.0f * (s1 - s2) + s3 - s0))); \
		unsigned int oldFrac = posFrac; \
		if (LOWPASS) { float lp = val * a0 + z1; z1 = val * a1 + z2 - b1 * lp; z2 = val * a0 - b2 * lp; val = lp; } \
		outL[0] += val * gainLeft; outL[1] += val * gainRight; outL += 2; \
		if (SENDS) { float dry = val * gainMono; sendOut[0] += dry * sendReverb; sendOut[sendStride] += dry * sendChorus; sendOut[2 * sendStride] += dry * sendDelay; sendOut++; } \
		gainLeft += stepLeft; gainRight += stepRight; gainMono += stepMono; \
		posFrac += stepFrac; \
		posInt += stepInt + (posFrac < oldFrac ? 1 : 0); \
	}

static void tsf_voice_render(tsf* f, struct tsf_voice* v, float* outputBuffer, int numSamples, float* sendBuffer, int sendStride) // VDAC4: посылы — свои у каждого ядра
{
	struct tsf_region* region = v->region;
	const tsf_s16* input = f->fontSamples; // VDAC4: 16-bit samples
	const unsigned int lastSample = (f->fontSampleCount ? f->fontSampleCount - 1 : 0);
	float* outL = outputBuffer;
	float* outR = (f->outputmode == TSF_STEREO_UNWEAVED ? outL + numSamples : TSF_NULL);

	// VDAC4: посылы реверберации и хоруса — у голоса (модуляторы SF2: CC91, CC93 и свои
	// модуляторы банка), эха — у канала (CC94).
	struct tsf_channel* vch = tsf_voice_channel(f, v);
	float* sendOut = sendBuffer;
	float sendReverb = v->reverbSend, sendChorus = v->chorusSend, sendDelay = (vch ? vch->delaySend : 0.0f);
	if (!sendBuffer || (sendReverb <= 0.0f && sendChorus <= 0.0f && sendDelay <= 0.0f)) sendOut = TSF_NULL;

	// Cache some values, to give them at least some chance of ending up in registers.
	// VDAC4: параметры — голоса (регион плюс модуляторы, tsf_voice_update).
	TSF_BOOL updateModEnv = (v->modEnvToPitch != 0.0f || v->modEnvToFilterFc != 0.0f);
	TSF_BOOL updateModLFO = (v->modlfo.delta && (v->modLfoToPitch != 0.0f || v->modLfoToFilterFc != 0.0f || v->modLfoToVolume != 0.0f));
	TSF_BOOL updateVibLFO = (v->viblfo.delta && v->vibLfoToPitch != 0.0f);
	TSF_BOOL isLooping    = (v->loopStart < v->loopEnd);
	unsigned int tmpLoopStart = v->loopStart, tmpLoopEnd = v->loopEnd;
	// VDAC4: position as 32.32 fixed point (the ESP32-S3 has no hardware double).
	unsigned int tmpSampleEnd = region->end, loopLength = tmpLoopEnd - tmpLoopStart + 1;
	unsigned int posInt = v->sourcePosInt;
	unsigned int posFrac = v->sourcePosFrac;
	TSF_BOOL looped = v->looped; // VDAC4
	struct tsf_voice_lowpass tmpLowpass = v->lowpass;

	// VDAC4: фильтр пересчитывается и после смены среза или резонанса модуляторами.
	TSF_BOOL dynamicLowpass = (v->modLfoToFilterFc != 0.0f || v->modEnvToFilterFc != 0.0f || v->lowpassDirty);
	float tmpSampleRate = f->outSampleRate;

	TSF_BOOL dynamicPitchRatio = (v->modLfoToPitch != 0.0f || v->modEnvToPitch != 0.0f || v->vibLfoToPitch != 0.0f || v->portaCents != 0.0f); // VDAC4
	float pitchRatio; // VDAC4: float
	unsigned int stepInt, stepFrac;

	TSF_BOOL dynamicGain = (v->modLfoToVolume != 0.0f);
	float tmpModLfoToVolume = v->modLfoToVolume * 0.1f;
	// VDAC4: громкость ноты (отсчёты 16-битные — сразу с масштабом) и громкость в начале
	// куска; дальше — линейно к концу куска.
	float noteGain = (dynamicGain ? 0.0f : tsf_decibelsToGain(v->noteGainDB) * (1.0f / 32767.0f));
	float gainStart = (dynamicGain ? tsf_decibelsToGain(v->noteGainDB + v->modlfo.level * tmpModLfoToVolume) * (1.0f / 32767.0f) : noteGain) * v->ampenv.level;

	if (dynamicPitchRatio) pitchRatio = 0;
	else pitchRatio = tsf_timecents2Secsf(v->pitchInputTimecents) * v->pitchOutputFactor;
	tsf_voice_ratio_step(pitchRatio, &stepInt, &stepFrac); // VDAC4

	while (numSamples)
	{
		float gainMono, gainLeft, gainRight, gainEnd, stepMono, stepLeft, stepRight;
		int blockSamples = (numSamples > TSF_RENDER_EFFECTSAMPLEBLOCK ? TSF_RENDER_EFFECTSAMPLEBLOCK : numSamples);
		numSamples -= blockSamples;

		if (dynamicLowpass)
		{
			float fres = v->initialFilterFc + v->modlfo.level * v->modLfoToFilterFc + v->modenv.level * v->modEnvToFilterFc;
			float lowpassFc = (fres <= 13500 ? tsf_cents2Hertz(fres) / tmpSampleRate : 1.0f);
			tmpLowpass.active = (lowpassFc < 0.499f);
			if (tmpLowpass.active) tsf_voice_lowpass_setup(&tmpLowpass, lowpassFc);
		}

		if (dynamicPitchRatio)
		{
			pitchRatio = tsf_timecents2Secsf(v->pitchInputTimecents + v->portaCents + (v->modlfo.level * v->modLfoToPitch + v->viblfo.level * v->vibLfoToPitch + v->modenv.level * v->modEnvToPitch)) * v->pitchOutputFactor;
			tsf_voice_ratio_step(pitchRatio, &stepInt, &stepFrac); // VDAC4
			// VDAC4: portamento glides towards the note.
			if (v->portaCents > 0.0f) { v->portaCents -= v->portaStep * blockSamples; if (v->portaCents < 0.0f) v->portaCents = 0.0f; }
			else if (v->portaCents < 0.0f) { v->portaCents += v->portaStep * blockSamples; if (v->portaCents > 0.0f) v->portaCents = 0.0f; }
		}

		// Update EG.
		tsf_voice_envelope_process(&v->ampenv, blockSamples, tmpSampleRate);
		if (updateModEnv) tsf_voice_envelope_process(&v->modenv, blockSamples, tmpSampleRate);

		// Update LFOs.
		if (updateModLFO) tsf_voice_lfo_process(&v->modlfo, blockSamples);
		if (updateVibLFO) tsf_voice_lfo_process(&v->viblfo, blockSamples);

		// VDAC4: громкость к концу куска — по огибающей после него; внутри — линейно.
		gainEnd = (dynamicGain ? tsf_decibelsToGain(v->noteGainDB + v->modlfo.level * tmpModLfoToVolume) * (1.0f / 32767.0f) : noteGain) * v->ampenv.level;
		gainMono = gainStart, stepMono = (gainEnd - gainStart) / (float)blockSamples;
		gainLeft = gainMono * v->panFactorLeft, gainRight = gainMono * v->panFactorRight;
		stepLeft = stepMono * v->panFactorLeft, stepRight = stepMono * v->panFactorRight;
		gainStart = gainEnd;
		if (f->outputmode == TSF_STEREO_INTERLEAVED)
		{
			// VDAC4: быстрый путь (MIDI.ELF выводит только так). Голос стоил на ESP32-S3
			// около 240 тактов на отсчёт: фильтр жил в памяти, режим вывода и посылы
			// проверялись на каждом отсчёте. Отсчёты, где до края петли и сэмпла далеко
			// (позиция от 1 до limit), идут пачкой без проверок; у краёв — по одному,
			// общим путём. Петля замыкается после пачки: внутри неё позиция петли не
			// проходит.
			float z1 = tmpLowpass.z1, z2 = tmpLowpass.z2;
			const float a0 = tmpLowpass.a0, a1 = tmpLowpass.a1, b1 = tmpLowpass.b1, b2 = tmpLowpass.b2;
			const TSF_BOOL lowpass = tmpLowpass.active;
			unsigned int limit = (lastSample >= 2 ? lastSample - 2 : 0);
			if (tmpSampleEnd != 0 && tmpSampleEnd - 1 < limit) limit = tmpSampleEnd - 1;
			if (isLooping) limit = (tmpLoopEnd < 2 ? 0 : (tmpLoopEnd - 2 < limit ? tmpLoopEnd - 2 : limit));
			while (blockSamples > 0 && posInt < tmpSampleEnd)
			{
				unsigned int run = 0;
				if (posInt >= (isLooping && looped ? tmpLoopStart + 1 : 1) && posInt <= limit && lastSample >= 2)
				{
					run = (limit - posInt) / (stepInt + 1) + 1;
					if (run > (unsigned int)blockSamples) run = (unsigned int)blockSamples;
				}
				if (run)
				{
					blockSamples -= (int)run;
					if (lowpass) { if (sendOut) TSF_VDAC4_RUN(1, 1) else TSF_VDAC4_RUN(1, 0) }
					else { if (sendOut) TSF_VDAC4_RUN(0, 1) else TSF_VDAC4_RUN(0, 0) }
				}
				else
				{
					float val = tsf_voice_cubic(input, lastSample, posInt, (float)(posFrac >> 8) * (1.0f / 16777216.0f), isLooping, tmpLoopStart, tmpLoopEnd, looped);
					unsigned int oldFrac = posFrac;
					if (lowpass) { float lp = val * a0 + z1; z1 = val * a1 + z2 - b1 * lp; z2 = val * a0 - b2 * lp; val = lp; }
					outL[0] += val * gainLeft; outL[1] += val * gainRight; outL += 2;
					if (sendOut)
					{
						float dry = val * gainMono;
						sendOut[0] += dry * sendReverb;
						sendOut[sendStride] += dry * sendChorus;
						sendOut[2 * sendStride] += dry * sendDelay;
						sendOut++;
					}
					gainLeft += stepLeft; gainRight += stepRight; gainMono += stepMono;
					posFrac += stepFrac;
					posInt += stepInt + (posFrac < oldFrac ? 1 : 0);
					blockSamples--;
				}
				if (isLooping && posInt > tmpLoopEnd) { posInt = tmpLoopStart + (posInt - tmpLoopStart) % loopLength; looped = 1; } // VDAC4
			}
			tmpLowpass.z1 = z1, tmpLowpass.z2 = z2;
		}
		else while (blockSamples-- && posInt < tmpSampleEnd)
		{
			// VDAC4: fraction in single precision (24 bits): double would be software per sample.
			float val = tsf_voice_cubic(input, lastSample, posInt, (float)(posFrac >> 8) * (1.0f / 16777216.0f), isLooping, tmpLoopStart, tmpLoopEnd, looped);
			unsigned int oldFrac = posFrac;

			// Low-pass filter.
			if (tmpLowpass.active) val = tsf_voice_lowpass_process(&tmpLowpass, val);

			switch (f->outputmode)
			{
				case TSF_STEREO_INTERLEAVED: *outL++ += val * gainLeft; *outL++ += val * gainRight; break;
				case TSF_STEREO_UNWEAVED: *outL++ += val * gainLeft; *outR++ += val * gainRight; break;
				case TSF_MONO: *outL++ += val * gainMono; break;
			}

			// VDAC4: mono effect sends, before panning.
			if (sendOut)
			{
				float dry = val * gainMono;
				sendOut[0] += dry * sendReverb;
				sendOut[sendStride] += dry * sendChorus;
				sendOut[2 * sendStride] += dry * sendDelay;
				sendOut++;
			}
			gainLeft += stepLeft; gainRight += stepRight; gainMono += stepMono; // VDAC4

			// Next sample.
			posFrac += stepFrac;
			posInt += stepInt + (posFrac < oldFrac ? 1 : 0);
			if (isLooping && posInt > tmpLoopEnd) { posInt = tmpLoopStart + (posInt - tmpLoopStart) % loopLength; looped = 1; } // VDAC4
		}

		if (posInt >= tmpSampleEnd || v->ampenv.segment == TSF_SEGMENT_DONE)
		{
			tsf_voice_kill(v);
			return;
		}
	}

	v->sourcePosInt = posInt, v->sourcePosFrac = posFrac, v->looped = (unsigned char)looped; // VDAC4
	if (tmpLowpass.active || dynamicLowpass) v->lowpass = tmpLowpass;
	v->lowpassDirty = 0; // VDAC4
}

TSFDEF tsf* tsf_load(struct tsf_stream* stream)
{
	tsf* res = TSF_NULL;
	struct tsf_riffchunk chunkHead;
	struct tsf_riffchunk chunkList;
	struct tsf_hydra hydra;
	void* rawBuffer = TSF_NULL;
	float* floatBuffer = TSF_NULL;
	tsf_s16* shortBuffer = TSF_NULL; // VDAC4
	int shortInPlace = 0; // VDAC4
	tsf_u32 smplCount = 0;

	tsf_mod_tables(); // VDAC4: кривые модуляторов

	if (!tsf_riffchunk_read(TSF_NULL, &chunkHead, stream) || !TSF_FourCCEquals(chunkHead.id, "sfbk"))
	{
		//if (e) *e = TSF_INVALID_NOSF2HEADER;
		return res;
	}

	// Read hydra and locate sample data.
	TSF_MEMSET(&hydra, 0, sizeof(hydra));
	while (tsf_riffchunk_read(&chunkHead, &chunkList, stream))
	{
		struct tsf_riffchunk chunk;
		if (TSF_FourCCEquals(chunkList.id, "pdta"))
		{
			while (tsf_riffchunk_read(&chunkList, &chunk, stream))
			{
				// VDAC4: второй такой же блок пропускается (прежде память первого терялась), чтение
				// каждой записи проверяется — обрезанный блок не даёт записей с мусором.
				#define HandleChunk(chunkName) (TSF_FourCCEquals(chunk.id, #chunkName) && !(chunk.size % chunkName##SizeInFile) && !hydra.chunkName##s && chunk.size) \
					{ \
						int num = chunk.size / chunkName##SizeInFile, i; \
						hydra.chunkName##Num = num; \
						hydra.chunkName##s = (struct tsf_hydra_##chunkName*)TSF_MALLOC(num * sizeof(struct tsf_hydra_##chunkName)); \
						if (!hydra.chunkName##s) goto out_of_memory; \
						for (i = 0; i < num; ++i) if (!tsf_hydra_read_##chunkName(&hydra.chunkName##s[i], stream)) goto out_of_memory; \
					}
				enum
				{
					phdrSizeInFile = 38, pbagSizeInFile =  4, pmodSizeInFile = 10,
					pgenSizeInFile =  4, instSizeInFile = 22, ibagSizeInFile =  4,
					imodSizeInFile = 10, igenSizeInFile =  4, shdrSizeInFile = 46
				};
				if      HandleChunk(phdr) else if HandleChunk(pbag) else if HandleChunk(pmod)
				else if HandleChunk(pgen) else if HandleChunk(inst) else if HandleChunk(ibag)
				else if HandleChunk(imod) else if HandleChunk(igen) else if HandleChunk(shdr)
				else stream->skip(stream->data, chunk.size);
				#undef HandleChunk
			}
		}
		else if (TSF_FourCCEquals(chunkList.id, "sdta"))
		{
			while (tsf_riffchunk_read(&chunkList, &chunk, stream))
			{
				if ((TSF_FourCCEquals(chunk.id, "smpl")
						#ifdef STB_VORBIS_INCLUDE_STB_VORBIS_H
						|| TSF_FourCCEquals(chunk.id, "smpo")
						#endif
					) && !rawBuffer && !floatBuffer && !shortBuffer && chunk.size >= sizeof(short))
				{
					// VDAC4: 16-bit samples, in place when loading from memory.
					if (!tsf_load_samples_s16(&shortBuffer, &shortInPlace, &smplCount, &chunk, stream)) goto out_of_memory;
				}
				else stream->skip(stream->data, chunk.size);
			}
		}
		else stream->skip(stream->data, chunkList.size);
	}
	if (!hydra.phdrs || !hydra.pbags || !hydra.pmods || !hydra.pgens || !hydra.insts || !hydra.ibags || !hydra.imods || !hydra.igens || !hydra.shdrs || !tsf_hydra_valid(&hydra)) // VDAC4: связи
	{
		//if (e) *e = TSF_INVALID_INCOMPLETE;
	}
	else if (!shortBuffer) // VDAC4
	{
		//if (e) *e = TSF_INVALID_NOSAMPLEDATA;
	}
	else
	{
		#ifdef STB_VORBIS_INCLUDE_STB_VORBIS_H
		if (!floatBuffer && !tsf_decode_sf3_samples(rawBuffer, &floatBuffer, &smplCount, &hydra)) goto out_of_memory;
		#endif
		res = (tsf*)TSF_MALLOC(sizeof(tsf));
		if (res) TSF_MEMSET(res, 0, sizeof(tsf));
		if (!res || !tsf_load_presets(res, &hydra, smplCount)) goto out_of_memory;
		res->outSampleRate = 44100.0f;
		res->fontSamples = shortBuffer; // VDAC4
		res->fontSampleCount = smplCount;
		res->fontSamplesOwned = !shortInPlace;
		shortBuffer = TSF_NULL; // don't free below
		{
			// VDAC4: банк без играбельных зон — не банк (прежде открывался молчащим).
			int p, regions = 0;
			for (p = 0; p < res->presetNum; p++) regions += res->presets[p].regionNum;
			if (!regions) { tsf_close(res); res = TSF_NULL; }
		}
	}
	if (0)
	{
		out_of_memory:
		TSF_FREE(res);
		res = TSF_NULL;
		//if (e) *e = TSF_OUT_OF_MEMORY;
	}
	TSF_FREE(hydra.phdrs); TSF_FREE(hydra.pbags); TSF_FREE(hydra.pmods);
	TSF_FREE(hydra.pgens); TSF_FREE(hydra.insts); TSF_FREE(hydra.ibags);
	TSF_FREE(hydra.imods); TSF_FREE(hydra.igens); TSF_FREE(hydra.shdrs);
	TSF_FREE(rawBuffer);   TSF_FREE(floatBuffer);
	if (!shortInPlace) TSF_FREE(shortBuffer); // VDAC4
	return res;
}

TSFDEF tsf* tsf_copy(tsf* f)
{
	tsf* res;
	if (!f) return TSF_NULL;
	if (!f->refCount)
	{
		f->refCount = (int*)TSF_MALLOC(sizeof(int));
		if (!f->refCount) return TSF_NULL;
		*f->refCount = 1;
	}
	res = (tsf*)TSF_MALLOC(sizeof(tsf));
	if (!res) return TSF_NULL;
	TSF_MEMCPY(res, f, sizeof(tsf));
	res->voices = TSF_NULL;
	res->voiceNum = 0;
	res->channels = TSF_NULL;
	(*res->refCount)++;
	return res;
}

TSFDEF void tsf_close(tsf* f)
{
	if (!f) return;
	if (!f->refCount || !--(*f->refCount))
	{
		struct tsf_preset *preset = f->presets, *presetEnd = preset + f->presetNum;
		for (; preset != presetEnd; preset++) TSF_FREE(preset->regions), TSF_FREE(preset->mods); // VDAC4: и модуляторы
		TSF_FREE(f->presets);
		if (f->fontSamplesOwned) TSF_FREE((void*)f->fontSamples); // VDAC4
		TSF_FREE(f->refCount);
	}
	TSF_FREE(f->channels);
	TSF_FREE(f->voices);
	TSF_FREE(f);
}

TSFDEF void tsf_reset(tsf* f)
{
	struct tsf_voice *v = f->voices, *vEnd = v + f->voiceNum;
	for (; v != vEnd; v++)
		if (v->playingPreset != -1 && (v->ampenv.segment < TSF_SEGMENT_RELEASE || v->ampenv.parameters.release))
			tsf_voice_endquick(f, v);
	if (f->channels) { TSF_FREE(f->channels); f->channels = TSF_NULL; }
}

TSFDEF int tsf_get_presetindex(const tsf* f, int bank, int preset_number)
{
	const struct tsf_preset *presets;
	int i, iMax;
	for (presets = f->presets, i = 0, iMax = f->presetNum; i < iMax; i++)
		if (presets[i].preset == preset_number && presets[i].bank == bank)
			return i;
	return -1;
}

TSFDEF int tsf_get_presetcount(const tsf* f)
{
	return f->presetNum;
}

TSFDEF const char* tsf_get_presetname(const tsf* f, int preset)
{
	return (preset < 0 || preset >= f->presetNum ? TSF_NULL : f->presets[preset].presetName);
}

TSFDEF const char* tsf_bank_get_presetname(const tsf* f, int bank, int preset_number)
{
	return tsf_get_presetname(f, tsf_get_presetindex(f, bank, preset_number));
}

TSFDEF void tsf_set_output(tsf* f, enum TSFOutputMode outputmode, int samplerate, float global_gain_db)
{
	f->outputmode = outputmode;
	f->outSampleRate = (float)(samplerate >= 1 ? samplerate : 44100.0f);
	f->globalGainDB = global_gain_db;
}

TSFDEF void tsf_set_volume(tsf* f, float global_volume)
{
	f->globalGainDB = (global_volume == 1.0f ? 0 : -tsf_gainToDecibels(1.0f / global_volume));
}

TSFDEF int tsf_set_max_voices(tsf* f, int max_voices)
{
	int i = f->voiceNum;
	int newVoiceNum = (f->voiceNum > max_voices ? f->voiceNum : max_voices);
	struct tsf_voice *newVoices = (struct tsf_voice*)TSF_REALLOC(f->voices, newVoiceNum * sizeof(struct tsf_voice));
	if (!newVoices) return 0;
	f->voices = newVoices;
	f->voiceNum = f->maxVoiceNum = newVoiceNum;
	for (; i < max_voices; i++)
		f->voices[i].playingPreset = -1;
	return 1;
}

// VDAC4: сдвиг высоты канала (полутоны): колесо, настройка RPN, общая настройка SysEx.
static float tsf_channel_pitchshift(const struct tsf_channel* c)
{
	return (c->pitchWheel == 8192 ? 0.0f : (c->pitchWheel / 16383.0f * c->pitchRange * 2.0f) - c->pitchRange) + c->tuning + c->masterTuning;
}

// VDAC4: громкость голоса для выбора жертвы — огибающая вместе с громкостью ноты (скорость,
// затухание, канал), а не одна огибающая: тихий и громкий инструменты прежде были равны.
static float tsf_voice_loudness(const struct tsf_voice* v)
{
	return v->ampenv.level * tsf_decibelsToGain(v->noteGainDB);
}

// VDAC4: голос держит педаль (sustain или sostenuto), клавиша отпущена.
static TSF_BOOL tsf_voice_pedal_held(const struct tsf_voice* v)
{
	return (v->heldSustain || v->heldSostenuto) && v->ampenv.segment < TSF_SEGMENT_RELEASE;
}

// VDAC4: голос на замену новой ноте: самый тихий из отпущенных, затем из держащихся педалью,
// затем (при пределе, tsf_set_voice_cap) из прошедших атаку; при равной громкости — старший.
// Гаснущие быстро не берутся: они уже уходят.
static struct tsf_voice* tsf_voice_victim(tsf* f)
{
	int pass;
	for (pass = 0; pass < 3; pass++)
	{
		struct tsf_voice *v, *vEnd = f->voices + f->voiceNum, *best = TSF_NULL;
		float bestLoud = 0.0f;
		if (pass == 2 && f->voiceCap <= 0) break;
		for (v = f->voices; v != vEnd; v++)
		{
			float loud;
			if (v->playingPreset == -1 || tsf_voice_quick_ending(v)) continue;
			if (pass == 0 && v->ampenv.segment != TSF_SEGMENT_RELEASE) continue;
			if (pass == 1 && !tsf_voice_pedal_held(v)) continue;
			if (pass == 2 && !(v->ampenv.segment >= TSF_SEGMENT_HOLD && v->ampenv.segment < TSF_SEGMENT_RELEASE)) continue;
			loud = tsf_voice_loudness(v);
			if (!best || loud < bestLoud || (loud == bestLoud && v->playIndex < best->playIndex)) best = v, bestLoud = loud;
		}
		if (best) return best;
	}
	return TSF_NULL;
}

// VDAC4: нота жертвы гаснет целиком (стереопара и слои — голоса одного нажатия) и плавно, за
// 10 мс, в запасных местах массива голосов: прежде голос обрывался на месте — щелчок.
static void tsf_voice_steal(tsf* f, struct tsf_voice* victim)
{
	struct tsf_voice *v, *vEnd = f->voices + f->voiceNum;
	unsigned int playIndex = victim->playIndex;
	int channel = victim->playingChannel;
	for (v = f->voices; v != vEnd; v++)
		if (v->playingPreset != -1 && v->playIndex == playIndex && v->playingChannel == channel && !tsf_voice_quick_ending(v))
			tsf_voice_endquick(f, v);
}

// VDAC4: место для нового голоса: свободное, а нет его — гаснущий быстро, ближе всех к концу.
static struct tsf_voice* tsf_voice_spare(tsf* f)
{
	struct tsf_voice *v, *vEnd = f->voices + f->voiceNum, *best = TSF_NULL;
	for (v = f->voices; v != vEnd; v++) if (v->playingPreset == -1) return v;
	for (v = f->voices; v != vEnd; v++)
		if (tsf_voice_quick_ending(v) && (!best || v->ampenv.samplesUntilNextSegment < best->ampenv.samplesUntilNextSegment)) best = v;
	if (best) tsf_voice_kill(best);
	return best;
}

TSFDEF int tsf_note_on(tsf* f, int preset_index, int key, float vel)
{
	short midiVelocity = (short)(vel * 127.0f + 0.5f); // VDAC4: с округлением (100/127·127 давало 99)
	unsigned int voicePlayIndex;
	struct tsf_region *region, *regionEnd;
	struct tsf_channel* c = (f->channels ? &f->channels->channels[f->channels->activeChannel] : TSF_NULL);

	if (preset_index < 0 || preset_index >= f->presetNum) return 1;
	if (vel <= 0.0f) { tsf_note_off(f, preset_index, key); return 1; }
	if (midiVelocity > 127) midiVelocity = 127;
	if (midiVelocity < 1) midiVelocity = 1;

	// Play all matching regions.
	voicePlayIndex = f->voicePlayIndex++;
	for (region = f->presets[preset_index].regions, regionEnd = region + f->presets[preset_index].regionNum; region != regionEnd; region++)
	{
		struct tsf_voice *voice = TSF_NULL, *v, *vEnd = f->voices + f->voiceNum; TSF_BOOL doLoop; float lowpassFc, acc[TSF_MD_COUNT];
		struct tsf_envelope ampenv, modenv;
		if (key < region->lokey || key > region->hikey || midiVelocity < region->lovel || midiVelocity > region->hivel) continue;

		if (region->group)
		{
			// VDAC4: класс глушит голоса прежних нот своего канала (без каналов — своего
			// пресета), но не слои этой же ноты: прежде второй регион двухслойного хай-хэта
			// гасил первый, а нота с тем же пресетом на другом канале гасила чужие голоса.
			int channel = (f->channels ? f->channels->activeChannel : -1);
			for (v = f->voices; v != vEnd; v++)
				if (v->playingPreset != -1 && v->playIndex != voicePlayIndex && v->region->group == region->group &&
				    (channel >= 0 ? v->playingChannel == channel : v->playingPreset == preset_index)) tsf_voice_endquick(f, v);
		}
		for (v = f->voices; v != vEnd; v++) if (v->playingPreset == -1) { voice = v; break; }

		// VDAC4: предел голосов (tsf_set_voice_cap) достигнут или мест нет — новой ноте отдаёт
		// голос другая: её голоса гаснут за 10 мс, новый берёт запасное место, а без запаса —
		// место жертвы (тогда — обрыв, как прежде).
		if ((voice && f->voiceCap > 0 && tsf_voices_counted(f) >= f->voiceCap) || (!voice && f->maxVoiceNum))
		{
			struct tsf_voice* victim = tsf_voice_victim(f);
			if (!victim)
			{
				f->voicesRefused++; // VDAC4
				continue;
			}
			tsf_voice_steal(f, victim);
			if (!voice) voice = tsf_voice_spare(f);
			if (!voice) { tsf_voice_kill(victim); voice = victim; }
		}
		else if (!voice)
		{
			// Allocate more voices so we don't need to kill one off.
			struct tsf_voice* newVoices;
			f->voiceNum += 4;
			newVoices = (struct tsf_voice*)TSF_REALLOC(f->voices, f->voiceNum * sizeof(struct tsf_voice));
			if (!newVoices) return 0;
			f->voices = newVoices;
			voice = &f->voices[f->voiceNum - 4];
			voice[1].playingPreset = voice[2].playingPreset = voice[3].playingPreset = -1;
		}

		voice->region = region;
		voice->playingPreset = preset_index;
		voice->playingKey = key;
		voice->playingChannel = (f->channels ? f->channels->activeChannel : -1);
		voice->playIndex = voicePlayIndex;
		voice->heldSustain = 0;
		// VDAC4: постоянные клавиша и скорость зоны (генераторы 46, 47) подменяют настоящие для
		// высоты, огибающих и модуляторов; диапазоны зоны выбираются по настоящим.
		voice->effKey = (unsigned char)(region->keynum >= 0 && region->keynum <= 127 ? region->keynum : key);
		voice->effVel = (unsigned char)(region->velocity > 0 && region->velocity <= 127 ? region->velocity : midiVelocity);
		voice->polyPressure = voice->sostenutoLatched = voice->heldSostenuto = 0;
		voice->portaCents = voice->portaStep = 0; // VDAC4
		voice->initialFilterFc = voice->filterQ = -1.0f; // VDAC4: фильтр — по итогам модуляторов
		voice->ampenv.segment = TSF_SEGMENT_NONE;
		// VDAC4: громкость (скорость, CC7, CC11 — модуляторы SF2), панорама, вибрато, фильтр,
		// посылы, настройка — модуляторы голоса.
		tsf_voice_update(f, voice, acc);
		tsf_voice_calcpitchratio(voice, (c ? tsf_channel_pitchshift(c) : 0.0f), f->outSampleRate);

		// Offset/end.
		voice->sourcePosInt = region->offset, voice->sourcePosFrac = 0, voice->looped = 0; // VDAC4

		// Loop.
		doLoop = (region->loop_mode != TSF_LOOPMODE_NONE && region->loop_start < region->loop_end);
		voice->loopStart = (doLoop ? region->loop_start : 0);
		voice->loopEnd = (doLoop ? region->loop_end : 0);

		// Setup envelopes.
		// VDAC4: атака, спад и отпускание огибающей громкости — со сдвигом модуляторов (CC73,
		// CC75, CC72 и модуляторы банка, тайм-центы).
		ampenv = region->ampenv, modenv = region->modenv;
		ampenv.attack = tsf_env_time(ampenv.attack, acc[TSF_MD_ATTACK]);
		if (ampenv.keynumToDecay) ampenv.decay += acc[TSF_MD_DECAY];
		else ampenv.decay = tsf_env_time(ampenv.decay, acc[TSF_MD_DECAY]);
		ampenv.release = tsf_env_time(ampenv.release, acc[TSF_MD_RELEASE]);
		tsf_voice_envelope_setup(&voice->ampenv, &ampenv, voice->effKey, voice->effVel, TSF_TRUE, f->outSampleRate);
		tsf_voice_envelope_setup(&voice->modenv, &modenv, voice->effKey, voice->effVel, TSF_FALSE, f->outSampleRate);

		// Setup lowpass filter.
		lowpassFc = (voice->initialFilterFc <= 13500 ? tsf_cents2Hertz(voice->initialFilterFc) / f->outSampleRate : 1.0f);
		voice->lowpass.z1 = voice->lowpass.z2 = 0;
		voice->lowpass.active = (lowpassFc < 0.499f);
		if (voice->lowpass.active) tsf_voice_lowpass_setup(&voice->lowpass, lowpassFc);
		voice->lowpassDirty = 0;

		// Setup LFO filters.
		tsf_voice_lfo_setup(&voice->modlfo, region->delayModLFO, region->freqModLFO, f->outSampleRate);
		tsf_voice_lfo_setup(&voice->viblfo, region->delayVibLFO, region->freqVibLFO, f->outSampleRate);
	}
	return 1;
}

TSFDEF int tsf_bank_note_on(tsf* f, int bank, int preset_number, int key, float vel)
{
	int preset_index = tsf_get_presetindex(f, bank, preset_number);
	if (preset_index == -1) return 0;
	return tsf_note_on(f, preset_index, key, vel);
}

TSFDEF void tsf_note_off(tsf* f, int preset_index, int key)
{
	struct tsf_voice *v = f->voices, *vEnd = v + f->voiceNum, *vMatchFirst = TSF_NULL, *vMatchLast = TSF_NULL;
	for (; v != vEnd; v++)
	{
		//Find the first and last entry in the voices list with matching preset, key and look up the smallest play index
		if (v->playingPreset != preset_index || v->playingKey != key || v->ampenv.segment >= TSF_SEGMENT_RELEASE) continue;
		else if (!vMatchFirst || v->playIndex < vMatchFirst->playIndex) vMatchFirst = vMatchLast = v;
		else if (v->playIndex == vMatchFirst->playIndex) vMatchLast = v;
	}
	if (!vMatchFirst) return;
	for (v = vMatchFirst; v <= vMatchLast; v++)
	{
		//Stop all voices with matching preset, key and the smallest play index which was enumerated above
		if (v != vMatchFirst && v != vMatchLast &&
			(v->playIndex != vMatchFirst->playIndex || v->playingPreset != preset_index || v->playingKey != key || v->ampenv.segment >= TSF_SEGMENT_RELEASE)) continue;
		tsf_voice_end(f, v);
	}
}

TSFDEF int tsf_bank_note_off(tsf* f, int bank, int preset_number, int key)
{
	int preset_index = tsf_get_presetindex(f, bank, preset_number);
	if (preset_index == -1) return 0;
	tsf_note_off(f, preset_index, key);
	return 1;
}

TSFDEF void tsf_note_off_all(tsf* f)
{
	struct tsf_voice *v = f->voices, *vEnd = v + f->voiceNum;
	for (; v != vEnd; v++) if (v->playingPreset != -1 && v->ampenv.segment < TSF_SEGMENT_RELEASE)
		tsf_voice_end(f, v);
}

TSFDEF int tsf_active_voice_count(tsf* f)
{
	int count = 0;
	struct tsf_voice *v = f->voices, *vEnd = v + f->voiceNum;
	for (; v != vEnd; v++) if (v->playingPreset != -1) count++;
	return count;
}

TSFDEF void tsf_render_short(tsf* f, short* buffer, int samples, int flag_mixing)
{
	float outputSamples[TSF_RENDER_SHORTBUFFERBLOCK];
	int channels = (f->outputmode == TSF_MONO ? 1 : 2), maxChannelSamples = TSF_RENDER_SHORTBUFFERBLOCK / channels;
	while (samples > 0)
	{
		int channelSamples = (samples > maxChannelSamples ? maxChannelSamples : samples);
		short* bufferEnd = buffer + channelSamples * channels;
		float *floatSamples = outputSamples;
		tsf_render_float(f, floatSamples, channelSamples, TSF_FALSE);
		samples -= channelSamples;

		if (flag_mixing)
			while (buffer != bufferEnd)
			{
				float v = *floatSamples++;
				int vi = *buffer + (v < -1.00004566f ? (int)-32768 : (v > 1.00001514f ? (int)32767 : (int)(v * 32767.5f)));
				*buffer++ = (vi < -32768 ? (short)-32768 : (vi > 32767 ? (short)32767 : (short)vi));
			}
		else
			while (buffer != bufferEnd)
			{
				float v = *floatSamples++;
				*buffer++ = (v < -1.00004566f ? (short)-32768 : (v > 1.00001514f ? (short)32767 : (short)(v * 32767.5f)));
			}
	}
}

TSFDEF void tsf_render_float_sends(tsf* f, float* buffer, float* sends, int samples) // VDAC4
{
	f->sendBuffer = sends;
	f->sendStride = samples;
	tsf_render_float(f, buffer, samples, 0);
	f->sendBuffer = TSF_NULL;
}

TSFDEF void tsf_render_float(tsf* f, float* buffer, int samples, int flag_mixing)
{
	struct tsf_voice *v = f->voices, *vEnd = v + f->voiceNum;
	if (!flag_mixing) TSF_MEMSET(buffer, 0, (f->outputmode == TSF_MONO ? 1 : 2) * sizeof(float) * samples);
	for (; v != vEnd; v++)
		if (v->playingPreset != -1)
			tsf_voice_render(f, v, buffer, samples, f->sendBuffer, f->sendStride);
}

TSFDEF void tsf_render_float_sends_range(tsf* f, float* buffer, float* sends, int samples, int first, int last) // VDAC4
{
	struct tsf_voice *v, *vEnd;
	if (first < 0) first = 0;
	if (last > f->voiceNum) last = f->voiceNum;
	TSF_MEMSET(buffer, 0, (f->outputmode == TSF_MONO ? 1 : 2) * sizeof(float) * samples);
	for (v = f->voices + first, vEnd = f->voices + last; v < vEnd; v++)
		if (v->playingPreset != -1)
			tsf_voice_render(f, v, buffer, samples, sends, samples);
}

TSFDEF int tsf_voice_split(tsf* f, int count) // VDAC4
{
	int index = 0;
	for (; index < f->voiceNum && count > 0; index++)
		if (f->voices[index].playingPreset != -1) count--;
	return index;
}

TSFDEF int tsf_voice_slots(tsf* f) { return f->voiceNum; } // VDAC4

TSFDEF void tsf_set_voice_cap(tsf* f, int cap) { f->voiceCap = (cap > 0 ? cap : 0); } // VDAC4


TSFDEF int tsf_voices_trim(tsf* f) // VDAC4
{
	struct tsf_voice *v, *vEnd = f->voices + f->voiceNum, *best;
	int count, ended = 0;
	if (f->voiceCap <= 0) return 0;
	count = tsf_voices_counted(f);
	while (count > f->voiceCap)
	{
		// Самый тихий (огибающая с громкостью ноты): сначала из отпущенных, потом под
		// педалью, потом прошедшие атаку.
		int pass;
		float bestLoud = 0.0f;
		best = TSF_NULL;
		for (pass = 0; pass < 3 && !best; pass++)
			for (v = f->voices; v != vEnd; v++)
			{
				float loud;
				if (v->playingPreset == -1 || tsf_voice_quick_ending(v)) continue;
				if (pass == 0 && v->ampenv.segment != TSF_SEGMENT_RELEASE) continue;
				if (pass == 1 && !tsf_voice_pedal_held(v)) continue;
				if (pass == 2 && !(v->ampenv.segment >= TSF_SEGMENT_HOLD && v->ampenv.segment < TSF_SEGMENT_RELEASE)) continue;
				loud = tsf_voice_loudness(v);
				if (!best || loud < bestLoud) best = v, bestLoud = loud;
			}
		if (!best) break;
		tsf_voice_endquick(f, best);
		count--, ended++;
	}
	return ended;
}

TSFDEF int tsf_voices_sounding(tsf* f) { return tsf_voices_counted(f); } // VDAC4

TSFDEF unsigned int tsf_voices_refused(tsf* f) { return f->voicesRefused; } // VDAC4

TSFDEF void tsf_voices_kill(tsf* f) // VDAC4
{
	struct tsf_voice *v = f->voices, *vEnd = v + f->voiceNum;
	for (; v != vEnd; v++) tsf_voice_kill(v);
}

static void tsf_channel_setup_voice(tsf* f, struct tsf_voice* v)
{
	// VDAC4: голос канала — модуляторы и высота (tsf_note_on делает это сам; для API).
	struct tsf_channel* c = &f->channels->channels[f->channels->activeChannel];
	v->playingChannel = f->channels->activeChannel;
	tsf_voice_update(f, v, TSF_NULL);
	tsf_voice_calcpitchratio(v, tsf_channel_pitchshift(c), f->outSampleRate);
}

// VDAC4: голоса канала — модуляторы заново (сменился их источник: контроллер, давление;
// громкость или панорама API); настройка модуляторами сменилась — и высота.
static void tsf_channel_update_voices(tsf* f, int channel)
{
	struct tsf_voice *v, *vEnd;
	struct tsf_channel* c = &f->channels->channels[channel];
	for (v = f->voices, vEnd = v + f->voiceNum; v != vEnd; v++)
		if (v->playingPreset != -1 && v->playingChannel == channel)
		{
			float tune = v->modTuneCents;
			tsf_voice_update(f, v, TSF_NULL);
			if (v->modTuneCents != tune) tsf_voice_calcpitchratio(v, tsf_channel_pitchshift(c), f->outSampleRate);
		}
}

static struct tsf_channel* tsf_channel_init(tsf* f, int channel)
{
	int i;
	if (f->channels && channel < f->channels->channelNum) return &f->channels->channels[channel];
	if (!f->channels)
	{
		f->channels = (struct tsf_channels*)TSF_MALLOC(sizeof(struct tsf_channels) + sizeof(struct tsf_channel) * channel);
		if (!f->channels) return TSF_NULL;
		f->channels->setupVoice = &tsf_channel_setup_voice;
		f->channels->channelNum = 0;
		f->channels->activeChannel = 0;
	}
	else
	{
		struct tsf_channels *newChannels = (struct tsf_channels*)TSF_REALLOC(f->channels, sizeof(struct tsf_channels) + sizeof(struct tsf_channel) * channel);
		if (!newChannels) return TSF_NULL;
		f->channels = newChannels;
	}
	i = f->channels->channelNum;
	f->channels->channelNum = channel + 1;
	for (; i <= channel; i++)
	{
		struct tsf_channel* c = &f->channels->channels[i];
		int cc;
		c->presetIndex = c->bank = 0;
		c->pitchWheel = c->midiPan = 8192;
		c->midiVolume = 100 << 7; // VDAC4: GM — громкость 100
		c->midiExpression = 16383;
		c->midiRPN = 0xFFFF;
		c->midiData = c->sustain = 0;
		c->panOffset = 0.0f;
		c->gainDB = 0.0f;
		c->pitchRange = 2.0f;
		c->tuning = 0.0f;
		c->delaySend = 0.0f; // VDAC4
		c->portaLastKey = -1; c->portamento = 0; c->portaTime = 0;
		c->bankMSB = c->bankLSB = 0; c->tuningCoarse = 0; c->tuningFine = 0.0f; // VDAC4
		for (cc = 0; cc < 128; cc++) c->cc[cc] = tsf_cc_initial(cc); // VDAC4
		c->channelPressure = c->sostenuto = c->mono = c->nrpnSelected = 0;
		c->midiNRPN = 0x3FFF;
		c->modRangeCents = 50.0f;
		c->masterTuning = 0.0f;
	}
	return &f->channels->channels[channel];
}

static void tsf_channel_applypitch(tsf* f, int channel, struct tsf_channel* c)
{
	struct tsf_voice *v, *vEnd;
	float pitchShift = tsf_channel_pitchshift(c);
	for (v = f->voices, vEnd = v + f->voiceNum; v != vEnd; v++)
		if (v->playingPreset != -1 && v->playingChannel == channel)
			tsf_voice_calcpitchratio(v, pitchShift, f->outSampleRate);
}

TSFDEF int tsf_channel_set_presetindex(tsf* f, int channel, int preset_index)
{
	struct tsf_channel *c = tsf_channel_init(f, channel);
	if (!c) return 0;
	c->presetIndex = (unsigned short)preset_index;
	return 1;
}

TSFDEF int tsf_channel_set_presetnumber(tsf* f, int channel, int preset_number, int flag_mididrums)
{
	// VDAC4: нет пресета — тишина на канале (как у FluidSynth), а не прежний инструмент; у
	// ударных — только наборы ударных (банк 128 с этим номером, затем стандартный набор),
	// мелодического подбора больше нет. Возвращает 0, если пресета нет.
	int preset_index;
	struct tsf_channel *c = tsf_channel_init(f, channel);
	if (!c) return 0;
	if (flag_mididrums)
	{
		preset_index = tsf_get_presetindex(f, 128 | (c->bank & 0x7FFF), preset_number);
		if (preset_index == -1) preset_index = tsf_get_presetindex(f, 128, preset_number);
		if (preset_index == -1) preset_index = tsf_get_presetindex(f, 128, 0);
	}
	else
	{
		preset_index = tsf_get_presetindex(f, (c->bank & 0x7FFF), preset_number);
		if (preset_index == -1) preset_index = tsf_get_presetindex(f, 0, preset_number);
	}
	c->presetIndex = (unsigned short)(preset_index != -1 ? preset_index : 0xFFFF);
	return preset_index != -1;
}

TSFDEF int tsf_channel_set_bank(tsf* f, int channel, int bank)
{
	struct tsf_channel *c = tsf_channel_init(f, channel);
	if (!c) return 0;
	c->bank = (unsigned short)bank;
	return 1;
}

TSFDEF int tsf_channel_set_bank_preset(tsf* f, int channel, int bank, int preset_number)
{
	int preset_index;
	struct tsf_channel *c = tsf_channel_init(f, channel);
	if (!c) return 0;
	preset_index = tsf_get_presetindex(f, bank, preset_number);
	if (preset_index == -1) return 0;
	c->presetIndex = (unsigned short)preset_index;
	c->bank = (unsigned short)bank;
	return 1;
}

TSFDEF int tsf_channel_set_pan(tsf* f, int channel, float pan)
{
	// VDAC4: сдвиг панорамы API (CC10 — модулятор SF2), к нему — модуляторы голоса.
	struct tsf_channel *c = tsf_channel_init(f, channel);
	if (!c) return 0;
	c->panOffset = pan - 0.5f;
	tsf_channel_update_voices(f, channel);
	return 1;
}

TSFDEF int tsf_channel_set_volume(tsf* f, int channel, float volume)
{
	// VDAC4: громкость API (CC7 и CC11 — модуляторы SF2), к ней — модуляторы голоса.
	float gainDB = tsf_gainToDecibels(volume);
	struct tsf_channel *c = tsf_channel_init(f, channel);
	if (!c) return 0;
	if (gainDB == c->gainDB) return 1;
	c->gainDB = gainDB;
	tsf_channel_update_voices(f, channel);
	return 1;
}

TSFDEF int tsf_channel_set_pitchwheel(tsf* f, int channel, int pitch_wheel)
{
	struct tsf_channel *c = tsf_channel_init(f, channel);
	if (!c) return 0;
	if (c->pitchWheel == pitch_wheel) return 1;
	c->pitchWheel = (unsigned short)pitch_wheel;
	tsf_channel_applypitch(f, channel, c);
	return 1;
}

TSFDEF int tsf_channel_set_pitchrange(tsf* f, int channel, float pitch_range)
{
	struct tsf_channel *c = tsf_channel_init(f, channel);
	if (!c) return 0;
	if (c->pitchRange == pitch_range) return 1;
	c->pitchRange = pitch_range;
	if (c->pitchWheel != 8192) tsf_channel_applypitch(f, channel, c);
	return 1;
}

TSFDEF int tsf_channel_set_tuning(tsf* f, int channel, float tuning)
{
	struct tsf_channel *c = tsf_channel_init(f, channel);
	if (!c) return 0;
	if (c->tuning == tuning) return 1;
	c->tuning = tuning;
	tsf_channel_applypitch(f, channel, c);
	return 1;
}

TSFDEF int tsf_channel_set_master_tuning(tsf* f, int channel, float semitones) // VDAC4
{
	struct tsf_channel *c = tsf_channel_init(f, channel);
	if (!c) return 0;
	if (c->masterTuning == semitones) return 1;
	c->masterTuning = semitones;
	tsf_channel_applypitch(f, channel, c);
	return 1;
}

TSFDEF int tsf_channel_set_pressure(tsf* f, int channel, int pressure) // VDAC4
{
	struct tsf_channel *c = tsf_channel_init(f, channel);
	if (!c) return 0;
	c->channelPressure = (unsigned char)(pressure < 0 ? 0 : (pressure > 127 ? 127 : pressure));
	tsf_channel_update_voices(f, channel);
	return 1;
}

TSFDEF int tsf_channel_set_key_pressure(tsf* f, int channel, int key, int pressure) // VDAC4
{
	struct tsf_voice *v, *vEnd;
	struct tsf_channel *c = tsf_channel_init(f, channel);
	if (!c) return 0;
	for (v = f->voices, vEnd = v + f->voiceNum; v != vEnd; v++)
		if (v->playingPreset != -1 && v->playingChannel == channel && v->playingKey == key)
		{
			float tune = v->modTuneCents;
			v->polyPressure = (unsigned char)(pressure < 0 ? 0 : (pressure > 127 ? 127 : pressure));
			tsf_voice_update(f, v, TSF_NULL);
			if (v->modTuneCents != tune) tsf_voice_calcpitchratio(v, tsf_channel_pitchshift(c), f->outSampleRate);
		}
	return 1;
}

TSFDEF int tsf_channel_set_sustain(tsf* f, int channel, int flag_sustain)
{
	struct tsf_channel *c = tsf_channel_init(f, channel);
	if (!c) return 0;
	if (!c->sustain == !flag_sustain) return 1;
	c->sustain = (unsigned short)(flag_sustain != 0);
	//Turning on sustain does no action now, just starts note_off behaving differently
	if (flag_sustain) return 1;
	//Turning off sustain, actually end voices that got a note_off and were set to heldSustain status
	struct tsf_voice *v = f->voices, *vEnd = v + f->voiceNum;
	for (; v != vEnd; v++)
		if (v->playingPreset != -1 && v->playingChannel == channel && v->ampenv.segment < TSF_SEGMENT_RELEASE && v->heldSustain)
			tsf_voice_end(f, v);
	return 1;
}

// VDAC4: отпустить клавишу голоса: держит sostenuto (голос был взят при нажатой педали) или
// sustain — голос ждёт педаль, иначе — отпускание.
static void tsf_channel_key_up(tsf* f, struct tsf_channel* c, struct tsf_voice* v)
{
	if (c->sostenuto && v->sostenutoLatched) v->heldSostenuto = 1;
	else if (c->sustain) v->heldSustain = 1;
	else tsf_voice_end(f, v);
}

TSFDEF int tsf_channel_note_on(tsf* f, int channel, int key, float vel)
{
	struct tsf_channel* c; int fromKey, result; unsigned int playIndex; // VDAC4
	if (!f->channels || channel >= f->channels->channelNum) return 1;
	f->channels->activeChannel = channel;
	if (!vel)
	{
		tsf_channel_note_off(f, channel, key);
		return 1;
	}
	c = &f->channels->channels[channel];
	// VDAC4: одноголосный режим (CC126): прежняя нота канала гаснет за 10 мс.
	if (c->mono)
	{
		struct tsf_voice *v = f->voices, *vEnd = v + f->voiceNum;
		for (; v != vEnd; v++)
			if (v->playingPreset != -1 && v->playingChannel == channel && v->ampenv.segment < TSF_SEGMENT_RELEASE)
				tsf_voice_endquick(f, v);
	}
	fromKey = c->portaLastKey;
	c->portaLastKey = (short)key;
	playIndex = f->voicePlayIndex;
	result = tsf_note_on(f, c->presetIndex, key, vel);
	// VDAC4: portamento — the note's voices start at the previous key's pitch and glide
	// at a constant rate: CC5 0..127 -> 10 ms .. 2.5 s per octave.
	if (c->portamento && c->portaTime && fromKey >= 0 && fromKey != key)
	{
		struct tsf_voice *v = f->voices, *vEnd = v + f->voiceNum;
		float cents = (float)(fromKey - key) * 100.0f;
		float secondsPerOctave = 0.01f * TSF_POWF(2.0f, c->portaTime / 16.0f);
		float step = 1200.0f / (secondsPerOctave * f->outSampleRate);
		for (; v != vEnd; v++)
			if (v->playingPreset != -1 && v->playIndex == playIndex && v->playingChannel == channel)
				v->portaCents = cents, v->portaStep = step;
	}
	return result;
}

TSFDEF void tsf_channel_note_off(tsf* f, int channel, int key)
{
	struct tsf_voice *v = f->voices, *vEnd = v + f->voiceNum, *vMatchFirst = TSF_NULL, *vMatchLast = TSF_NULL;
	struct tsf_channel* c;
	if (!f->channels || channel >= f->channels->channelNum) return; // VDAC4
	c = &f->channels->channels[channel];
	for (; v != vEnd; v++)
	{
		//Find the first and last entry in the voices list with matching channel, key and look up the smallest play index
		if (v->playingPreset == -1 || v->playingChannel != channel || v->playingKey != key || v->ampenv.segment >= TSF_SEGMENT_RELEASE || v->heldSustain || v->heldSostenuto) continue;
		else if (!vMatchFirst || v->playIndex < vMatchFirst->playIndex) vMatchFirst = vMatchLast = v;
		else if (v->playIndex == vMatchFirst->playIndex) vMatchLast = v;
	}
	if (!vMatchFirst) return;
	for (v = vMatchFirst; v <= vMatchLast; v++)
	{
		//Stop all voices with matching channel, key and the smallest play index which was enumerated above
		if (v != vMatchFirst && v != vMatchLast &&
			(v->playIndex != vMatchFirst->playIndex || v->playingPreset == -1 || v->playingChannel != channel || v->playingKey != key || v->ampenv.segment >= TSF_SEGMENT_RELEASE)) continue;
		//Don't turn off if sustain is active, just mark as held by sustain so we don't forget it
		tsf_channel_key_up(f, c, v); // VDAC4: и sostenuto
	}
}

TSFDEF void tsf_channel_note_off_all(tsf* f, int channel)
{
	//Ignore sustain channel settings, note_off_all overrides
	struct tsf_voice *v = f->voices, *vEnd = v + f->voiceNum;
	for (; v != vEnd; v++)
		if (v->playingPreset != -1 && v->playingChannel == channel && v->ampenv.segment < TSF_SEGMENT_RELEASE)
			tsf_voice_end(f, v);
}

TSFDEF void tsf_channel_sounds_off_all(tsf* f, int channel)
{
	struct tsf_voice *v = f->voices, *vEnd = v + f->voiceNum;
	for (; v != vEnd; v++)
		if (v->playingPreset != -1 && v->playingChannel == channel && (v->ampenv.segment < TSF_SEGMENT_RELEASE || v->ampenv.parameters.release))
			tsf_voice_endquick(f, v);
}

// VDAC4: NRPN GS/XG (MSB 1) — в звуковые контроллеры GM2, как у SpessaSynth: вибрато —
// CC76/77/78, срез — CC74, резонанс — CC71, атака — CC73, спад — CC75, отпускание — CC72.
static void tsf_channel_nrpn(tsf* f, int channel, struct tsf_channel* c, int value)
{
	int cc;
	if ((c->midiNRPN >> 7) != 1) return;
	switch (c->midiNRPN & 0x7F)
	{
		case 0x08: cc = 76; break; case 0x09: cc = 77; break; case 0x0A: cc = 78; break;
		case 0x20: cc = 74; break; case 0x21: cc = 71; break;
		case 0x63: cc = 73; break; case 0x64: cc = 75; break; case 0x66: cc = 72; break;
		default: return;
	}
	tsf_channel_midi_control(f, channel, cc, value);
}

TSFDEF int tsf_channel_midi_control(tsf* f, int channel, int controller, int control_value)
{
	struct tsf_channel* c = tsf_channel_init(f, channel);
	if (!c) return 0;
	if (controller < 0 || controller > 127) return 1;
	control_value &= 0x7F;
	c->cc[controller] = (unsigned char)control_value; // VDAC4: источник модуляторов SF2
	switch (controller)
	{
		// VDAC4: громкость, экспрессия, панорама — модуляторы SF2 (CC7, CC11 → затухание по
		// вогнутой кривой, CC10 → панорама); младшие байты в модуляторы не идут.
		case   7 /*VOLUME_MSB*/      : c->midiVolume     = (unsigned short)((c->midiVolume     & 0x7F  ) | (control_value << 7)); break;
		case  39 /*VOLUME_LSB*/      : c->midiVolume     = (unsigned short)((c->midiVolume     & 0x3F80) |  control_value);       return 1;
		case  11 /*EXPRESSION_MSB*/  : c->midiExpression = (unsigned short)((c->midiExpression & 0x7F  ) | (control_value << 7)); break;
		case  43 /*EXPRESSION_LSB*/  : c->midiExpression = (unsigned short)((c->midiExpression & 0x3F80) |  control_value);       return 1;
		case  10 /*PAN_MSB*/         : c->midiPan        = (unsigned short)((c->midiPan        & 0x7F  ) | (control_value << 7)); break;
		case  42 /*PAN_LSB*/         : c->midiPan        = (unsigned short)((c->midiPan        & 0x3F80) |  control_value);       return 1;
		case   6 /*DATA_ENTRY_MSB*/  : c->midiData       = (unsigned short)((c->midiData       & 0x7F)   | (control_value << 7)); goto TCMC_SET_DATA;
		case  38 /*DATA_ENTRY_LSB*/  : c->midiData       = (unsigned short)((c->midiData       & 0x3F80) |  control_value);       goto TCMC_SET_DATA;
		// VDAC4: Data Increment/Decrement — на единицу младшего байта (у диапазона колеса — на
		// цент: младший байт — центы 0..99; у грубой настройки и NRPN — на единицу старшего).
		case  96 /*DATA_INCREMENT*/  :
		case  97 /*DATA_DECREMENT*/  :
		{
			int data = c->midiData, step = (controller == 96 ? 1 : -1);
			if (c->nrpnSelected || c->midiRPN == 2) data += step << 7;
			else if (c->midiRPN == 0)
			{
				int cents = (data >> 7) * 100 + (data & 0x7F) + step;
				if (cents < 0) cents = 0;
				data = ((cents / 100) << 7) | (cents % 100);
			}
			else if (c->midiRPN <= 5) data += step;
			else return 1;
			c->midiData = (unsigned short)(data < 0 ? 0 : (data > 16383 ? 16383 : data));
			controller = 6;
			goto TCMC_SET_DATA;
		}
		// VDAC4: MSB и LSB хранятся порознь, банк — MSB, а без него — LSB (вариации XG). Прежде
		// второй CC32 терял MSB (CC0=1, CC32=2, CC32=3 давали 130, затем 3), а CC0 с CC32 у
		// файла GS давали MSB*128+LSB — банк, которого нет, и вариация пропадала.
		case   0 /*BANK_SELECT_MSB*/ : c->bankMSB = (unsigned char)control_value; c->bank = (unsigned short)(c->bankMSB ? c->bankMSB : c->bankLSB); return 1;
		case  32 /*BANK_SELECT_LSB*/ : c->bankLSB = (unsigned char)control_value; c->bank = (unsigned short)(c->bankMSB ? c->bankMSB : c->bankLSB); return 1;
		case 101 /*RPN_MSB*/         : c->midiRPN = (unsigned short)(((c->midiRPN == 0xFFFF ? 0 : c->midiRPN) & 0x7F  ) | (control_value << 7)); c->nrpnSelected = 0; return 1;
		case 100 /*RPN_LSB*/         : c->midiRPN = (unsigned short)(((c->midiRPN == 0xFFFF ? 0 : c->midiRPN) & 0x3F80) |  control_value); c->nrpnSelected = 0; return 1;
		// VDAC4: NRPN — свой номер; данные идут в него (tsf_channel_nrpn), а не в RPN.
		case  99 /*NRPN_MSB*/        : c->midiNRPN = (unsigned short)((c->midiNRPN & 0x7F) | (control_value << 7)); c->nrpnSelected = 1; return 1;
		case  98 /*NRPN_LSB*/        : c->midiNRPN = (unsigned short)((c->midiNRPN & 0x3F80) | control_value); c->nrpnSelected = 1; return 1;
		case  64 /*SUSTAIN*/         : tsf_channel_set_sustain(f, channel, (int)(control_value >= 64)); break;
		// VDAC4: sostenuto — держит ноты, клавиши которых нажаты в миг нажатия педали.
		case  66 /*SOSTENUTO*/       :
		{
			struct tsf_voice *v = f->voices, *vEnd = v + f->voiceNum;
			if (control_value >= 64 && !c->sostenuto)
			{
				c->sostenuto = 1;
				for (; v != vEnd; v++)
					if (v->playingPreset != -1 && v->playingChannel == channel && v->ampenv.segment < TSF_SEGMENT_RELEASE && !v->heldSustain && !v->heldSostenuto)
						v->sostenutoLatched = 1;
			}
			else if (control_value < 64 && c->sostenuto)
			{
				c->sostenuto = 0;
				for (; v != vEnd; v++)
					if (v->playingPreset != -1 && v->playingChannel == channel)
					{
						v->sostenutoLatched = 0;
						if (v->heldSostenuto)
						{
							v->heldSostenuto = 0;
							if (c->sustain) v->heldSustain = 1;
							else tsf_voice_end(f, v);
						}
					}
			}
			break;
		}
		case   5 /*PORTAMENTO_TIME*/ : c->portaTime = (unsigned char)control_value; return 1;
		case  65 /*PORTAMENTO*/      : c->portamento = (unsigned char)(control_value >= 64); break;
		case  84 /*PORTAMENTO_CTRL*/ : c->portaLastKey = (short)control_value; return 1;
		case  94 /*DELAY_SEND*/      : c->delaySend = control_value / 127.0f; return 1; // VDAC4: эхо — у канала
		case 120 /*ALL_SOUND_OFF*/   : tsf_channel_sounds_off_all(f, channel); return 1;
		// VDAC4: All Notes Off (и режимы 124..127, которые его включают) — как отпускание всех
		// клавиш: педали держат ноты дальше. 126 — одноголосный режим, 127 — многоголосный.
		case 123 /*ALL_NOTES_OFF*/   :
		case 124: case 125: case 126: case 127:
		{
			struct tsf_voice *v = f->voices, *vEnd = v + f->voiceNum;
			for (; v != vEnd; v++)
				if (v->playingPreset != -1 && v->playingChannel == channel && v->ampenv.segment < TSF_SEGMENT_RELEASE && !v->heldSustain && !v->heldSostenuto)
					tsf_channel_key_up(f, c, v);
			if (controller == 126) c->mono = 1;
			else if (controller == 127) c->mono = 0;
			return 1;
		}
		case 121 /*ALL_CTRL_OFF*/    :
		{
			// VDAC4: сброс по RP-015 — колесо модуляции, экспрессия, педали (64–67), колесо
			// высоты, давление на канал и клавиши, выбор RPN/NRPN; громкость, панорама, банк,
			// посылы, звуковые контроллеры и значения RPN (диапазон колеса, настройка)
			// остаются. Педали снимаются — удержанные ими ноты гаснут.
			struct tsf_voice *v = f->voices, *vEnd = v + f->voiceNum;
			c->cc[1] = 0; c->cc[11] = 127; c->midiExpression = 16383;
			c->cc[65] = 0; c->portamento = 0; c->cc[67] = 0;
			c->midiRPN = 0xFFFF; c->midiNRPN = 0x3FFF; c->nrpnSelected = 0;
			c->midiData = 0;
			c->channelPressure = 0;
			for (; v != vEnd; v++) if (v->playingPreset != -1 && v->playingChannel == channel) v->polyPressure = 0;
			tsf_channel_set_sustain(f, channel, 0); c->cc[64] = 0;
			tsf_channel_midi_control(f, channel, 66, 0);
			tsf_channel_set_pitchwheel(f, channel, 8192);
			break;
		}
		default: break; // VDAC4: прочие контроллеры — источники модуляторов
	}
	tsf_channel_update_voices(f, channel); // VDAC4
	return 1;
TCMC_SET_DATA:
	// VDAC4: точная (RPN 1) и грубая (RPN 2) настройка хранятся порознь: прежде целая часть
	// суммы считалась грубой, и грубая +1 с точной −0,5 давали +0,5, а повтор той же точной
	// — уже −0,5 (скачок на полутон). RPN 5 — глубина вибрато колесом модуляции.
	if (c->nrpnSelected) { if (controller == 6) tsf_channel_nrpn(f, channel, c, c->midiData >> 7); }
	else if (c->midiRPN == 0) tsf_channel_set_pitchrange(f, channel, (c->midiData >> 7) + 0.01f * (c->midiData & 0x7F));
	else if (c->midiRPN == 1) { c->tuningFine = ((float)c->midiData - 8192.0f) / 8192.0f; tsf_channel_set_tuning(f, channel, c->tuningCoarse + c->tuningFine); } //fine tune
	else if (c->midiRPN == 2 && controller == 6) { c->tuningCoarse = (signed char)((c->midiData >> 7) - 64); tsf_channel_set_tuning(f, channel, c->tuningCoarse + c->tuningFine); } //coarse tune
	else if (c->midiRPN == 5) { c->modRangeCents = c->midiData / 1.28f; tsf_channel_update_voices(f, channel); }
	return 1;
}

TSFDEF int tsf_channel_get_preset_index(tsf* f, int channel)
{
	return (f->channels && channel < f->channels->channelNum ? f->channels->channels[channel].presetIndex : 0);
}

TSFDEF int tsf_channel_get_preset_bank(tsf* f, int channel)
{
	return (f->channels && channel < f->channels->channelNum ? (f->channels->channels[channel].bank & 0x7FFF) : 0);
}

TSFDEF int tsf_channel_get_preset_number(tsf* f, int channel)
{
	return (f->channels && channel < f->channels->channelNum && f->channels->channels[channel].presetIndex < f->presetNum ? f->presets[f->channels->channels[channel].presetIndex].preset : 0); // VDAC4: пресета может не быть
}

TSFDEF float tsf_channel_get_pan(tsf* f, int channel)
{
	return (f->channels && channel < f->channels->channelNum ? f->channels->channels[channel].panOffset - 0.5f : 0.5f);
}

TSFDEF float tsf_channel_get_volume(tsf* f, int channel)
{
	return (f->channels && channel < f->channels->channelNum ? tsf_decibelsToGain(f->channels->channels[channel].gainDB) : 1.0f);
}

TSFDEF int tsf_channel_get_pitchwheel(tsf* f, int channel)
{
	return (f->channels && channel < f->channels->channelNum ? f->channels->channels[channel].pitchWheel : 8192);
}

TSFDEF float tsf_channel_get_pitchrange(tsf* f, int channel)
{
	return (f->channels && channel < f->channels->channelNum ? f->channels->channels[channel].pitchRange : 2.0f);
}

TSFDEF float tsf_channel_get_tuning(tsf* f, int channel)
{
	return (f->channels && channel < f->channels->channelNum ? f->channels->channels[channel].tuning : 0.0f);
}

#ifdef __cplusplus
}
#endif

#endif //TSF_IMPLEMENTATION
