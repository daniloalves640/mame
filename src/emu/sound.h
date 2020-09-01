// license:BSD-3-Clause
// copyright-holders:Aaron Giles
/***************************************************************************

    sound.h

    Core sound interface functions and definitions.

****************************************************************************

	In MAME, sound is represented as a graph of sound "streams". Each
	stream has a fixed number of inputs and outputs, and is responsible
	for producing sound on demand.

	The graph is driven from the outputs, which are speaker devices.
	These devices are updated on a regular basis (~50 times per second),
	and when an update occurs, the graph is walked from the speaker
	through each input, until all connected streams are up to date.

	Individual streams can also be updated manually. This is important
	for sound chips and CPU-driven devices, who should force any
	affected streams to update prior to making changes.

	Sound streams are *not* part of the device execution model. This is
	very important to understand. If the process of producing the ouput
	stream affects state that might be consumed by an executing device
	(e.g., a CPU), then care must be taken to ensure that the stream is
	updated frequently enough

	The model for timing sound samples is very important and explained
	here. Each stream source has a clock (aka sample rate). Each clock
	edge represents a sample that is held for the duration of one clock
	period. This model has interesting effects:

	For example, if you have a 10Hz clock, and call stream.update() at
	t=0.91, it will compute 10 samples (for clock edges 0.0, 0.1, 0.2,
	..., 0.7, 0.8, and 0.9). And then if you ask the stream what its
	current end time is (via stream.sample_time()), it will say t=1.0,
	which is in the future, because it knows it will hold that last
	sample until 1.0s.

***************************************************************************/

#pragma once

#ifndef __EMU_H__
#error Dont include this file directly; include emu.h instead.
#endif

#ifndef MAME_EMU_SOUND_H
#define MAME_EMU_SOUND_H


//**************************************************************************
//  CONSTANTS
//**************************************************************************

// special sample-rate values
constexpr u32 SAMPLE_RATE_INVALID = 0xffffffff;
constexpr u32 SAMPLE_RATE_INPUT_ADAPTIVE = 0xfffffffe;
constexpr u32 SAMPLE_RATE_OUTPUT_ADAPTIVE = 0xfffffffd;
constexpr u32 SAMPLE_RATE_SYNCHRONOUS = 0xfffffffc;



//**************************************************************************
//  DEBUGGING
//**************************************************************************

// turn this on to enable aggressive assertions and other checks
#define SOUND_DEBUG (1)

// if SOUND_DEBUG is on, make assertions fire regardless of MAME_DEBUG
#if (SOUND_DEBUG)
#define sound_assert(x) do { if (!(x)) { osd_printf_error("sound_assert: " #x "\n"); __debugbreak(); } } while (0)
#else
#define sound_assert assert
#endif



//**************************************************************************
//  TYPE DEFINITIONS
//**************************************************************************

// ======================> stream_buffer

class stream_buffer
{
	friend class read_stream_view;

public:
	using sample_t = float;

	// constructor/destructor
	stream_buffer(u32 sample_rate = 48000);
	~stream_buffer();

	// disable copying of stream_buffers directly
	stream_buffer(stream_buffer const &src) = delete;
	stream_buffer &operator=(stream_buffer const &rhs) = delete;

	// return the current sample rate
	u32 sample_rate() const { return m_sample_rate; }

	// return the current sample period in attoseconds
	attoseconds_t sample_period_attoseconds() const { return m_sample_attos; }
	attotime sample_period() const { return attotime(0, m_sample_attos); }

	// return the attotime of the current end of buffer
	attotime end_time() const { return index_time(m_end_sample); }

	// set a new sample rate
	void set_sample_rate(u32 rate);

	// set the ending time (for forced resyncs; generally not used)
	void set_end_time(attotime time)
	{
		m_end_second = time.seconds();
		m_end_sample = u32(time.attoseconds() / m_sample_attos);
	}

	// read the sample at the given index (clamped); should be valid in all cases
	sample_t get(s32 index) const
	{
		sample_t value = m_buffer[clamp_index(index)];
#if (SOUND_DEBUG)
		sound_assert(!std::isnan(value));
#endif
		return value;
	}

	// write the sample at the given index (clamped)
	void put(s32 index, sample_t data)
	{
		m_buffer[clamp_index(index)] = data;
	}

#if (SOUND_DEBUG)
	// for debugging, provide an interface to write a WAV stream
	void open_wav(char const *filename);
	void flush_wav();
	void close_wav();
#endif

private:
	// clamp an index to the size of the buffer; allows for indexing +/- one
	// buffers' worth of range
	u32 clamp_index(s32 index) const
	{
		if (index < 0)
			index += m_sample_rate;
		else if (index >= m_sample_rate)
			index -= m_sample_rate;
		sound_assert(index >= 0 && index < m_sample_rate);
		return index;
	}

	// return the attotime of a given index within the buffer
	attotime index_time(s32 index) const;

	// given an attotime, return the buffer index corresponding to it
	u32 time_to_buffer_index(attotime time, bool round_up = false);

	// downsample from our buffer into a temporary buffer
	void backfill_downsample(sample_t *dest, int samples, attotime newend, attotime newperiod);

	// upsample from a temporary buffer into our buffer
	void backfill_upsample(sample_t const *src, int samples, attotime prevend, attotime prevperiod);

	// internal state
	u32 m_end_second;                     // current full second of the buffer end
	u32 m_end_sample;                     // current sample number within the final second
	u32 m_sample_rate;                    // sample rate of the data in teh buffer
	attoseconds_t m_sample_attos;         // pre-computed attoseconds per sample
	std::vector<sample_t> m_buffer;       // vector of actual buffer data

#if (SOUND_DEBUG)
	// internal debugging state
	wav_file *m_wav_file = nullptr;       // pointer to the current WAV file
	u32 m_last_written = 0;               // last written sample index
#endif
};


// ======================> read_stream_view

class read_stream_view
{
	friend class write_stream_view;

public:
	using sample_t = stream_buffer::sample_t;

	// empty constructor so we can live in an array or vector
	read_stream_view() :
		m_buffer(nullptr),
		m_start(0),
		m_end(0),
		m_gain(1.0)
	{
	}

	// constructor that covers the given sample range of a buffer
	read_stream_view(stream_buffer &buffer, s32 start, s32 end, sample_t gain) :
		m_buffer(&buffer),
		m_start(start),
		m_end(end),
		m_gain(gain)
	{
		normalize_start_end();
	}

	// constructor that covers the given time period
	read_stream_view(stream_buffer &buffer, attotime start, attotime end, sample_t gain) :
		m_buffer(&buffer),
		m_start(0),
		m_end(buffer.time_to_buffer_index(end, true)),
		m_gain(gain)
	{
		// it's important to compute the end first, since it could invalidate the start
		m_start = buffer.time_to_buffer_index(start, false);
		normalize_start_end();
	}

	// copy constructor
	read_stream_view(read_stream_view const &src) :
		m_buffer(src.m_buffer),
		m_start(src.m_start),
		m_end(src.m_end),
		m_gain(src.m_gain)
	{
		normalize_start_end();
	}

	// copy constructor that sets a different start time
	read_stream_view(read_stream_view const &src, attotime start) :
		m_buffer(src.m_buffer),
		m_start(0),
		m_end(src.m_end),
		m_gain(src.m_gain)
	{
		// end must be set before start can be
		m_start = m_buffer->time_to_buffer_index(start, false);
		normalize_start_end();
	}

	// copy assignment
	read_stream_view &operator=(read_stream_view const &rhs)
	{
		m_buffer = rhs.m_buffer;
		m_start = rhs.m_start;
		m_end = rhs.m_end;
		m_gain = rhs.m_gain;
		normalize_start_end();
		return *this;
	}

	// return the local gain
	sample_t gain() const { return m_gain; }

	// return the sample rate of the data
	u32 sample_rate() const { return m_buffer->sample_rate(); }

	// return the sample period (in attoseconds) of the data
	attoseconds_t sample_period_attoseconds() const { return m_buffer->sample_period_attoseconds(); }
	attotime sample_period() const { return m_buffer->sample_period(); }

	// return the number of samples represented by the buffer
	u32 samples() const { return m_end - m_start; }

	// return the starting or ending time of the buffer
	attotime start_time() const { return m_buffer->index_time(m_start); }
	attotime end_time() const { return m_buffer->index_time(m_end); }

	// set the gain
	read_stream_view &set_gain(float gain) { m_gain = gain; return *this; }

	// apply an additional gain factor
	read_stream_view &apply_gain(float gain) { m_gain *= gain; return *this; }

	// safely fetch a gain-scaled sample from the buffer
	sample_t get(s32 index) const
	{
		if (u32(index) >= samples())
			return 0;
		return m_buffer->get(m_start + index) * m_gain;
	}

	// safely fetch a raw sample from the buffer; if you use this, you need to
	// apply the gain yourself for correctness
	sample_t getraw(s32 index) const
	{
		if (u32(index) >= samples())
			return 0;
		return m_buffer->get(m_start + index);
	}

protected:
	// normalize start/end
	void normalize_start_end()
	{
		// ensure that end is always greater than start; we'll
		// wrap to the buffer length as needed
		if (m_end < m_start && m_buffer != nullptr)
			m_end += m_buffer->sample_rate();
		sound_assert(m_end >= m_start);
	}

	// internal state
	stream_buffer *m_buffer;              // pointer to the stream buffer we're viewing
	s32 m_start;                          // starting sample index
	s32 m_end;                            // ending sample index (always >= start)
	sample_t m_gain;                      // overall gain factor
};


// ======================> write_stream_view

class write_stream_view : public read_stream_view
{
public:
	// empty constructor so we can live in an array or vector
	write_stream_view()
	{
	}

	// constructor that covers the given time period
	write_stream_view(stream_buffer &buffer, attotime start, attotime end) :
		read_stream_view(buffer, start, end, sample_t(1.0))
	{
	}

	// constructor that converts from a read_stream_view
	write_stream_view(read_stream_view &src) :
		read_stream_view(src)
	{
	}

	// safely write a gain-applied sample to the buffer
	void put(s32 index, sample_t sample)
	{
		sound_assert(u32(index) < samples());
		if (u32(index) < samples())
			m_buffer->put(m_start + index, sample);
	}

	// safely add a gain-applied sample to the buffer
	void add(s32 index, sample_t sample)
	{
		sound_assert(u32(index) < samples());
		if (u32(index) < samples())
			m_buffer->put(m_start + index, m_buffer->get(m_start + index) + sample);
	}

	// fill part of the view with the given value
	void fill(sample_t value, s32 start, s32 count)
	{
		if (start + count > samples())
			count = samples() - start;
		for (s32 index = 0; index < count; index++)
			put(start + index, value);
	}
	void fill(sample_t value, s32 start) { fill(value, start, samples() - start); }
	void fill(sample_t value) { fill(value, 0, samples()); }

	// copy data from another view
	void copy(read_stream_view const &src, s32 start, s32 count)
	{
		if (start + count > samples())
			count = samples() - start;
		for (s32 index = 0; index < count; index++)
			put(start + index, src.get(start + index));
	}
	void copy(read_stream_view const &src, s32 start) { copy(src, start, samples() - start); }
	void copy(read_stream_view const &src) { copy(src, 0, samples()); }

	// add data from another view to our current values
	void add(read_stream_view const &src, s32 start, s32 count)
	{
		if (start + count > samples())
			count = samples() - start;
		for (s32 index = 0; index < count; index++)
			add(start + index, src.get(start + index));
	}
	void add(read_stream_view const &src, s32 start) { add(src, start, samples() - start); }
	void add(read_stream_view const &src) { add(src, 0, samples()); }
};


// ======================> sound_stream_output

class sound_stream_output
{
#if (SOUND_DEBUG)
	friend class sound_stream;
#endif

public:
	// construction/destruction
	sound_stream_output();

	// initialization
	void init(sound_stream &stream, u32 index, char const *tag_base);

	// no copying allowed
	sound_stream_output(sound_stream_output const &src) = delete;
	sound_stream_output &operator=(sound_stream_output const &rhs) = delete;

	// simple getters
	sound_stream &stream() const { sound_assert(m_stream != nullptr); return *m_stream; }
	attotime end_time() const { return m_buffer.end_time(); }
	u32 index() const { return m_index; }
	stream_buffer::sample_t gain() const { return m_gain; }

	// simple setters
	void set_gain(float gain) { m_gain = gain; }

	// return a friendly name
	std::string name() const;

	// handle a changing sample rate
	void sample_rate_changed(u32 rate) { m_buffer.set_sample_rate(rate); }

	// return an output view covering a time period
	write_stream_view view(attotime start, attotime end) { return write_stream_view(m_buffer, start, end); }

	// resync the buffer to the given end time
	void set_end_time(attotime end) { m_buffer.set_end_time(end); }

private:
	// internal state
	sound_stream *m_stream;               // owning stream
	stream_buffer m_buffer;               // output buffer
	u32 m_index;                          // output index within the stream
	stream_buffer::sample_t m_gain;       // gain to apply to the output
};


// ======================> sound_stream_input

class sound_stream_input
{
#if (SOUND_DEBUG)
	friend class sound_stream;
#endif

public:
	// construction/destruction
	sound_stream_input();

	// initialization
	void init(sound_stream &stream, u32 index, char const *tag_base, sound_stream_output *resampler);

	// no copying allowed
	sound_stream_input(sound_stream_input const &src) = delete;
	sound_stream_input &operator=(sound_stream_input const &rhs) = delete;

	// simple getters
	bool valid() const { return (m_native_source != nullptr); }
	sound_stream &owner() const { sound_assert(valid()); return *m_owner; }
	sound_stream_output &source() const { sound_assert(valid()); return *m_native_source; }
	u32 index() const { return m_index; }
	stream_buffer::sample_t gain() const { return m_gain; }
	stream_buffer::sample_t user_gain() const { return m_user_gain; }

	// simple setters
	void set_gain(float gain) { m_gain = gain; }
	void set_user_gain(float gain) { m_user_gain = gain; }

	// return a friendly name
	std::string name() const;

	// connect the source
	void set_source(sound_stream_output *source);

	// update and return an reading view
	read_stream_view update(attotime start, attotime end);

	// tell inputs to apply sample rate changes
	void apply_sample_rate_changes(u32 updatenum, u32 downstream_rate);

private:
	// internal state
	sound_stream *m_owner;                   // pointer to the owning stream
	sound_stream_output *m_native_source;    // pointer to the native sound_stream_output
	sound_stream_output *m_resampler_source; // pointer to the resampler output
	u32 m_index;                             // input index within the stream
	stream_buffer::sample_t m_gain;          // gain to apply to this input
	stream_buffer::sample_t m_user_gain;     // user-controlled gain to apply to this input
};


// ======================> stream_update_delegate/stream_update_ex_delegate

// old-style callback; eventually should be deprecated
using stream_update_delegate = delegate<void (sound_stream &stream, stream_sample_t **inputs, stream_sample_t **outputs, int samples)>;

// new-style callback
using stream_update_ex_delegate = delegate<void (sound_stream &stream, std::vector<read_stream_view> const &inputs, std::vector<write_stream_view> &outputs)>;


// ======================> sound_stream_flags

enum sound_stream_flags : u32
{
	//
	// specify which resampler to use for inputs:
	//
	//    STREAM_RESAMPLER_DEFAULT = classic MAME resampler
	//
	//    STREAM_RESAMPLER_NONE = no resampler; update handler must handle
	//        inputs of differing sample rates
	//
	STREAM_RESAMPLER_MASK = 0x0f,
		STREAM_RESAMPLER_DEFAULT = 0,
		STREAM_RESAMPLER_NONE = 1,

	// specify that updates should be forced to one sample at a time, in real time
	STREAM_SYNCHRONOUS = 0x10,
};


// ======================> sound_stream

class sound_stream
{
	friend class sound_manager;

public:
	// construction/destruction
	sound_stream(device_t &device, u32 inputs, u32 outputs, u32 output_base, u32 sample_rate, stream_update_delegate callback, sound_stream_flags flags = STREAM_RESAMPLER_DEFAULT);
	sound_stream(device_t &device, u32 inputs, u32 outputs, u32 output_base, u32 sample_rate, stream_update_ex_delegate callback, sound_stream_flags flags = STREAM_RESAMPLER_DEFAULT);
	virtual ~sound_stream();

	// simple getters
	sound_stream *next() const { return m_next; }
	device_t &device() const { return m_device; }
	std::string name() const { return m_name; }
	bool synchronous() const { return m_synchronous; }
	bool input_adaptive() const { return m_input_adaptive || m_synchronous; }
	bool output_adaptive() const { return m_output_adaptive; }

	// input and output getters
	u32 input_count() const { return m_input.size(); }
	u32 output_count() const { return m_output.size(); }
	u32 output_base() const { return m_output_base; }
	sound_stream_input &input(int index) { sound_assert(index >= 0 && index < m_input.size()); return m_input[index]; }
	sound_stream_output &output(int index) { sound_assert(index >= 0 && index < m_output.size()); return m_output[index]; }

	// sample rate and timing getters
	u32 sample_rate() const { return (m_pending_sample_rate != SAMPLE_RATE_INVALID) ? m_pending_sample_rate : m_sample_rate; }
	attotime sample_time() const { return m_output[0].end_time(); }
	attotime sample_period() const { return attotime(0, sample_period_attoseconds()); }
	attoseconds_t sample_period_attoseconds() const { return (m_sample_rate != SAMPLE_RATE_INVALID) ? HZ_TO_ATTOSECONDS(m_sample_rate) : ATTOSECONDS_PER_SECOND; }

	// set the sample rate of the stream; will kick in at the next global update
	void set_sample_rate(u32 sample_rate);

	// connect the output 'outputnum' of given input_stream to this stream's input 'inputnum'
	void set_input(int inputnum, sound_stream *input_stream, int outputnum = 0, float gain = 1.0f);

	// force an update to the current time
	void update();

	// force an update to the current time, returning a view covering the given time period
	read_stream_view update_view(attotime start, attotime end, u32 outputnum = 0);

	// apply any pending sample rate changes; should only be called by the sound manager
	void apply_sample_rate_changes(u32 updatenum, u32 downstream_rate);

#if (SOUND_DEBUG)
	// print one level of the sound graph and recursively tell our inputs to do the same
	void print_graph_recursive(int indent);
#endif

protected:
	// protected state
	std::string m_name;                            // name of this stream

private:
	// perform most of the initialization here
	void init_common(u32 inputs, u32 outputs, u32 sample_rate, sound_stream_flags flags);

	// if the sample rate has changed, this gets called to update internals
	void sample_rate_changed();

	// handle updates after a save state load
	void postload();

	// re-print the synchronization timer
	void reprime_sync_timer();

	// timer callback for synchronous streams
	void sync_update(void *, s32);

	// now callback which wrapps calls through to the old-style callbacks
	void oldstyle_callback_ex(sound_stream &stream, std::vector<read_stream_view> const &inputs, std::vector<write_stream_view> &outputs);

	// linking information
	device_t &m_device;                            // owning device
	sound_stream *m_next;                          // next stream in the chain

	// general information
	u32 m_sample_rate;                             // current live sample rate
	u32 m_pending_sample_rate;                     // pending sample rate for dynamic changes
	u32 m_last_sample_rate_update;                 // update number of last sample rate change
	bool m_input_adaptive;                         // adaptive stream that runs at the sample rate of its input
	bool m_output_adaptive;                        // adaptive stream that runs at the sample rate of its output
	bool m_synchronous;                            // synchronous stream that runs at the rate of its input
	emu_timer *m_sync_timer;                       // update timer for synchronous streams

	// input information
	std::vector<sound_stream_input> m_input;       // list of streams we directly depend upon
	std::vector<stream_sample_t *> m_input_array;  // array of inputs for passing to the callback
	std::vector<read_stream_view> m_input_view;    // array of output views for passing to the callback
	std::vector<std::unique_ptr<sound_stream>> m_resampler_list; // internal list of resamplers

	// output information
	u32 m_output_base;                             // base index of our outputs, relative to our device
	std::vector<sound_stream_output> m_output;     // list of streams which directly depend upon us
	std::vector<stream_sample_t *> m_output_array; // array of outputs for passing to the callback
	std::vector<write_stream_view> m_output_view;  // array of output views for passing to the callback

	// callback information
	stream_update_delegate m_callback;             // callback function
	stream_update_ex_delegate m_callback_ex;       // extended callback function
};


// ======================> default_resampler_stream

class default_resampler_stream : public sound_stream
{
public:
	// construction/destruction
	default_resampler_stream(device_t &device);

	// update handler
	void resampler_sound_update(sound_stream &stream, std::vector<read_stream_view> const &inputs, std::vector<write_stream_view> &outputs);

private:
	// internal state
	u32 m_max_latency;
};


// ======================> sound_manager

// structure describing an indexed mixer
struct mixer_input
{
	device_mixer_interface *mixer;          // owning device interface
	sound_stream *          stream;         // stream within the device
	int                     inputnum;       // input on the stream
};

class sound_manager
{
	friend class sound_stream;

	// reasons for muting
	static constexpr u8 MUTE_REASON_PAUSE = 0x01;
	static constexpr u8 MUTE_REASON_UI = 0x02;
	static constexpr u8 MUTE_REASON_DEBUGGER = 0x04;
	static constexpr u8 MUTE_REASON_SYSTEM = 0x08;

	// stream updates
	static const attotime STREAMS_UPDATE_ATTOTIME;

public:
	static constexpr int STREAMS_UPDATE_FREQUENCY = 50;

	// construction/destruction
	sound_manager(running_machine &machine);
	~sound_manager();

	// getters
	running_machine &machine() const { return m_machine; }
	int attenuation() const { return m_attenuation; }
	const std::vector<std::unique_ptr<sound_stream>> &streams() const { return m_stream_list; }
	attotime last_update() const { return m_last_update; }
	int sample_count() const { return m_samples_this_update; }
	int unique_id() { return m_unique_id++; }

	// allocate a new stream with the old-style callback
	sound_stream *stream_alloc(device_t &device, u32 inputs, u32 outputs, u32 sample_rate, stream_update_delegate callback);

	// allocate a new stream with a new-style callback
	sound_stream *stream_alloc(device_t &device, u32 inputs, u32 outputs, u32 sample_rate, stream_update_ex_delegate callback, sound_stream_flags resampler = STREAM_RESAMPLER_DEFAULT);

	// begin recording a WAV file if options has requested it
	void start_recording();

	// stop recording the WAV file
	void stop_recording();

	// set the global OSD attenuation level
	void set_attenuation(float attenuation);

	// mute sound for one of various independent reasons
	void ui_mute(bool turn_off = true) { mute(turn_off, MUTE_REASON_UI); }
	void debugger_mute(bool turn_off = true) { mute(turn_off, MUTE_REASON_DEBUGGER); }
	void system_mute(bool turn_off = true) { mute(turn_off, MUTE_REASON_SYSTEM); }
	void system_enable(bool turn_on = true) { mute(!turn_on, MUTE_REASON_SYSTEM); }

	// return information about the given mixer input, by index
	bool indexed_mixer_input(int index, mixer_input &info) const;

	// fill the given buffer with 16-bit stereo audio samples
	void samples(s16 *buffer);

private:
	// set/reset the mute state for the given reason
	void mute(bool mute, u8 reason);

	// reset all sound chips
	void reset();

	// pause/resume sound output
	void pause();
	void resume();

	// handle configuration load/save
	void config_load(config_type cfg_type, util::xml::data_node const *parentnode);
	void config_save(config_type cfg_type, util::xml::data_node *parentnode);

	// helper to adjust scale factor toward a goal
	stream_buffer::sample_t adjust_toward_compressor_scale(stream_buffer::sample_t curscale, stream_buffer::sample_t prevsample, stream_buffer::sample_t rawsample);

	// periodic sound update, called STREAMS_UPDATE_FREQUENCY per second
	void update(void *ptr = nullptr, s32 param = 0);

	// internal state
	running_machine &m_machine;           // reference to the running machine
	emu_timer *m_update_timer;            // timer that runs the update function

	u32 m_update_number;                  // current update index; used for sample rate updates
	attotime m_last_update;               // time of the last update
	u32 m_finalmix_leftover;              // leftover samples in the final mix
	u32 m_samples_this_update;            // number of samples this update
	std::vector<s16> m_finalmix;          // final mix, in 16-bit signed format
	std::vector<stream_buffer::sample_t> m_leftmix; // left speaker mix, in native format
	std::vector<stream_buffer::sample_t> m_rightmix; // right speaker mix, in native format

	stream_buffer::sample_t m_compressor_scale; // current compressor scale factor
	int m_compressor_counter;             // compressor update counter for backoff

	u8 m_muted;                           // bitmask of muting reasons
	bool m_nosound_mode;                  // true if we're in "nosound" mode
	int m_attenuation;                    // current attentuation level (at the OSD)
	int m_unique_id;                      // unique ID used for stream identification
	wav_file *m_wavfile;                  // WAV file for streaming

	// streams data
	std::vector<std::unique_ptr<sound_stream>> m_stream_list; // list of streams
};


#endif // MAME_EMU_SOUND_H
