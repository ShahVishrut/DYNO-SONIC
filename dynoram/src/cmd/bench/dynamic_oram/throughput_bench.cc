#include <chrono>
#include <iostream>
#include <vector>
#include <memory>
#include <cstring>
#include <string>
#include <random>

#include "src/dynamic/oram.h"
#include "src/utils/crypto.h"
#include "src/dynamic/pmchain_adapter.h"

using namespace dyno::crypto;
using namespace dyno::dynamic_stepping_path_oram;

struct BenchmarkResult {
    size_t batch_size;
    double latency_ms;
    double throughput_ops_sec;
};

BenchmarkResult MeasurePMChainThroughput(
    PMChainAdapter* sonic, 
    int work_type, 
    double target_sla_ms,
    int raw_sonic_mode = 0 
) {
    size_t low = 1;
    size_t high;
    if (raw_sonic_mode == 2) high = 8388608;
    else if (raw_sonic_mode == 1) high = 1048576;
    else high = 32768;
    size_t best_batch = 1;
    double best_latency = 0;

    while (low <= high) {
        size_t mid = low + (high - low) / 2;
        std::cout << "    ... Testing batch size: " << mid << std::flush;
        
        double ms = 0.0;
        if (raw_sonic_mode == 2) {
            ms = sonic->SpinlockSonicBenchmark(work_type, mid);
        } else {
            ms = sonic->RawSonicBenchmark(work_type, mid);
        }
        
        std::cout << " -> " << ms << " ms\n";

        if (ms <= target_sla_ms) {
            best_batch = mid;
            best_latency = ms;
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }

    double throughput = (best_batch / best_latency) * 1000.0;
    return {best_batch, best_latency, throughput};
}





BenchmarkResult MeasureThroughput(
    ORam* oram, 
    dyno::crypto::Key enc_key,
    int work_type, // 0: Insert, 1: Search, 2: Delete, 3: Mixed
    double target_sla_ms
) {
    size_t low = 1;
    size_t high = 8388608; // Increased to 8 million to not artificially cap high throughput
    size_t best_batch = 1;
    double best_latency = 0;

    std::mt19937_64 rng(1337);

    while (low <= high) {
        size_t mid = low + (high - low) / 2;
        std::cout << "    ... Testing batch size: " << mid << std::flush;
        
        std::vector<ORam::BatchOperation> batch;
        for (size_t i = 0; i < mid; ++i) {
            ORam::BatchOperation op;
            if (work_type == 0) op.type = ORam::OpType::Insert;
            else if (work_type == 1) op.type = ORam::OpType::Search;
            else if (work_type == 2) op.type = ORam::OpType::Delete;
            else { 
                if (i % 3 == 0) op.type = ORam::OpType::Insert;
                else if (i % 3 == 1) op.type = ORam::OpType::Search;
                else op.type = ORam::OpType::Delete;
            }
            
            op.key = (rng() % oram->Capacity()) + 1; 
            if (op.type == ORam::OpType::Insert) {
                op.val = std::make_unique<uint8_t[]>(56);
            }
            batch.push_back(std::move(op));
        }

        auto start = std::chrono::high_resolution_clock::now();
        oram->ExecuteBatch(batch, enc_key);
        auto end = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();
        std::cout << " -> " << ms << " ms\n";

        if (ms <= target_sla_ms) {
            best_batch = mid;
            best_latency = ms;
            low = mid + 1; // Try larger batch
        } else {
            high = mid - 1; // SLA exceeded, reduce batch
        }
    }

    double throughput = (best_batch / best_latency) * 1000.0;
    return {best_batch, best_latency, throughput};
}

int main(int argc, char **argv) {
    double target_sla_ms = 1000.0; // Default 1 second SLA
    if (argc > 1) {
        target_sla_ms = std::stod(argv[1]);
    }
    
    std::cout << "Starting throughput benchmark with SLA = " << target_sla_ms << " ms\n";
    std::cout << "CSV FORMAT: Workload,MaxBatchSize,LatencyMs,ThroughputOpsSec\n";
    
    auto enc_key = GenerateKey();
    
    size_t capacity_po2 = 23; // 2^23 = 8,388,608 blocks
    auto oram = std::make_unique<ORam>(capacity_po2, 56); 
    
    // Fill the ORAM to 50% capacity so we don't trigger resizes easily
    std::cout << "Initializing ORAM to 50% capacity...\n";
    size_t target_size = (1ULL << capacity_po2) / 2;
    std::vector<ORam::BatchOperation> init_batch;
    for (size_t i = 1; i <= target_size; ++i) {
        ORam::BatchOperation op;
        op.type = ORam::OpType::Insert;
        op.key = i;
        op.val = std::make_unique<uint8_t[]>(56);
        init_batch.push_back(std::move(op));
        
        if (init_batch.size() >= 16384 || i == target_size) {
            try {
                oram->ExecuteBatch(init_batch, enc_key);
            } catch (const std::exception& e) {
                std::cerr << "[CRITICAL ERROR] Exception during ORAM initialization (batch execution): " << e.what() << std::endl;
                throw;
            } catch (...) {
                std::cerr << "[CRITICAL ERROR] Unknown exception during ORAM initialization!" << std::endl;
                throw;
            }
            init_batch.clear();
            std::cout << "  ... Executed initialization batch, inserted " << i << " blocks" << std::endl;
        }
    }
    std::cout << "Initialization complete. Running tests...\n";

    // 1. 100% Searches (Does not change size)
    std::cout << "=============================================\n";
    std::cout << "Testing PAPER-ONLINE SONIC Interface (Raw Throughput - NO Evictions, Deferred offline, SPINLOCK MODE)\n";
    std::cout << "=============================================\n";
    auto sonic = std::make_unique<dyno::dynamic_stepping_path_oram::PMChainAdapter>(1ULL << capacity_po2, 56, 100000);

    auto res_core_search_spin = MeasurePMChainThroughput(sonic.get(), 1, target_sla_ms, 2);
    std::cout << "[PAPER-ONLINE SPINLOCK] 100% Search," << res_core_search_spin.batch_size << "," << res_core_search_spin.latency_ms << "," << res_core_search_spin.throughput_ops_sec << "\n\n";

    std::cout << "=============================================\n";
    std::cout << "Testing PAPER-ONLINE SONIC Interface (Raw Throughput - NO Evictions, Deferred offline, POLITE MODE)\n";
    std::cout << "=============================================\n";
    auto res_core_search = MeasurePMChainThroughput(sonic.get(), 1, target_sla_ms, 1);
    std::cout << "[PAPER-ONLINE POLITE] 100% Search," << res_core_search.batch_size << "," << res_core_search.latency_ms << "," << res_core_search.throughput_ops_sec << "\n";
    auto res_core_mixed = MeasurePMChainThroughput(sonic.get(), 3, target_sla_ms, 1);
    std::cout << "[PAPER-ONLINE POLITE] Mixed (I/S/D)," << res_core_mixed.batch_size << "," << res_core_mixed.latency_ms << "," << res_core_mixed.throughput_ops_sec << "\n";
    auto res_core_delete = MeasurePMChainThroughput(sonic.get(), 2, target_sla_ms, 1);
    std::cout << "[PAPER-ONLINE POLITE] 100% Delete," << res_core_delete.batch_size << "," << res_core_delete.latency_ms << "," << res_core_delete.throughput_ops_sec << "\n";
    auto res_core_insert = MeasurePMChainThroughput(sonic.get(), 0, target_sla_ms, 1);
    std::cout << "[PAPER-ONLINE POLITE] 100% Insert," << res_core_insert.batch_size << "," << res_core_insert.latency_ms << "," << res_core_insert.throughput_ops_sec << "\n\n";

    std::cout << "\n=============================================\n";
    std::cout << "Testing DYNO-SONIC Interface (High-Level Throughput)\n";
    std::cout << "=============================================\n";

    auto res_search = MeasureThroughput(oram.get(), enc_key, 1, target_sla_ms);
    std::cout << "100% Search," << res_search.batch_size << "," << res_search.latency_ms << "," << res_search.throughput_ops_sec << "\n";

    auto res_mixed = MeasureThroughput(oram.get(), enc_key, 3, target_sla_ms);
    std::cout << "Mixed (I/S/D)," << res_mixed.batch_size << "," << res_mixed.latency_ms << "," << res_mixed.throughput_ops_sec << "\n";

    auto res_delete = MeasureThroughput(oram.get(), enc_key, 2, target_sla_ms);
    std::cout << "100% Delete," << res_delete.batch_size << "," << res_delete.latency_ms << "," << res_delete.throughput_ops_sec << "\n";

    auto res_insert = MeasureThroughput(oram.get(), enc_key, 0, target_sla_ms);
    std::cout << "100% Insert," << res_insert.batch_size << "," << res_insert.latency_ms << "," << res_insert.throughput_ops_sec << "\n";

    return 0;
}
