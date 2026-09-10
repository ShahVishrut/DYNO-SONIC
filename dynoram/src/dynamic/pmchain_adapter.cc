#include "src/dynamic/pmchain_adapter.h"
#include "sonic/omap/suboram/pmchain_driver.hpp"
#include "sonic/threads/thread_team.hpp"
#include "sonic/threads/platform/pthread_thread_pool.hpp"
#include "sonic/threads/tuning.hpp"
#include <chrono>
#include <iostream>
#include <random>

namespace dyno::dynamic_stepping_path_oram {

struct PMChainAdapter::Impl {
    sn::threads::thread_context thread_ctx;
    std::unique_ptr<sn::threads::pthread_thread_pool> eviction_pool;
    std::unique_ptr<sn::threads::thread_team> eviction_team;
    
    std::unique_ptr<sn::threads::pthread_thread_pool> access_pool;
    std::unique_ptr<sn::threads::thread_team> access_team;

    using PMDriver = sn::omap::suboram::pmchain::driver<uint64_t, 56>;
    std::unique_ptr<PMDriver> driver;

    Impl(size_t capacity, size_t max_batch_size) {
        thread_ctx.bind_current_thread();
        
        eviction_pool = std::make_unique<sn::threads::pthread_thread_pool>(thread_ctx, 8, "pmchain-evict");
        eviction_team = std::make_unique<sn::threads::thread_team>(eviction_pool->pool(), 8); 

        access_pool = std::make_unique<sn::threads::pthread_thread_pool>(thread_ctx, 8, "pmchain-access");
        access_team = std::make_unique<sn::threads::thread_team>(access_pool->pool(), 8); 

        sn::omap::suboram::pmchain::config cfg{};
        cfg.block_count = capacity;
        cfg.batch_size = ((max_batch_size + 127) / 128) * 128;
        cfg.bucket_real_size = 16;
        cfg.bucket_dummy_size = 16;
        cfg.eviction_rate = 2;
        cfg.routing_depth = 3;
        cfg.evict_batch = 2;
        cfg.access_concurrency = 8;
        cfg.posmap_bucket_size = 64; 

        driver = std::make_unique<PMDriver>(cfg, std::move(*eviction_team), std::move(*access_team));
    }
};

PMChainAdapter::PMChainAdapter(size_t capacity, size_t val_len, size_t max_batch_size)
    : capacity_(capacity), val_len_(val_len), max_batch_size_(((max_batch_size + 63) / 64) * 64), impl_(std::make_unique<Impl>(capacity, max_batch_size_)) {}

PMChainAdapter::~PMChainAdapter() = default;

double PMChainAdapter::RawSonicBenchmark(int work_type, size_t batch_size) {
    return SpinlockSonicBenchmark(work_type, batch_size, false);
}

double PMChainAdapter::SpinlockSonicBenchmark(int work_type, size_t batch_size, bool steady_state) {
    if (batch_size > max_batch_size_) {
        std::cerr << "Batch size exceeds PMChain max_batch_size!" << std::endl;
        std::terminate();
    }

    const size_t aligned_batch_size = ((max_batch_size_ + 127) / 128) * 128;
    std::vector<Impl::PMDriver::operation> ops(aligned_batch_size);
    std::mt19937_64 rng(1337);

    for (size_t i = 0; i < aligned_batch_size; ++i) {
        auto& op = ops[i];
        
        if (i < batch_size) { 
            int op_type = work_type;
            if (work_type == 3) {
                op_type = i % 3;
            }

            op.key = (rng() % capacity_) + 1;
            op.is_dummy = false;
            
            if (op_type == 0 || op_type == 3) { 
                op.is_write = true;
                auto payload = impl_->driver->payload_buffer(i);
                std::fill(payload.begin(), payload.end(), 1);
            } else { 
                op.is_write = false;
            }
        } else { 
            op.key = 0;
            op.is_dummy = true;
            op.is_write = false;
        }
    }

    auto start = std::chrono::high_resolution_clock::now();

    impl_->driver->chain_.populate_requests(sn::util::span<const Impl::PMDriver::operation>(ops.data(), ops.size()));
    auto t1 = std::chrono::high_resolution_clock::now();
    
    impl_->driver->chain_.execute_o2th_chains();
    auto t2 = std::chrono::high_resolution_clock::now();
    
    impl_->driver->chain_.sort_o2th_chains();
    auto t3 = std::chrono::high_resolution_clock::now();
    
    impl_->driver->chain_.execute_oram_queries();
    auto t4 = std::chrono::high_resolution_clock::now();

    impl_->driver->chain_.flush_pending();
    auto t5 = std::chrono::high_resolution_clock::now();

    if (!steady_state && batch_size == 100000) {
        std::cout << "\n[PMChain Breakdown] "
                  << "\n  Populate: " << std::chrono::duration<double, std::milli>(t1 - start).count() << " ms"
                  << "\n  O2TH Build: " << std::chrono::duration<double, std::milli>(t2 - t1).count() << " ms"
                  << "\n  O2TH Sort: " << std::chrono::duration<double, std::milli>(t3 - t2).count() << " ms"
                  << "\n  ORAM Queries: " << std::chrono::duration<double, std::milli>(t4 - t3).count() << " ms"
                  << "\n  Eviction (Flush): " << std::chrono::duration<double, std::milli>(t5 - t4).count() << " ms\n";
    }
    
    return std::chrono::duration<double, std::milli>(t5 - start).count();
}

} // namespace dyno::dynamic_stepping_path_oram
