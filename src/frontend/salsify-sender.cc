/* -*-mode:c++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */

#include <getopt.h>

#include <cstdlib>
#include <iostream>
#include <chrono>
#include <vector>
#include <random>
#include <limits>
#include <thread>
#include <future>
#include <algorithm>
#include <unordered_map>

#include "exception.hh"
#include "finally.hh"
#include "paranoid.hh"
#include "yuv4mpeg.hh"
#include "encoder.hh"
#include "socket.hh"
#include "packet.hh"
#include "poller.hh"
#include "socketpair.hh"
#include "camera.hh"
#include "pacer.hh"

using namespace std;
using namespace std::chrono;
using namespace PollerShortNames;

class AverageEncodingTime
{
private:
  static constexpr double ALPHA = 0.1;

  double value_ { -1.0 };
  microseconds last_update_{ 0 };

public:
  void add( const microseconds timestamp_us )
  {
    assert( timestamp_us >= last_update_ );

    if ( value_ < 0 ) {
      value_ = 0;
    }
    else if ( timestamp_us - last_update_ > 1s /* 1 seconds */ ) {
      value_ = 0;
    }
    else {
      double new_value = max( 0l, duration_cast<microseconds>( timestamp_us - last_update_ ).count() );
      value_ = ALPHA * new_value + ( 1 - ALPHA ) * value_;
    }

    last_update_ = timestamp_us;
  }

  uint32_t int_value() const { return static_cast<uint32_t>( value_ ); }
};

struct EncodeJob
{
  string name;

  RasterHandle raster;

  Encoder encoder;
  EncoderMode mode;

  uint8_t y_ac_qi;
  size_t target_size;

  EncodeJob( const string & name, RasterHandle raster, const Encoder & encoder,
             const EncoderMode mode, const uint8_t y_ac_qi, const size_t target_size )
    : name( name ), raster( raster ), encoder( encoder ),
      mode( mode ), y_ac_qi( y_ac_qi ), target_size( target_size )
  {}
};

struct EncodeOutput
{
  Encoder encoder;
  vector<uint8_t> frame;
  uint32_t source_minihash;
  milliseconds encode_time;
  string job_name;
  uint8_t y_ac_qi;

  EncodeOutput( Encoder && encoder, vector<uint8_t> && frame,
                const uint32_t source_minihash, const milliseconds encode_time,
                const string & job_name, const uint8_t y_ac_qi )
    : encoder( move( encoder ) ), frame( move( frame ) ),
      source_minihash( source_minihash ), encode_time( encode_time ),
      job_name( job_name ), y_ac_qi( y_ac_qi )
  {}
};

EncodeOutput do_encode_job( EncodeJob && encode_job )
{
  vector<uint8_t> output;

  uint32_t source_minihash = encode_job.encoder.minihash();

  const auto encode_beginning = system_clock::now();

  uint8_t quantizer_in_use = 0;

  switch ( encode_job.mode ) {
  case CONSTANT_QUANTIZER:
    output = encode_job.encoder.encode_with_quantizer( encode_job.raster.get(),
                                                       encode_job.y_ac_qi );
    quantizer_in_use = encode_job.y_ac_qi;
    break;

  case TARGET_FRAME_SIZE:
    output = encode_job.encoder.encode_with_target_size( encode_job.raster.get(),
                                                         encode_job.target_size );
    break;

  default:
    throw runtime_error( "unsupported encoding mode." );
  }

  const auto encode_ending = system_clock::now();
  const auto ms_elapsed = duration_cast<milliseconds>( encode_ending - encode_beginning );

  return { move( encode_job.encoder ), move( output ), source_minihash, ms_elapsed, encode_job.name, quantizer_in_use };
}

size_t target_size( uint32_t avg_delay, const uint64_t last_acked, const uint64_t last_sent )
{
  static constexpr uint32_t max_delay = 100 * 1000; // 100 ms = 100,000 us

  if ( avg_delay == 0 ) { avg_delay = 1; }

  /* cerr << "Packets in flight: " << last_sent - last_acked << "\n";
  cerr << "Avg inter-packet-arrival interval: " << avg_delay << "\n";
  cerr << "Imputed delay: " << avg_delay * (last_sent - last_acked) << " us\n"; */

  return 1400 * max( 0l, static_cast<int64_t>( max_delay / avg_delay - ( last_sent - last_acked ) ) );
}

void usage( const char *argv0 )
{
  cerr << "Usage: " << argv0 << " [-d, --device CAMERA] [-p, --pixfmt PIXEL_FORMAT] HOST PORT CONNECTION_ID" << endl;
}

uint64_t ack_seq_no( const AckPacket & ack,
                     const vector<uint64_t> & cumulative_fpf )
{
  return ( ack.frame_no() > 0 )
       ? ( cumulative_fpf[ ack.frame_no() - 1 ] + ack.fragment_no() )
       : ack.fragment_no();
}

int main( int argc, char *argv[] )
{
  /* check the command-line arguments */
  if ( argc < 1 ) { /* for sticklers */
    abort();
  }

  /* camera settings */
  string camera_device = "/dev/video0";
  string pixel_format = "NV12";

  const option command_line_options[] = {
    { "device", required_argument, nullptr, 'd' },
    { "pixfmt", required_argument, nullptr, 'p' },
    { 0, 0, 0, 0 }
  };

  while ( true ) {
    const int opt = getopt_long( argc, argv, "d:p:", command_line_options, nullptr );

    if ( opt == -1 ) { break; }

    switch ( opt ) {
    case 'd': camera_device = optarg; break;
    case 'p': pixel_format = optarg; break;
    default: usage( argv[ 0 ] ); return EXIT_FAILURE;
    }
  }

  if ( optind + 2 >= argc ) {
    usage( argv[ 0 ] );
    return EXIT_FAILURE;
  }

  /* construct Socket for outgoing datagrams */
  UDPSocket socket;
  socket.connect( Address( argv[ optind ], argv[ optind + 1 ] ) );
  socket.set_timestamps();

  /* make pacer to smooth out outgoing packets */
  Pacer pacer;

  /* get connection_id */
  const uint16_t connection_id = paranoid::stoul( argv[ optind + 2 ] );

  /* average inter-packet delay, reported by receiver */
  uint32_t avg_delay = numeric_limits<uint32_t>::max();

  /* keep the number of fragments per frame */
  vector<uint64_t> cumulative_fpf;
  uint64_t last_acked = numeric_limits<uint64_t>::max();

  /* maximum number of frames to be skipped in a row */
  const size_t MAX_SKIPPED = 3;
  size_t skipped_count = 0;

  if ( not PIXEL_FORMAT_STRS.count( pixel_format ) ) {
    throw runtime_error( "unsupported pixel format" );
  }

  /* camera device */
  Camera camera { 1280, 720, PIXEL_FORMAT_STRS.at( pixel_format ), camera_device };

  /* construct the encoder */
  Encoder base_encoder { camera.display_width(), camera.display_height(),
                         false /* two-pass */, REALTIME_QUALITY };

  const uint32_t initial_state = base_encoder.minihash();

  /* encoded frame index */
  unsigned int frame_no = 0;

  /* latest raster that is received from the input */
  Optional<RasterHandle> last_raster;

  /* where we keep the outputs of parallel encoding jobs */
  vector<EncodeJob> encode_jobs;
  vector<future<EncodeOutput>> encode_outputs;

  /* keep the moving average of encoding times */
  AverageEncodingTime avg_encoding_time;

  /* track the last quantizer used */
  uint8_t last_quantizer = 64;

  /* decoder hash => encoder object */
  deque<uint32_t> encoder_states;
  unordered_map<uint32_t, Encoder> encoders { { initial_state, base_encoder } };

  /* latest state of the receiver, based on ack packets */
  Optional<uint32_t> receiver_last_acked_state;
  Optional<uint32_t> receiver_assumed_state;
  deque<uint32_t> receiver_complete_states;

  /* if the receiver goes into an invalid state, for this amount of seconds,
     we will go into a conservative mode: we only encode based on a known state */
  seconds conservative_for { 5 };
  system_clock::time_point conservative_until = system_clock::now();

  /* :D */
  system_clock::time_point last_sent = system_clock::now();

  /* comment */
  auto encode_start_pipe = UnixDomainSocket::make_pair();
  auto encode_end_pipe = UnixDomainSocket::make_pair();

  Poller poller;

  /* fetch frames from webcam */
  poller.add_action( Poller::Action( encode_start_pipe.second, Direction::In,
    [&]() -> Result {
      encode_start_pipe.second.read();

      last_raster = camera.get_next_frame();

      if ( not last_raster.initialized() ) {
        return { ResultType::Exit, EXIT_FAILURE };
      }

      if ( encode_jobs.size() > 0 ) {
        /* a frame is being encoded now */
        return ResultType::Continue;
      }

      /* let's cleanup the stored encoders based on the lastest ack */
      if ( receiver_last_acked_state.initialized() and
           receiver_last_acked_state.get() != initial_state and
           encoders.count( receiver_last_acked_state.get() ) ) {
        // cleaning up
        auto it = encoder_states.begin();

        while ( it != encoder_states.end() ) {
          if ( *it != receiver_last_acked_state.get() ) {
            encoders.erase( *it );
            it++;
          }
          else {
            break;
          }
        }

        encoder_states.erase( encoder_states.begin(), it );
      }

      RasterHandle raster = last_raster.get();

      uint32_t selected_source_hash = initial_state;

      /* reason about the state of the receiver based on ack messages
       * this is the logic that decides which encoder to use. for example,
       * if the packet loss is huge, we can always select an encoder with a sure
       * state. */

      /* if we're in 'conservative' mode, let's just encode based on something
         we're sure that is available in the receiver */
      if ( system_clock::now() < conservative_until ) {
        if( receiver_complete_states.size() == 0 ) {
          /* and the receiver doesn't have any other states, other than the
             default state */
          selected_source_hash = initial_state;
        }
        else {
          /* the receiver has at least one stored state, let's use it */
          selected_source_hash = receiver_complete_states.back();
        }
      }
      else if ( not receiver_last_acked_state.initialized() ) {
        /* okay, we're not in 'conservative' mode */
        if ( not receiver_assumed_state.initialized() ) {
          /* okay, let's just encode as a keyframe */
          selected_source_hash = initial_state;
        }
        else {
          /* we assume that the receiver is in a right state */
          selected_source_hash = receiver_assumed_state.get();
        }
      }
      else {
        if ( encoders.count( receiver_last_acked_state.get() ) == 0 ) {
          /* it seems that the receiver is in an invalid state */

          /* step 1: let's go into 'conservative' mode; just encode based on a
             known for a while */

          conservative_until = system_clock::now() + conservative_for;

          cerr << "Going into 'conservative' mode for next "
               << conservative_for.count() << "seconds." << endl;

          if( receiver_complete_states.size() == 0 ) {
            /* and the receiver doesn't have any other states, other than the
               default state */
            selected_source_hash = initial_state;
          }
          else {
            /* the receiver has at least one stored state, let's use it */
            selected_source_hash = receiver_complete_states.back();
          }
        }
        else {
          /* we assume that the receiver is in a right state */
          selected_source_hash = receiver_assumed_state.get();
        }
      }
      /* end of encoder selection logic */

      const Encoder & encoder = encoders.at( selected_source_hash );

      const auto increment_quantizer = []( const uint8_t q, const int8_t inc ) -> uint8_t
        {
          int orig = q;
          orig += inc;
          orig = max( 3, orig );
          orig = min( 127, orig );
          return orig;
        };

      /* try various quantizers */
      //encode_jobs.emplace_back( "same", raster, encoder, CONSTANT_QUANTIZER, last_quantizer, 0 );

      // COMMENTED OUT FOR SALSIFY-FOUR
      /* encode_jobs.emplace_back( "improvealittle", raster, encoder, CONSTANT_QUANTIZER,
                                increment_quantizer( last_quantizer, -2 ), 0 ); */

      size_t alpha = max( 5, last_quantizer / 2 );

      encode_jobs.emplace_back( "improve", raster, encoder, CONSTANT_QUANTIZER,
                                increment_quantizer( last_quantizer, -alpha ), 0 );

      /* encode_jobs.emplace_back( "improvemore", raster, encoder, CONSTANT_QUANTIZER,
                                increment_quantizer( last_quantizer, -11 ), 0 ); */

      /* encode_jobs.emplace_back( "improvemuchmore", raster, encoder, CONSTANT_QUANTIZER,
                                increment_quantizer( last_quantizer, -29 ), 0 ); */

      encode_jobs.emplace_back( "fail-small", raster, encoder, CONSTANT_QUANTIZER,
                                increment_quantizer( last_quantizer, alpha ), 0 );

      // COMMENTED OUT FOR SALSIFY-FOUR
      /* encode_jobs.emplace_back( "worsenmore", raster, encoder, CONSTANT_QUANTIZER,
                                increment_quantizer( last_quantizer, +19 ), 0 ); */

      // COMMENTED OUT FOR SALSIFY-FOUR
      /* encode_jobs.emplace_back( "worsenmuchmore", raster, encoder, CONSTANT_QUANTIZER,
                                increment_quantizer( last_quantizer, +37 ), 0 ); */

      /*encode_jobs.emplace_back( "worsenalotmore", raster, encoder, CONSTANT_QUANTIZER,
                                increment_quantizer( last_quantizer, +51 ), 0 ); */

      //encode_jobs.emplace_back( "fail-small", raster, encoder, CONSTANT_QUANTIZER, 127, 0 );

      // this thread will spawn all the encoding jobs and will wait on the results
      thread(
        [&encode_jobs, &encode_outputs, &encode_end_pipe]()
        {
          encode_outputs.clear();
          encode_outputs.reserve( encode_jobs.size() );

          for ( auto & job : encode_jobs ) {
            encode_outputs.push_back( async( launch::async, do_encode_job, move( job ) ) );
          }

          for ( auto & future_res : encode_outputs ) {
            future_res.wait();
          }

          encode_end_pipe.first.write( "1" );
        }
      ).detach();

      return ResultType::Continue;
    } )
  );

  /* all encode jobs have finished */
  poller.add_action( Poller::Action( encode_end_pipe.second, Direction::In,
    [&]()
    {
      /* whatever happens, encode_jobs will be empty after this block is done. */
      auto _ = finally(
        [&]()
        {
          encode_jobs.clear();
          encode_start_pipe.first.write( "1" );
        }
      );

      encode_end_pipe.second.read();

      avg_encoding_time.add( duration_cast<microseconds>( system_clock::now().time_since_epoch() ) );

      if ( not any_of( encode_outputs.cbegin(), encode_outputs.cend(),
                       [&]( const future<EncodeOutput> & o ) { return o.valid(); } ) ) {
        cerr << "All encoding jobs got killed for frame " << frame_no << "\n";
        // no encoding job has ended in time
        return ResultType::Continue;
      }

      /* what is the current capacity of the network,
         now that the encoding is done? */
      size_t frame_size = numeric_limits<size_t>::max();

      if ( avg_delay != numeric_limits<uint32_t>::max() ) {
        frame_size = target_size( avg_delay, last_acked, cumulative_fpf.back() );
      }

      size_t best_output_index = numeric_limits<size_t>::max();
      size_t best_size_diff = numeric_limits<size_t>::max();

      vector<EncodeOutput> good_outputs;

      for ( auto & out_future : encode_outputs ) {
        if ( out_future.valid() ) {
          good_outputs.push_back( move( out_future.get() ) );
        }
      }

      /* choose the best based on the current capacity */
      for ( size_t i = 0; i < good_outputs.size(); i++ ) {
        if ( good_outputs[ i ].frame.size() <= frame_size ) {
          if ( frame_size - good_outputs[ i ].frame.size() < best_size_diff ) {
            best_size_diff = frame_size - good_outputs[ i ].frame.size();
            best_output_index = i;
          }
        }
      }

      if ( best_output_index == numeric_limits<size_t>::max() ) {
        if ( skipped_count < MAX_SKIPPED or good_outputs.back().job_name != "fail-small" ) {
          /* skip frame */
          cerr << "Skipping frame " << frame_no << "\n";
          skipped_count++;
          return ResultType::Continue;
        } else {
          cerr << "Too many skipped frames; sending the bad-quality option on " << frame_no << "\n";
          best_output_index = good_outputs.size() - 1;
          assert( good_outputs[ best_output_index ].job_name == "fail-small" );
        }
      }

      auto output = move( good_outputs[ best_output_index ] );

      uint32_t target_minihash = output.encoder.minihash();

      /*
      cerr << "Sending frame #" << frame_no << " (size=" << output.frame.size() << " bytes, "
           << "source_hash=" << output.source_minihash << ", target_hash="
           << target_minihash << ")...";
      */

      last_quantizer = output.y_ac_qi;

      FragmentedFrame ff { connection_id, output.source_minihash, target_minihash,
                           frame_no,
                           static_cast<uint32_t>( duration_cast<microseconds>( system_clock::now() - last_sent ).count() ),
                           output.frame };
      /* enqueue the packets to be sent */
      /* send 5x faster than packets are being received */
      const unsigned int inter_send_delay = min( 2000u, max( 500u, avg_delay / 5 ) );
      for ( const auto & packet : ff.packets() ) {
        pacer.push( packet.to_string(), inter_send_delay );
      }

      last_sent = system_clock::now();

      cerr << "Frame " << frame_no << " from encoder job " << output.job_name
           << " [" << to_string( output.y_ac_qi ) << "] = "
           << ff.fragments_in_this_frame() << " fragments ("
           << avg_encoding_time.int_value()/1000 << " ms, ssim="
           << output.encoder.stats().ssim.get_or( -1.0 )
           << ") {" << output.source_minihash << " -> " << target_minihash << "}"
           << " intersend_delay = " << inter_send_delay << " us\n";

      cumulative_fpf.push_back( ( frame_no > 0 )
                                ? ( cumulative_fpf[ frame_no - 1 ] + ff.fragments_in_this_frame() )
                                : ff.fragments_in_this_frame() );

      /* now we assume that the receiver will successfully get this */
      receiver_assumed_state.reset( target_minihash );

      encoders.insert( make_pair( target_minihash, move( output.encoder ) ) );
      encoder_states.push_back( target_minihash );

      skipped_count = 0;
      frame_no++;

      return ResultType::Continue;
    } )
  );

  /* new ack from receiver */
  poller.add_action( Poller::Action( socket, Direction::In,
    [&]()
    {
      auto packet = socket.recv();
      AckPacket ack( packet.payload );

      if ( ack.connection_id() != connection_id ) {
        /* this is not an ack for this session! */
        return ResultType::Continue;
      }

      uint64_t this_ack_seq = ack_seq_no( ack, cumulative_fpf );

      if ( last_acked != numeric_limits<uint64_t>::max() and
           this_ack_seq < last_acked ) {
        /* we have already received an ACK newer than this */
        return ResultType::Continue;
      }

      last_acked = this_ack_seq;
      avg_delay = ack.avg_delay();
      receiver_last_acked_state.reset( ack.current_state() );
      receiver_complete_states = move( ack.complete_states() );

      return ResultType::Continue;
    } )
  );

  /* outgoing packet ready to leave the pacer */
  poller.add_action( Poller::Action( socket, Direction::Out, [&]() {
        assert( pacer.ms_until_due() == 0 );

        while ( pacer.ms_until_due() == 0 ) {
          assert( not pacer.empty() );

          socket.send( pacer.front() );
          pacer.pop();
        }

        return ResultType::Continue;
      }, [&]() { return pacer.ms_until_due() == 0; } ) );

  /* kick off the first encode */
  encode_start_pipe.first.write( "1" );

  /* handle events */
  while ( true ) {
    const auto poll_result = poller.poll( pacer.ms_until_due() );
    if ( poll_result.result == Poller::Result::Type::Exit ) {
      return poll_result.exit_status;
    }
  }

  return EXIT_FAILURE;
}
