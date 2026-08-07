#include <chrono>
#include <iostream>
#include <vector>
#include <memory>
#include <cstring>
#include <string>

#include "src/dynamic/oram.h"
#include "src/utils/crypto.h"

using namespace dyno::crypto;
using namespace dyno::dynamic_stepping_path_oram;

const static int batch_size = 128; // Standard batch size

bool is_po2(size_t x) {
    return x && !(x & (x - 1));
}

// Tree allocation happens when `capacity_` (which is i + 1) is a power of 2.
// The user wants to see the allocation iteration, and the 2 before & 2 after it.
// This is equivalent to checking if any value in [i-1, i+3] is a power of 2 >= 4.
bool is_near_po2_alloc(size_t i) {
    for (int offset = -1; offset <= 3; ++offset) {
        size_t test_val = i + offset;
        if (test_val >= 4 && is_po2(test_val)) {
            return true;
        }
    }
    return false;
}

int main(int argc, char **argv) {
    int max_po2 = 20; // 2^20 defaults
    if (argc > 1) {
        max_po2 = std::stoi(argv[1]);
    }
    
    size_t target_inserts = 1ULL << max_po2;
    std::cout << "Starting batched progressive growth benchmark up to " << target_inserts << " inserts (2^" << max_po2 << ").\n";
    std::cout << "CSV FORMAT: Iteration,TreeCapacity,AvgBatchTimeMs,ThisBatchTimeMs,ThisGrowTimeMs,ThisBatchSearchTimeMs\n";
    
    auto enc_key = GenerateKey();
    
    // Start at capacity 2
    auto oram = std::make_unique<ORam>(1, 256); 
    
    double total_batch_ms = 0;
    
    for (size_t i = 1; i <= target_inserts; i += batch_size) {
        double grow_ms = 0;
        
        // Trigger Grow if at capacity
        if (oram->Size() + batch_size > oram->Capacity()) {
            auto start = std::chrono::high_resolution_clock::now();
            oram->Grow(enc_key);
            auto end = std::chrono::high_resolution_clock::now();
            grow_ms = std::chrono::duration<double, std::milli>(end - start).count();
        }
        
        // Prepare Batch
        std::vector<ORam::BatchOperation> batch;
        for (int b = 0; b < batch_size; ++b) {
            ORam::BatchOperation op;
            op.type = ORam::OpType::Insert;
            op.key = i + b;
            
            op.val = std::make_unique<uint8_t[]>(256);
            std::memset(op.val.get(), (i + b) % 255, 256);
            batch.push_back(std::move(op));
        }
        
        // Time Batch Insert
        auto start = std::chrono::high_resolution_clock::now();
        oram->ExecuteBatch(batch, enc_key);
        auto end = std::chrono::high_resolution_clock::now();
        double batch_ms = std::chrono::duration<double, std::milli>(end - start).count();
        
        total_batch_ms += batch_ms;
        
        // Print stats at powers of two, every 10k, or near an allocation
        size_t current_end = i + batch_size - 1;
        if (is_po2(current_end) || current_end % 10000 < batch_size || current_end >= target_inserts || is_near_po2_alloc(current_end)) {
            
            // Perform a sample Search batch to measure time
            std::vector<ORam::BatchOperation> search_batch;
            for (int b = 0; b < batch_size; ++b) {
                ORam::BatchOperation op;
                op.type = ORam::OpType::Search;
                op.key = (current_end / 2) + b + 1; // ensure > 0
                search_batch.push_back(std::move(op));
            }
            
            start = std::chrono::high_resolution_clock::now();
            oram->ExecuteBatch(search_batch, enc_key);
            end = std::chrono::high_resolution_clock::now();
            double search_ms = std::chrono::duration<double, std::milli>(end - start).count();
            
            double avg_batch = total_batch_ms / (i / batch_size + 1);
            
            std::cout << current_end << "," 
                      << oram->Capacity() << "," 
                      << avg_batch << "," 
                      << batch_ms << ","
                      << grow_ms << ","
                      << search_ms << std::endl;
        }
    }
    
    std::cout << "Done!" << std::endl;
    return 0;
}
