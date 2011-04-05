/**
 * @file database/src/VideoReader.cc
 * @author <a href="mailto:andre.anjos@idiap.ch">Andre Anjos</a> 
 *
 * Implements a class to read and write Video files and convert the frames into
 * something that torch can understand. This implementation is heavily based on
 * the excellent tutorial here: http://dranger.com/ffmpeg/, with some personal
 * modifications.
 */

#include <stdexcept>
#include <boost/format.hpp>
#include <boost/preprocessor.hpp>
#include <limits>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

#include "database/VideoReader.h"
#include "database/Exception.h"
#include "database/VideoException.h"

namespace db = Torch::database;

/**
 * When called, initializes the ffmpeg library
 */
static bool initialize_ffmpeg() {
  av_register_all();
  av_log_set_level(-1);
  return true;
}

bool db::VideoReader::s_ffmpeg_initialized = initialize_ffmpeg();

db::VideoReader::VideoReader(const std::string& filename):
  m_filepath(filename),
  m_height(0),
  m_width(0),
  m_nframes(0),
  m_framerate(0),
  m_duration(0),
  m_codecname(""),
  m_codecname_long(""),
  m_formatted_info("")
{
  open();
}

db::VideoReader::VideoReader(const db::VideoReader& other):
  m_filepath(other.m_filepath),
  m_height(0),
  m_width(0),
  m_nframes(0),
  m_framerate(0),
  m_duration(0),
  m_codecname(""),
  m_codecname_long(""),
  m_formatted_info("")
{
  open();
}

db::VideoReader& db::VideoReader::operator= (const db::VideoReader& other) {
  m_filepath = other.m_filepath;
  m_height = 0;
  m_width = 0;
  m_nframes = 0;
  m_framerate = 0.0;
  m_duration = 0;
  m_codecname = "";
  m_codecname_long = "";
  m_formatted_info = "";
  open();
  return *this;
}

void db::VideoReader::open() {
  AVFormatContext* format_ctxt;

  if (av_open_input_file(&format_ctxt, m_filepath.c_str(),NULL,0,NULL) != 0) {
    throw db::FileNotReadable(m_filepath);
  }

  // Retrieve stream information
  if (av_find_stream_info(format_ctxt)<0) {
    throw db::FFmpegException(m_filepath.c_str(), "cannot find stream info");
  }

  // Look for the first video stream in the file
  int stream_index = -1;
  for (size_t i=0; i<format_ctxt->nb_streams; ++i) {
    if (format_ctxt->streams[i]->codec->codec_type==CODEC_TYPE_VIDEO) {
      stream_index = i;
      break;
    }
  }
  if(stream_index == -1) {
    throw db::FFmpegException(m_filepath.c_str(), "cannot find any video stream");
  }

  // Get a pointer to the codec context for the video stream
  AVCodecContext* codec_ctxt = format_ctxt->streams[stream_index]->codec;

  // Hack to correct frame rates that seem to be generated by some codecs 
  if(codec_ctxt->time_base.num > 1000 && codec_ctxt->time_base.den == 1) {
    codec_ctxt->time_base.den = 1000;
  }

  // Find the decoder for the video stream
  AVCodec* codec = avcodec_find_decoder(codec_ctxt->codec_id);

  if (!codec) {
    throw db::FFmpegException(m_filepath.c_str(), "unsupported codec required");
  }

  // Open codec
  if (avcodec_open(codec_ctxt, codec) < 0) {
    throw db::FFmpegException(m_filepath.c_str(), "cannot open supported codec");
  }

  /**
   * Copies some information from the contexts opened
   */
  m_width = codec_ctxt->width;
  m_height = codec_ctxt->height;
  m_nframes = format_ctxt->streams[stream_index]->nb_frames;
  m_framerate = m_nframes * AV_TIME_BASE / format_ctxt->duration;
  m_duration = format_ctxt->duration;
  m_codecname = codec->name;
  m_codecname_long = codec->long_name;

  /**
   * This will create a local description of the contents of the stream, in
   * printable format.
   */
  boost::format fmt("Video file: %s; FFmpeg: avformat-%s; avcodec-%s; avutil-%s; swscale-%d; Codec: %s (%s); Time: %.2f s (%d @ %2.fHz); Size (w x h): %d x %d pixels");
  fmt % m_filepath;
  fmt % BOOST_PP_STRINGIZE(LIBAVFORMAT_VERSION);
  fmt % BOOST_PP_STRINGIZE(LIBAVCODEC_VERSION);
  fmt % BOOST_PP_STRINGIZE(LIBAVUTIL_VERSION);
  fmt % BOOST_PP_STRINGIZE(LIBSWSCALE_VERSION);
  fmt % m_codecname_long;
  fmt % m_codecname;
  fmt % (m_duration / 1e6);
  fmt % m_nframes;
  fmt % m_framerate;
  fmt % m_width;
  fmt % m_height;
  m_formatted_info = fmt.str();

  //closes the codec we used
  avcodec_close(codec_ctxt);

  //and we close the input file
  av_close_input_file(format_ctxt);
}

db::VideoReader::~VideoReader() {
}

void db::VideoReader::load(blitz::Array<uint8_t,4>& data) const {
  //checks if the output array shape conforms to the video specifications,
  //otherwise, resize it.
  if ((size_t)data.extent(0) != numberOfFrames() || data.extent(1) != 3 ||
      (size_t)data.extent(2) != height() || 
      (size_t)data.extent(3) != width() ||
      !data.isStorageContiguous())
    data.resize(numberOfFrames(), 3, height(), width());

  blitz::Range a = blitz::Range::all();
  for (const_iterator it=begin(); it!=end();) {
    blitz::Array<uint8_t,3> ref = data(it.cur(), a, a, a);
    it.read(ref);
  }
}

db::VideoReader::const_iterator db::VideoReader::begin() const {
  return this;
}

db::VideoReader::const_iterator db::VideoReader::end() const {
  return db::VideoReader::const_iterator();
}

/**
 * iterator implementation
 */

db::VideoReader::const_iterator::const_iterator(const db::VideoReader* parent) :
  m_parent(parent),
  m_format_ctxt(0),
  m_stream_index(-1),
  m_codec_ctxt(0),
  m_codec(0),
  m_frame_buffer(0),
  m_rgb_frame_buffer(0),
  m_raw_buffer(0),
  m_current_frame(std::numeric_limits<size_t>::max()),
  m_sws_context(0)
{
  init();
}

db::VideoReader::const_iterator::const_iterator():
  m_parent(0),
  m_format_ctxt(0),
  m_stream_index(-1),
  m_codec_ctxt(0),
  m_codec(0),
  m_frame_buffer(0),
  m_rgb_frame_buffer(0),
  m_raw_buffer(0),
  m_current_frame(std::numeric_limits<size_t>::max()),
  m_sws_context(0)
{
}

db::VideoReader::const_iterator::const_iterator
(const db::VideoReader::const_iterator& other) :
  m_parent(other.m_parent),
  m_format_ctxt(0),
  m_stream_index(-1),
  m_codec_ctxt(0),
  m_codec(0),
  m_frame_buffer(0),
  m_rgb_frame_buffer(0),
  m_raw_buffer(0),
  m_current_frame(std::numeric_limits<size_t>::max()),
  m_sws_context(0)
{
  init();
  (*this) += other.m_current_frame;
}

db::VideoReader::const_iterator::~const_iterator() {
  reset();
}

db::VideoReader::const_iterator& db::VideoReader::const_iterator::operator= (const db::VideoReader::const_iterator& other) {
  reset();
  m_parent = other.m_parent;
  init();
  (*this) += other.m_current_frame;
  return *this;
}

void db::VideoReader::const_iterator::init() {
  const char* filename = m_parent->filename().c_str();

  //basic constructor, prepare readout
  if (av_open_input_file(&m_format_ctxt, filename, NULL, 0, NULL) != 0) {
    throw db::FileNotReadable(filename);
  }

  // Retrieve stream information
  if (av_find_stream_info(m_format_ctxt)<0) {
    throw db::FFmpegException(filename, "cannot find stream info");
  }

  // Look for the first video stream in the file
  for (size_t i=0; i<m_format_ctxt->nb_streams; ++i) {
    if (m_format_ctxt->streams[i]->codec->codec_type==CODEC_TYPE_VIDEO) {
      m_stream_index = i;
      break;
    }
  }
  if(m_stream_index == -1) {
    throw db::FFmpegException(filename, "cannot find any video stream");
  }

  // Get a pointer to the codec context for the video stream
  m_codec_ctxt = m_format_ctxt->streams[m_stream_index]->codec;

  // Find the decoder for the video stream
  m_codec = avcodec_find_decoder(m_codec_ctxt->codec_id);

  if (!m_codec) {
    throw db::FFmpegException(filename, "unsupported codec required");
  }

  // Open codec
  if (avcodec_open(m_codec_ctxt, m_codec) < 0) {
    throw db::FFmpegException(filename, "cannot open supported codec");
  }

  // Hack to correct frame rates that seem to be generated by some codecs 
  if(m_codec_ctxt->time_base.num > 1000 && m_codec_ctxt->time_base.den == 1)
    m_codec_ctxt->time_base.den = 1000;

  // Allocate memory for a buffer to read frames
  m_frame_buffer = avcodec_alloc_frame();
  if (!m_frame_buffer) {
    throw db::FFmpegException(filename, "cannot allocate frame buffer");
  }

  // Allocate memory for a second buffer that contains RGB converted data.
  m_rgb_frame_buffer = avcodec_alloc_frame();
  if (!m_rgb_frame_buffer) {
    throw db::FFmpegException(filename, "cannot allocate RGB frame buffer");
  }

  // Allocate memory for the raw data buffer
  int nbytes = avpicture_get_size(PIX_FMT_RGB24, m_codec_ctxt->width,
      m_codec_ctxt->height);
  m_raw_buffer = (uint8_t*)av_malloc(nbytes*sizeof(uint8_t));
  if (!m_raw_buffer) {
    throw db::FFmpegException(filename, "cannot allocate raw frame buffer");
  }
  
  // Assign appropriate parts of buffer to image planes in m_rgb_frame_buffer
  avpicture_fill((AVPicture *)m_rgb_frame_buffer, m_raw_buffer, PIX_FMT_RGB24,
      m_parent->width(), m_parent->height());

  /**
   * Initializes the software scaler (SWScale) so we can convert images from
   * the movie native format into RGB. You can define which kind of
   * interpolation to perform. Some options from libswscale are:
   * SWS_FAST_BILINEAR, SWS_BILINEAR, SWS_BICUBIC, SWS_X, SWS_POINT, SWS_AREA
   * SWS_BICUBLIN, SWS_GAUSS, SWS_SINC, SWS_LANCZOS, SWS_SPLINE
   */
  m_sws_context = sws_getContext(m_parent->width(), m_parent->height(),
      m_codec_ctxt->pix_fmt, m_parent->width(), m_parent->height(),
      PIX_FMT_RGB24, SWS_BICUBIC, NULL, NULL, NULL);
  if (!m_sws_context) {
    throw db::FFmpegException(filename, "cannot initialize software scaler");
  }

  //At this point we are ready to start reading out frames.
  m_current_frame = 0;
  
  //The file maybe valid, but contain zero frames... We check for this here:
  if (m_current_frame >= m_parent->numberOfFrames()) {
    //transform the current iterator in "end"
    reset();
  }
}

void db::VideoReader::const_iterator::reset() {
  //free-up memory
  if (m_frame_buffer) { 
    av_free(m_frame_buffer); 
    m_frame_buffer = 0;
  }
  
  if (m_rgb_frame_buffer) { 
    av_free(m_rgb_frame_buffer); 
    m_rgb_frame_buffer = 0;
  }

  if (m_raw_buffer) { 
    av_free(m_raw_buffer); 
    m_raw_buffer=0; 
  }
  
  if (m_sws_context) { 
    sws_freeContext(m_sws_context);
    m_sws_context=0; 
  }
  
  //closes the codec we used
  if (m_codec_ctxt) {
    avcodec_close(m_codec_ctxt);
    m_codec = 0;
  }
  
  //closes the video file we opened
  if (m_format_ctxt) {
    av_close_input_file(m_format_ctxt);
    m_codec_ctxt = 0;
    m_format_ctxt = 0;
  }

  m_current_frame = std::numeric_limits<size_t>::max(); //that means "end" 

  m_parent = 0;
}

void db::VideoReader::const_iterator::read(blitz::Array<uint8_t,3>& data) {
  //checks if we have not passed the end of the video sequence already
  if(m_current_frame > m_parent->numberOfFrames()) {
    throw db::IndexError(m_current_frame);
  }

  //checks if the output array shape conforms to the video specifications,
  //otherwise, resize it.
  if (data.extent(0) != 3 || 
      (size_t)data.extent(1) != m_parent->height() || 
      (size_t)data.extent(2) != m_parent->width())
    data.resize(3, m_parent->height(), m_parent->width());

  int gotPicture = 0;
  AVPacket packet;
  av_init_packet(&packet);

  while (av_read_frame(m_format_ctxt, &packet) >= 0) {
    // Is this a packet from the video stream?
    if (packet.stream_index == m_stream_index) {
      // Decodes video frame, store it on my buffer
      avcodec_decode_video2(m_codec_ctxt, m_frame_buffer, &gotPicture, &packet);

      // Did we get a video frame?
      if (gotPicture) {
        sws_scale(m_sws_context, m_frame_buffer->data, m_frame_buffer->linesize,
            0, m_parent->height(), m_rgb_frame_buffer->data, 
            m_rgb_frame_buffer->linesize);

        // Got the image - exit
        ++m_current_frame;

        // Frees the packet that was allocated by av_read_frame
        av_free_packet(&packet);
        break;
      }
    }

    // Frees the packet that was allocated by av_read_frame
    av_free_packet(&packet);
  }

  // Copies the data into the destination array. Here is some background: Torch
  // arranges the data for a colored image like: (color-bands, height, width).
  // That makes it easy to extract a given band from the image as its memory is
  // contiguous. FFmpeg prefers the following encoding (height, width,
  // color-bands). So, we have no other choice than copying the data twice. The
  // most practical would be, of course, to have the software scaler lay down
  // the data directly onto the blitz::Array memory, but with the current
  // settings, that is not possible. 
  //
  // The FFmpeg way to read and write image data is hard-coded and impossible
  // to circumvent by passing a different stride setup.
  data = blitz::Array<uint8_t,3>(m_rgb_frame_buffer->data[0],
      blitz::shape(m_parent->height(), m_parent->width(), 3),
      blitz::neverDeleteData).transpose(2,0,1);

  if (m_current_frame >= m_parent->numberOfFrames()) {
    //transform the current iterator in "end"
    reset();
  }
}

/**
 * This method does essentially the same as read(), except it skips a few
 * operations to get a better performance.
 */
db::VideoReader::const_iterator& db::VideoReader::const_iterator::operator++ () {
  //checks if we have not passed the end of the video sequence already
  if(m_current_frame > m_parent->numberOfFrames()) {
    throw db::IndexError(m_current_frame);
  }

  int gotPicture = 0;
  AVPacket packet;
  av_init_packet(&packet);

  while (av_read_frame(m_format_ctxt, &packet) >= 0) {
    // Is this a packet from the video stream?
    if (packet.stream_index == m_stream_index) {
      // Decodes video frame, store it on my buffer
      avcodec_decode_video2(m_codec_ctxt, m_frame_buffer, &gotPicture, &packet);

      // Did we get a video frame?
      if (gotPicture) {
        // Got the image - exit
        ++m_current_frame;

        // Frees the packet that was allocated by av_read_frame
        av_free_packet(&packet);
        break;
      }
    }

    // Frees the packet that was allocated by av_read_frame
    av_free_packet(&packet);
  }

  if (m_current_frame >= m_parent->numberOfFrames()) {
    //transform the current iterator in "end"
    reset();
  }
  return *this;
}

db::VideoReader::const_iterator& db::VideoReader::const_iterator::operator+= (size_t frames) {
  for (size_t i=0; i<frames; ++i) ++(*this);
  return *this;
}

bool db::VideoReader::const_iterator::operator== (const const_iterator& other) {
  return (this->m_parent == other.m_parent) && (this->m_current_frame == other.m_current_frame);
}

bool db::VideoReader::const_iterator::operator!= (const const_iterator& other) {
  return !(*this == other);
}
