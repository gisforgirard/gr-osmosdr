/* -*- c++ -*- */
/*
 * Copyright 2013 Dimitri Stolnikov <horiz0n@gmx.net>
 * Copyright 2014 Hoernchen <la@tfc-server.de>
 * Copyright 2020 Clayton Smith <argilo@gmail.com>
 *
 * gr-osmosdr is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * gr-osmosdr is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with gr-osmosdr; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

/*
 * config.h is generated by configure.  It contains the results
 * of probing for features, options etc.  It should be the first
 * file included in your .cc file.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdexcept>
#include <iostream>
#include <algorithm>
#ifdef USE_AVX
#include <immintrin.h>
#elif USE_SSE2
#include <emmintrin.h>
#endif

#include <gnuradio/io_signature.h>

#include "hackrf_sink_c.h"

#include "arg_helpers.h"

static inline bool cb_init(circular_buffer_t *cb, size_t capacity, size_t sz)
{
  cb->buffer = malloc(capacity * sz);
  if(cb->buffer == NULL)
    return false; // handle error
  cb->buffer_end = (int8_t *)cb->buffer + capacity * sz;
  cb->capacity = capacity;
  cb->count = 0;
  cb->sz = sz;
  cb->head = cb->buffer;
  cb->tail = cb->buffer;
  return true;
}

static inline void cb_free(circular_buffer_t *cb)
{
  free(cb->buffer);
  cb->buffer = NULL;
  // clear out other fields too, just to be safe
  cb->buffer_end = 0;
  cb->capacity = 0;
  cb->count = 0;
  cb->sz = 0;
  cb->head = 0;
  cb->tail = 0;
}

static inline bool cb_has_room(circular_buffer_t *cb)
{
  if(cb->count == cb->capacity)
    return false;
  return true;
}

static inline bool cb_is_empty(circular_buffer_t *cb)
{
  return cb->count == 0;
}

static inline bool cb_push_back(circular_buffer_t *cb, const void *item)
{
  if(cb->count == cb->capacity)
    return false; // handle error
  memcpy(cb->head, item, cb->sz);
  cb->head = (int8_t *)cb->head + cb->sz;
  if(cb->head == cb->buffer_end)
    cb->head = cb->buffer;
  cb->count++;
  return true;
}

static inline bool cb_pop_front(circular_buffer_t *cb, void *item)
{
  if(cb->count == 0)
    return false; // handle error
  memcpy(item, cb->tail, cb->sz);
  cb->tail = (int8_t *)cb->tail + cb->sz;
  if(cb->tail == cb->buffer_end)
    cb->tail = cb->buffer;
  cb->count--;
  return true;
}

hackrf_sink_c_sptr make_hackrf_sink_c (const std::string & args)
{
  return gnuradio::get_initial_sptr(new hackrf_sink_c (args));
}

/*
 * Specify constraints on number of input and output streams.
 * This info is used to construct the input and output signatures
 * (2nd & 3rd args to gr::block's constructor).  The input and
 * output signatures are used by the runtime system to
 * check that a valid number and type of inputs and outputs
 * are connected to this block.  In this case, we accept
 * only 0 input and 1 output.
 */
static const int MIN_IN = 1;  // mininum number of input streams
static const int MAX_IN = 1;  // maximum number of input streams
static const int MIN_OUT = 0;  // minimum number of output streams
static const int MAX_OUT = 0;  // maximum number of output streams

/*
 * The private constructor
 */
hackrf_sink_c::hackrf_sink_c (const std::string &args)
  : gr::sync_block ("hackrf_sink_c",
        gr::io_signature::make(MIN_IN, MAX_IN, sizeof (gr_complex)),
        gr::io_signature::make(MIN_OUT, MAX_OUT, sizeof (gr_complex))),
    hackrf_common::hackrf_common(args),
    _buf(NULL),
    _vga_gain(0)
{
  dict_t dict = params_to_dict(args);

  _buf_num = 0;

  if (dict.count("buffers"))
    _buf_num = std::stoi(dict["buffers"]);

  if (0 == _buf_num)
    _buf_num = BUF_NUM;

  _stopping = false;

  if ( BUF_NUM != _buf_num ) {
    std::cerr << "Using " << _buf_num << " buffers of size " << BUF_LEN << "."
              << std::endl;
  }

  set_center_freq( (get_freq_range().start() + get_freq_range().stop()) / 2.0 );
  set_sample_rate( get_sample_rates().start() );
  set_bandwidth( 0 );

  set_gain( 0 ); /* disable AMP gain stage by default to protect full sprectrum pre-amp from physical damage */

  set_if_gain( 16 ); /* preset to a reasonable default (non-GRC use case) */

  // Check device args to find out if bias/phantom power is desired.
  if ( dict.count("bias_tx") ) {
    hackrf_common::set_bias(dict["bias_tx"] == "1");
  }

  _buf = (int8_t *) malloc( BUF_LEN );

  cb_init( &_cbuf, _buf_num, BUF_LEN );
}

/*
 * Our virtual destructor.
 */
hackrf_sink_c::~hackrf_sink_c ()
{
  free(_buf);
  _buf = NULL;

  cb_free( &_cbuf );
}

int hackrf_sink_c::_hackrf_tx_callback(hackrf_transfer *transfer)
{
  hackrf_sink_c *obj = (hackrf_sink_c *)transfer->tx_ctx;
  return obj->hackrf_tx_callback(transfer->buffer, transfer->valid_length);
}

int hackrf_sink_c::hackrf_tx_callback(unsigned char *buffer, uint32_t length)
{
#if 0
  for (unsigned int i = 0; i < length; ++i) /* simulate noise */
    *buffer++ = rand() % 255;
#else
  {
    std::lock_guard<std::mutex> lock(_buf_mutex);

    if ( ! cb_pop_front( &_cbuf, buffer ) ) {
      memset(buffer, 0, length);
      if (_stopping) {
        _buf_cond.notify_one();
        return -1;
      } else {
        std::cerr << "U" << std::flush;
      }
    } else {
//      std::cerr << "-" << std::flush;
      _buf_cond.notify_one();
    }
  }
#endif
  return 0; // TODO: return -1 on error/stop
}

bool hackrf_sink_c::start()
{
  if ( ! _dev.get() )
    return false;

  _stopping = false;
  _buf_used = 0;
  hackrf_common::start();
  int ret = hackrf_start_tx( _dev.get(), _hackrf_tx_callback, (void *)this );
  if ( ret != HACKRF_SUCCESS ) {
    std::cerr << "Failed to start TX streaming (" << ret << ")" << std::endl;
    return false;
  }
  return true;
}

bool hackrf_sink_c::stop()
{
  int i;

  if ( ! _dev.get() )
    return false;

  {
    std::unique_lock<std::mutex> lock(_buf_mutex);

    while ( ! cb_has_room(&_cbuf) )
      _buf_cond.wait( lock );

    // Fill the rest of the current buffer with silence.
    memset(_buf + _buf_used, 0, BUF_LEN - _buf_used);
    cb_push_back( &_cbuf, _buf );
    _buf_used = 0;

    // Add some more silence so the end doesn't get cut off.
    memset(_buf, 0, BUF_LEN);
    for (i = 0; i < 5; i++) {
      while ( ! cb_has_room(&_cbuf) )
        _buf_cond.wait( lock );

      cb_push_back( &_cbuf, _buf );
    }

    _stopping = true;

    while (hackrf_is_streaming(_dev.get()) == HACKRF_TRUE)
      _buf_cond.wait( lock );
  }

  hackrf_common::stop();
  int ret = hackrf_stop_tx( _dev.get() );
  if ( ret != HACKRF_SUCCESS ) {
    std::cerr << "Failed to stop TX streaming (" << ret << ")" << std::endl;
    return false;
  }
  return true;
}

#ifdef USE_AVX
void convert_avx(const float* inbuf, int8_t* outbuf,const unsigned int count)
{
  __m256 mulme = _mm256_set_ps(127.0f, 127.0f, 127.0f, 127.0f, 127.0f, 127.0f, 127.0f, 127.0f);
  for(unsigned int i=0; i<count;i++){

  __m256i itmp3 = _mm256_cvtps_epi32(_mm256_mul_ps(_mm256_loadu_ps(&inbuf[i*16+0]), mulme));
  __m256i itmp4 = _mm256_cvtps_epi32(_mm256_mul_ps(_mm256_loadu_ps(&inbuf[i*16+8]), mulme));

  __m128i a1 = _mm256_extractf128_si256(itmp3, 1);
  __m128i a0 = _mm256_castsi256_si128(itmp3);
  __m128i a3 = _mm256_extractf128_si256(itmp4, 1);
  __m128i a2 = _mm256_castsi256_si128(itmp4);

  __m128i outshorts1 = _mm_packs_epi32(a0, a1);
  __m128i outshorts2 = _mm_packs_epi32(a2, a3);

  __m128i outbytes = _mm_packs_epi16(outshorts1, outshorts2);

  _mm_storeu_si128 ((__m128i*)&outbuf[i*16], outbytes);
  }
}

#elif USE_SSE2
void convert_sse2(const float* inbuf, int8_t* outbuf,const unsigned int count)
{
  const __m128 mulme = _mm_set_ps( 127.0f, 127.0f, 127.0f, 127.0f );
  __m128 itmp1,itmp2,itmp3,itmp4;
  __m128i otmp1,otmp2,otmp3,otmp4;

  __m128i outshorts1,outshorts2;
  __m128i outbytes;

  for(unsigned int i=0; i<count;i++){

  itmp1 = _mm_mul_ps(_mm_loadu_ps(&inbuf[i*16+0]), mulme);
  itmp2 = _mm_mul_ps(_mm_loadu_ps(&inbuf[i*16+4]), mulme);
  itmp3 = _mm_mul_ps(_mm_loadu_ps(&inbuf[i*16+8]), mulme);
  itmp4 = _mm_mul_ps(_mm_loadu_ps(&inbuf[i*16+12]), mulme);

  otmp1 = _mm_cvtps_epi32(itmp1);
  otmp2 = _mm_cvtps_epi32(itmp2);
  otmp3 = _mm_cvtps_epi32(itmp3);
  otmp4 = _mm_cvtps_epi32(itmp4);

  outshorts1 = _mm_packs_epi32(otmp1, otmp2);
  outshorts2 = _mm_packs_epi32(otmp3, otmp4);

  outbytes = _mm_packs_epi16(outshorts1, outshorts2);

  _mm_storeu_si128 ((__m128i*)&outbuf[i*16], outbytes);
  }
}
#endif

void convert_default(float* inbuf, int8_t* outbuf,const unsigned int count)
{
  for(unsigned int i=0; i<count;i++){
    outbuf[i]= inbuf[i]*127;
  }
}

int hackrf_sink_c::work( int noutput_items,
                         gr_vector_const_void_star &input_items,
                         gr_vector_void_star &output_items )
{
  const gr_complex *in = (const gr_complex *) input_items[0];

  {
    std::unique_lock<std::mutex> lock(_buf_mutex);

    while ( ! cb_has_room(&_cbuf) )
      _buf_cond.wait( lock );
  }

  int8_t *buf = _buf + _buf_used;
  unsigned int prev_buf_used = _buf_used;

  unsigned int remaining = (BUF_LEN-_buf_used)/2; //complex

  unsigned int count = std::min((unsigned int)noutput_items,remaining);
  unsigned int sse_rem = count/8; // 8 complex = 16f==512bit for avx
  unsigned int nosse_rem = count%8; // remainder

#ifdef USE_AVX
  convert_avx((float*)in, buf, sse_rem);
  convert_default((float*)(in+sse_rem*8), buf+(sse_rem*8*2), nosse_rem*2);
#elif USE_SSE2
  convert_sse2((float*)in, buf, sse_rem);
  convert_default((float*)(in+sse_rem*8), buf+(sse_rem*8*2), nosse_rem*2);
#else
  convert_default((float*)in, buf, count*2);
#endif

  _buf_used += (sse_rem*8+nosse_rem)*2;
  int items_consumed = sse_rem*8+nosse_rem;

  if((unsigned int)noutput_items >= remaining) {
    {
      std::lock_guard<std::mutex> lock(_buf_mutex);

      if ( ! cb_push_back( &_cbuf, _buf ) ) {
        _buf_used = prev_buf_used;
        items_consumed = 0;
        std::cerr << "O" << std::flush;
      } else {
//        std::cerr << "+" << std::flush;
        _buf_used = 0;
      }
    }
  }

  // Tell runtime system how many input items we consumed on
  // each input stream.
  consume_each(items_consumed);

  // Tell runtime system how many output items we produced.
  return 0;
}

std::vector<std::string> hackrf_sink_c::get_devices()
{
  return hackrf_common::get_devices();
}

size_t hackrf_sink_c::get_num_channels()
{
  return 1;
}

osmosdr::meta_range_t hackrf_sink_c::get_sample_rates()
{
  return hackrf_common::get_sample_rates();
}

double hackrf_sink_c::set_sample_rate( double rate )
{
  return hackrf_common::set_sample_rate(rate);
}

double hackrf_sink_c::get_sample_rate()
{
  return hackrf_common::get_sample_rate();
}

osmosdr::freq_range_t hackrf_sink_c::get_freq_range( size_t chan )
{
  return hackrf_common::get_freq_range(chan);
}

double hackrf_sink_c::set_center_freq( double freq, size_t chan )
{
  return hackrf_common::set_center_freq(freq, chan);
}

double hackrf_sink_c::get_center_freq( size_t chan )
{
  return hackrf_common::get_center_freq(chan);
}

double hackrf_sink_c::set_freq_corr( double ppm, size_t chan )
{
  return hackrf_common::set_freq_corr(ppm, chan);
}

double hackrf_sink_c::get_freq_corr( size_t chan )
{
  return hackrf_common::get_freq_corr(chan);
}

std::vector<std::string> hackrf_sink_c::get_gain_names( size_t chan )
{
  return { "RF", "IF" };
}

osmosdr::gain_range_t hackrf_sink_c::get_gain_range( size_t chan )
{
  return get_gain_range( "RF", chan );
}

osmosdr::gain_range_t hackrf_sink_c::get_gain_range( const std::string & name, size_t chan )
{
  if ( "RF" == name ) {
    return osmosdr::gain_range_t( 0, 14, 14 );
  }

  if ( "IF" == name ) {
    return osmosdr::gain_range_t( 0, 47, 1 );
  }

  return osmosdr::gain_range_t();
}

bool hackrf_sink_c::set_gain_mode( bool automatic, size_t chan )
{
  return hackrf_common::set_gain_mode(automatic, chan);
}

bool hackrf_sink_c::get_gain_mode( size_t chan )
{
  return hackrf_common::get_gain_mode(chan);
}

double hackrf_sink_c::set_gain( double gain, size_t chan )
{
  return hackrf_common::set_gain(gain, chan);
}

double hackrf_sink_c::set_gain( double gain, const std::string & name, size_t chan)
{
  if ( "RF" == name ) {
    return set_gain( gain, chan );
  }

  if ( "IF" == name ) {
    return set_if_gain( gain, chan );
  }

  return set_gain( gain, chan );
}

double hackrf_sink_c::get_gain( size_t chan )
{
  return hackrf_common::get_gain(chan);
}

double hackrf_sink_c::get_gain( const std::string & name, size_t chan )
{
  if ( "RF" == name ) {
    return get_gain( chan );
  }

  if ( "IF" == name ) {
    return _vga_gain;
  }

  return get_gain( chan );
}

double hackrf_sink_c::set_if_gain( double gain, size_t chan )
{
  int ret;
  osmosdr::gain_range_t if_gains = get_gain_range( "IF", chan );

  if (_dev.get()) {
    double clip_gain = if_gains.clip( gain, true );

    ret = hackrf_set_txvga_gain( _dev.get(), uint32_t(clip_gain) );
    if ( HACKRF_SUCCESS == ret ) {
      _vga_gain = clip_gain;
    } else {
      HACKRF_THROW_ON_ERROR( ret, HACKRF_FUNC_STR( "hackrf_set_txvga_gain", clip_gain ) )
    }
  }

  return _vga_gain;
}

double hackrf_sink_c::set_bb_gain( double gain, size_t chan )
{
  return 0;
}

std::vector< std::string > hackrf_sink_c::get_antennas( size_t chan )
{
  return hackrf_common::get_antennas(chan);
}

std::string hackrf_sink_c::set_antenna( const std::string & antenna, size_t chan )
{
  return hackrf_common::set_antenna(antenna, chan);
}

std::string hackrf_sink_c::get_antenna( size_t chan )
{
  return hackrf_common::get_antenna(chan);
}

double hackrf_sink_c::set_bandwidth( double bandwidth, size_t chan )
{
  return hackrf_common::set_bandwidth(bandwidth, chan);
}

double hackrf_sink_c::get_bandwidth( size_t chan )
{
  return hackrf_common::get_bandwidth(chan);
}

osmosdr::freq_range_t hackrf_sink_c::get_bandwidth_range( size_t chan )
{
  return hackrf_common::get_bandwidth_range(chan);
}
