#include <chrono>
#include <iostream>
#include <vector>
#include <memory>
#include <cstring>
#include <string>
#include <random>

#include "src/dynamic/oram.h"
#include "src/utils/crypto.h"

using namespace dyno::crypto;
using namespace dyno::dynamic_stepping_path_oram;

struct BenchmarkResult {
    size_t batch_size;
    double latency_ms;
    double throughput_ops_sec;
};


BenchmarkResult MeasureSonicThroughput(
    SonicORamAdapter* sonic, 
    dyno::crypto::Key enc_key,
    int work_type, // 0: Insert, 1: Search, 2: Delete, 3: Mixed
    double target_sla_ms,
    bool raw_sonic_only = false
) {
    size_t low = 1;
    size_t high = 8388608; 
    size_t best_batch = 1;
    double best_latency = 0;

    std::mt19937_64 rng(1337);

    while (low <= high) {
        size_t mid = low + (high - low) / 2;
        std::cout << "    ... Testing batch size: " << mid << std::flush;
        
        std::vector<dyno::static_path_oram::Block> insert_batch;
        std::vector<std::pair<dyno::static_path_oram::Key, bool>> search_delete_batch;
        
        for (size_t i = 0; i < mid; ++i) {
            int op_type = work_type;
            if (work_type == 3) {
                op_type = i % 3;
            }
            
            dyno::static_path_oram::Key key = (rng() % sonic->Capacity()) + 1; 
            
            if (op_type == 0) { // Insert
                dyno::static_path_oram::Block b(true);
                b.meta_.key_ = key;
                b.meta_.pos_ = sonic->GenerateRandomLeaf();
                b.val_ = std::make_unique<uint8_t[]>(256);
                insert_batch.push_back(std::move(b));
            } else {
                search_delete_batch.push_back({key, true});
            }
        }

        double ms = 0.0;
        
        if (raw_sonic_only) {
            // Bypass adapter logic, directly call RawSonicBenchmark
            ms = sonic->RawSonicBenchmark(work_type, mid);
        } else {
            auto start = std::chrono::high_resolution_clock::now();
            if (work_type == 0) {
                sonic->InsertBatch(insert_batch, enc_key);
            } else if (work_type == 1) {
                sonic->ReadBatch(search_delete_batch, enc_key);
            } else if (work_type == 2) {
                sonic->ReadAndRemoveBatch(search_delete_batch, enc_key);
            } else {
                if (!insert_batch.empty()) sonic->InsertBatch(insert_batch, enc_key);
                if (!search_delete_batch.empty()) sonic->ReadAndRemoveBatch(search_delete_batch, enc_key);
            }
            auto end = std::chrono::high_resolution_clock::now();
            ms = std::chrono::duration<double, std::milli>(end - start).count();
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
                op.val = std::make_unique<uint8_t[]>(256);
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
    
    size_t capacity_po2 = 18; // 2^18 = 262,144 blocks
    auto oram = std::make_unique<ORam>(capacity_po2, 256); 
    
    // Fill the ORAM to 50% capacity so we don't trigger resizes easily
    std::cout << "Initializing ORAM to 50% capacity...\n";
    size_t target_size = (1ULL << capacity_po2) / 2;
    std::vector<ORam::BatchOperation> init_batch;
    for (size_t i = 1; i <= target_size; ++i) {
        ORam::BatchOperation op;
        op.type = ORam::OpType::Insert;
        op.key = i;
        op.val = std::make_unique<uint8_t[]>(256);
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
    std::cout << "\n=============================================\n";
    std::cout << "Testing PAPER-ONLINE SONIC Interface (Raw Throughput - NO Evictions, Deferred offline)\n";
    std::cout << "=============================================\n";
    auto sonic = std::make_unique<SonicORamAdapter>(1ULL << capacity_po2, 256, true);

    auto res_core_search = MeasureSonicThroughput(sonic.get(), enc_key, 1, target_sla_ms, true);
    std::cout << "[PAPER-ONLINE] 100% Search," << res_core_search.batch_size << "," << res_core_search.latency_ms << "," << res_core_search.throughput_ops_sec << "\n";

    auto res_core_mixed = MeasureSonicThroughput(sonic.get(), enc_key, 3, target_sla_ms, true);
    std::cout << "[PAPER-ONLINE] Mixed (I/S/D)," << res_core_mixed.batch_size << "," << res_core_mixed.latency_ms << "," << res_core_mixed.throughput_ops_sec << "\n";

    auto res_core_delete = MeasureSonicThroughput(sonic.get(), enc_key, 2, target_sla_ms, true);
    std::cout << "[PAPER-ONLINE] 100% Delete," << res_core_delete.batch_size << "," << res_core_delete.latency_ms << "," << res_core_delete.throughput_ops_sec << "\n";

    auto res_core_insert = MeasureSonicThroughput(sonic.get(), enc_key, 0, target_sla_ms, true);
    std::cout << "[PAPER-ONLINE] 100% Insert," << res_core_insert.batch_size << "," << res_core_insert.latency_ms << "," << res_core_insert.throughput_ops_sec << "\n";

    std::cout << "\n=============================================\n";
    std::cout << "Testing Base SONIC Interface (Raw Throughput - WITH Adapter Linear Scan Overhead)\n";
    std::cout << "=============================================\n";
    
    auto res_sonic_search = MeasureSonicThroughput(sonic.get(), enc_key, 1, target_sla_ms);
    std::cout << "[Adapter] 100% Search," << res_sonic_search.batch_size << "," << res_sonic_search.latency_ms << "," << res_sonic_search.throughput_ops_sec << "\n";

    auto res_sonic_mixed = MeasureSonicThroughput(sonic.get(), enc_key, 3, target_sla_ms);
    std::cout << "[Adapter] Mixed (I/S/D)," << res_sonic_mixed.batch_size << "," << res_sonic_mixed.latency_ms << "," << res_sonic_mixed.throughput_ops_sec << "\n";

    auto res_sonic_delete = MeasureSonicThroughput(sonic.get(), enc_key, 2, target_sla_ms);
    std::cout << "[Adapter] 100% Delete," << res_sonic_delete.batch_size << "," << res_sonic_delete.latency_ms << "," << res_sonic_delete.throughput_ops_sec << "\n";

    auto res_sonic_insert = MeasureSonicThroughput(sonic.get(), enc_key, 0, target_sla_ms);
    std::cout << "[Adapter] 100% Insert," << res_sonic_insert.batch_size << "," << res_sonic_insert.latency_ms << "," << res_sonic_insert.throughput_ops_sec << "\n";

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
