#include "mpi.h"
#include <iostream>
#include <vector>
#include <glog/logging.h>
#include <gflags/gflags.h>
#include <functional>
#include <unistd.h>

#include "basic_types.hpp"
#include "dvec.hpp"
#include "dc.hpp"


DEFINE_int32(bytes_mb, 32, " send recv bytes to each machine in mb.");
DEFINE_int32(sleep_ms, 0, " sleep in ms to simulate complex operations.");
DEFINE_bool(ring, true, " Ring communication ." );


class Bench {

  struct Atype {
    int x;
    int y;
    Atype () {}
    Atype (int _x, int _y) : x(_x), y(_y) {}
  };
  typedef struct Atype Atype;

  void data_op(Atype & a) {
    a.x += 1;
    a.y += 1;
  }

  public :
  size_t bbytes; /* bench bytes */
  size_t num_ele;
  DistVec<Atype> *send_dvec;
  DistVec<Atype> *recv_dvec;

  DistControl *dc;

  void init_bbytes()
  {
    size_t ele_size = sizeof(Atype);
    size_t tot_bytes = FLAGS_bytes_mb * (1024 * 1024);
    num_ele = tot_bytes / ele_size;
    bbytes = num_ele * ele_size;
  }

  void init()
  {
    init_bbytes();
    /* init dvec */
    std::vector<size_t> part_eles(dc->num_rank);
    std::for_each(part_eles.begin(), part_eles.end(),
      [this](size_t &x){x = num_ele;});
    send_dvec = new DistVec<Atype>(*dc, part_eles, ROLE_ACT);
    send_dvec->init();
    recv_dvec = new DistVec<Atype>(*dc, part_eles, ROLE_PAS);
    recv_dvec->init();
    LOG(INFO) << "Bench init ok. bytes = " << bbytes << " num_ele = " << num_ele;
  }


  Bench(DistControl * _dc) : dc(_dc)
  {
  }

  void execute_chunk(std::vector<Atype> & vec, Range range)
  {
    for (size_t it = range.first; it < range.second; it ++) {
      data_op(vec[it]);
    }
    if ( unlikely(FLAGS_sleep_ms != 0) ) {
      usleep((uint32_t)FLAGS_sleep_ms * 1000);
    }
  }


  void do_bench()
  {
    LOG(INFO) << dc->rank << " Bech Begin .";
    dc->barrier();
    /* post receives */
    recv_dvec->post_receives();
    int num_chan = dc->num_rank;

    /* do work and chunk send */
    for(int chan_cnt = 0; chan_cnt < num_chan; chan_cnt ++) {
      int chan_id;
      if (FLAGS_ring) {
        chan_id = ( dc->rank + chan_cnt) % num_chan;
      } else { 
        chan_id = chan_cnt;
      }

      /* get chunk and do work and send chunk */
      auto &chunk_range = *(send_dvec->chunk_ranges[chan_id]);
      auto &vec = *(send_dvec->loc_data[chan_id]);

      for(size_t chunk_id = 0; chunk_id < chunk_range.size(); chunk_id ++) {
        auto range = chunk_range[chunk_id];
        /* work chunk */
        execute_chunk(vec, range);
        /* transfer chunk */
        send_dvec->do_asend(chan_id, chunk_id);
      }
    }

    LOG(INFO) << " Send OK.-------------";
    /* receive chunk */
    int chan_id ;
    size_t chunk_id;
    while (recv_dvec->wait_any(&chan_id, &chunk_id)) {
      //LOG(INFO) << dc->rank << " -> " << chan_id << " chunk " << chunk_id;
    }

    /* wait finish */
    send_dvec->wait_all();

    /* report perf */
    LOG(INFO) << dc->rank << " METRICS::"  << send_dvec->get_metrics("send_dvec");
    LOG(INFO) << dc->rank << " METRICS::" << recv_dvec->get_metrics("recv_dvec");
  }
};


int main(int argc, char ** argv)
{
  google::ParseCommandLineFlags(&argc, &argv, false);
  google::InitGoogleLogging(argv[0]);
  LOG(INFO) << " Bench async operation performance .";
  LOG(INFO) << " bytes_mb : " << FLAGS_bytes_mb << " tbuf_mb : " << FLAGS_tbuf_mb 
            << " sleep_ms : " << FLAGS_sleep_ms;

  DistControl dc;
  dc.init(argc, argv);

  Bench bench(&dc);
  bench.init();
  bench.do_bench();

  return 0;
}
