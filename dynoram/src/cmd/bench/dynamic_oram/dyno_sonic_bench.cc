#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <chrono>
#include <random>
#include "src/dynamic/oram.h"

using namespace dyno::dynamic_stepping_path_oram;

void RunDynoWorkload(ORam& oram, crypto::Key enc_key, const std::string& test_name, int num_ops, int mix_type, bool steady_state = true) {
    // mix_type: 0=Reads, 1=Updates, 2=Inserts, 3=Deletes, 4=Mixed (30% I, 50% R, 10% U, 10% D)
    
    std::cout << "=============================================\n";
    std::cout << test_name << "\n";
    std::cout << "=============================================\n";

    std::mt19937 rng(42);
    size_t cap = oram.Capacity();
    std::uniform_int_distribution<Key> dist_key(1, cap);

    std::vector<ORam::BatchOperation> batch;
    batch.reserve(num_ops);

    for (int i = 0; i < num_ops; ++i) {
        ORam::BatchOperation op;
        Key k = dist_key(rng);
        
        int type_to_use = mix_type;
        if (mix_type == 4) { // Mixed workload
            int r = rng() % 100;
            if (r < 30) type_to_use = 2; // Insert
            else if (r < 80) type_to_use = 0; // Read
            else if (r < 90) type_to_use = 1; // Update
            else type_to_use = 3; // Delete
        }

        op.key = k;
        if (type_to_use == 0) {
            op.type = ORam::OpType::Search;
        } else if (type_to_use == 1) {
            op.type = ORam::OpType::Update;
            op.val = std::make_unique<uint8_t[]>(16); // Assuming val_len = 16
            std::memset(op.val.get(), 1, 16);
        } else if (type_to_use == 2) {
            op.type = ORam::OpType::Insert;
            op.val = std::make_unique<uint8_t[]>(16);
            std::memset(op.val.get(), 2, 16);
        } else if (type_to_use == 3) {
            op.type = ORam::OpType::Delete;
        }

        batch.push_back(std::move(op));
    }

    auto t_start = std::chrono::high_resolution_clock::now();
    
    oram.ExecuteBatch(batch, enc_key, steady_state);
    
    auto t_end = std::chrono::high_resolution_clock::now();
    double total_ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();

    double ops_sec = (num_ops / total_ms) * 1000.0;
    
    std::cout << "CSV FORMAT: TestName,BatchSize,LatencyMs,ThroughputOpsSec\n";
    std::cout << test_name << "," << num_ops << "," << total_ms << "," << std::fixed << std::setprecision(2) << ops_sec << "\n\n";
}

int main(int argc, char** argv) {
    size_t batch_size = 131072;
    if (argc > 1) {
        batch_size = std::stoull(argv[1]);
    }

    std::cout << "Starting DYNO+SONIC benchmark\n";
    
    auto enc_key = dyno::crypto::GenerateKey();
    ORam oram(24, 16); // 2^24 capacity, 16 byte val_len
    
    std::cout << "Initializing ORAM to a baseline state with dummy operations...\n";
    std::vector<ORam::BatchOperation> init_batch;
    for (size_t i = 0; i < 200000; ++i) {
        ORam::BatchOperation op;
        op.type = ORam::OpType::Insert;
        op.key = (i % oram.Capacity()) + 1;
        op.val = std::make_unique<uint8_t[]>(16);
        std::memset(op.val.get(), 0, 16);
        init_batch.push_back(std::move(op));
    }
    oram.ExecuteBatch(init_batch, enc_key);
    std::cout << "Initialization complete.\n\n";

    RunDynoWorkload(oram, enc_key, "TestDynoBurstThroughput", batch_size, 4, false);
    RunDynoWorkload(oram, enc_key, "TestDynoReadsOnly", batch_size, 0);
    RunDynoWorkload(oram, enc_key, "TestDynoUpdatesOnly", batch_size, 1);
    RunDynoWorkload(oram, enc_key, "TestDynoInsertsOnly", batch_size, 2);
    RunDynoWorkload(oram, enc_key, "TestDynoDeletesOnly", batch_size, 3);
    RunDynoWorkload(oram, enc_key, "TestDynoMixedWorkload", batch_size, 4);

    return 0;
}
