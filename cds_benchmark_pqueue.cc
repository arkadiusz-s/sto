#include <cds/init.h>
#include <cds/container/fcpriority_queue.h>
#include <cds/container/mspriority_queue.h>

#include "Transaction.hh"
#include "PriorityQueue.hh"
#include "cds_benchmarks.hh"
#include "randgen.hh"

// run the specified transaction for the particular benchmark
template <typename T>
void do_txn(Tester<T>* tp, Rand transgen) {
    int me = tp->me;
    int bm = tp->bm;
    size_t size = tp->size;
    T* pq = tp->ds;
    auto txn_set = tp->txn_set;

    // randomly select a transaction to run
    auto txn = txn_set[transgen() % txn_set.size()];

    std::uniform_int_distribution<long> slotdist(0, MAX_VALUE);
    int val = 0;
    for (unsigned j = 0; j < txn.size(); ++j) {
        // figure out what type of value to push into the queue
        // based on which benchmark we are running
        switch(bm) {
            case RANDOM:
                val = slotdist(transgen);
                break;
            default: // NOABORTS, DECREASING
                val = --global_val;
                break;
        }
        // avoid shrinking the queue too much
        switch(txn[j]) {
            case push:
                pq->push(val);
                global_thread_push_ctrs[me]++;
                break;
            case pop:
                if (pq->size() < size*.25) {
                    global_thread_skip_ctrs[me]++;
                    break;
                }
                pq->pop();
                global_thread_pop_ctrs[me]++;
                break;
            default:
                assert(0);
                break;
        }
    }
}

void run_benchmark(int bm, int size, std::vector<std::vector<op>> txn_set, int nthreads) {
    global_val = MAX_VALUE;
    
    // initialize all data structures
    PriorityQueue<int> sto_pqueue;
    PriorityQueue<int, true> sto_pqueue_opaque;
    FCPriorityQueue<int> fc_pqueue;
    MSPriorityQueue<int> ms_pqueue(MAX_SIZE);
    for (int i = global_val; i < size; ++i) {
    Sto::start_transaction();
        sto_pqueue.push(global_val);
    assert(Sto::try_commit());
        fc_pqueue.push(global_val);
        ms_pqueue.push(global_val);
        global_val--;
    }

    struct timeval tv1,tv2;
    
    // benchmark FC Pqueue
    gettimeofday(&tv1, NULL);
    startAndWait(&fc_pqueue, CDS, bm, txn_set, size, nthreads);
    gettimeofday(&tv2, NULL);
    //dualprint("CDS: FC Priority Queue \tFinal Size %lu", fc_pqueue.size());
    print_stats(tv1, tv2, bm);
    clear_balance_ctrs();

    // benchmark MS Pqueue
    gettimeofday(&tv1, NULL);
    startAndWait(&ms_pqueue, CDS, bm, txn_set, size, nthreads);
    gettimeofday(&tv2, NULL);
    //dualprint("CDS: MS Priority Queue \tFinal Size %lu", ms_pqueue.size());
    print_stats(tv1, tv2, bm);
    clear_balance_ctrs();

    // benchmark STO
    gettimeofday(&tv1, NULL);
    startAndWait(&sto_pqueue, STO, bm, txn_set, size, nthreads);
    gettimeofday(&tv2, NULL);
    //dualprint("STO: Priority Queue \tFinal Size %d", sto_pqueue.unsafe_size());
    print_stats(tv1, tv2, bm);
    print_abort_stats();
    clear_balance_ctrs();
    
    // benchmark STO w/Opacity
    gettimeofday(&tv1, NULL);
    startAndWait(&sto_pqueue_opaque, STO, bm, txn_set, size, nthreads);
    gettimeofday(&tv2, NULL);
    //dualprint("STO: Priority Queue/O \tFinal Size %d", sto_pqueue_opaque.unsafe_size());
    print_stats(tv1, tv2, bm);
    print_abort_stats();
    clear_balance_ctrs();
	    
    dualprint("\n");
}

int main() {
    dualprint("nthreads: %d, ntxns/thread: %d, max_value: %d, max_size: %d\n", N_THREADS, NTRANS, MAX_VALUE, MAX_SIZE);
    dualprint("FC, MS, STO, STO(O)\n");

    std::ios_base::sync_with_stdio(true);
    assert(CONSISTENCY_CHECK); // set CONSISTENCY_CHECK in Transaction.hh

    cds::Initialize();

    // create the epoch advancer thread
    pthread_t advancer;
    pthread_create(&advancer, NULL, Transaction::epoch_advancer, NULL);
    pthread_detach(advancer);

    /*
    // run the different txn set workloads with different initial sizes
    // test both random and decreasing-only values
    for (auto txn_set = begin(q_txn_sets); txn_set != end(q_txn_sets); ++txn_set) {
        dualprint("\n****************TXN SET %ld*****************\n", txn_set-begin(q_txn_sets));
        for (auto size = begin(sizes); size != end(sizes); ++size) {
            dualprint("\nInit size: %d\n", *size);
            
            // benchmark with random values. pushes can conflict with pops
            dualprint("---------Benchmark: Random-----------\n");
            run_benchmark(RANDOM, *size, *txn_set, N_THREADS);
          
            // benchmark with decreasing values 
            // pushes and pops will never conflict (only pops conflict)
            fprintf(stderr, "---------Benchmark: Decreasing-----------\n");
            fprintf(stdout, "---------Benchmark: Decreasing-----------\n");
            run_benchmark(DECREASING, *size, *txn_set, N_THREADS);
        }
    }
   
    // run the two-thread test where one thread only pushes and the other only pops
    dualprint("\nBenchmark: No Aborts (2 threads: one pushing, one popping)\n");
    for (auto size = begin(sizes); size != end(sizes); ++size) {
        dualprint("Init size: %d\n", *size);
        run_benchmark(NOABORTS, *size, {}, 2);
    }
    */
 
    // run the push-only test (with single-thread all-pops at the end) 
    dualprint("\nBenchmark: Multithreaded Push, Singlethreaded Pops, Random Values\n");
    for (auto n = begin(nthreads_set); n != end(nthreads_set); ++n) {
        fprintf(stderr, "nthreads: %d\n", *n);
        run_benchmark(RANDOM, 100000, q_push_only_txn_set, *n);
    }
    dualprint("\nBenchmark: Multithreaded Push, Singlethreaded Pops, Decreasing Values\n");
    for (auto n = begin(nthreads_set); n != end(nthreads_set); ++n) {
        fprintf(stderr, "nthreads: %d\n", *n);
        run_benchmark(DECREASING, 100000, q_push_only_txn_set, *n);
    }

    // run single-operation txns with different nthreads
    dualprint("\nSingle-Op Txns, Random Values\n");
    for (auto n = begin(nthreads_set); n != end(nthreads_set); ++n) {
        fprintf(stderr, "nthreads: %d\n", *n);
	    fprintf(stderr, "size: 50000\n");
        run_benchmark(RANDOM, 50000, q_single_op_txn_set, *n);
	    fprintf(stderr, "size: 100000\n");
        run_benchmark(RANDOM, 100000, q_single_op_txn_set, *n);
	    fprintf(stderr, "size: 150000\n");
        run_benchmark(RANDOM, 150000, q_single_op_txn_set, *n);
    }
    dualprint("\nSingle-Op Txns, Decreasing Values\n");
    for (auto n = begin(nthreads_set); n != end(nthreads_set); ++n) {
        fprintf(stderr, "nthreads: %d, ", *n);
        run_benchmark(DECREASING, 50000, q_single_op_txn_set, *n);
	    fprintf(stderr, "size: 100000\n");
        run_benchmark(DECREASING, 100000, q_single_op_txn_set, *n);
	    fprintf(stderr, "size: 150000\n");
        run_benchmark(DECREASING, 150000, q_single_op_txn_set, *n);
	    dualprint("\n");
    }

    cds::Terminate();
    return 0;
}
