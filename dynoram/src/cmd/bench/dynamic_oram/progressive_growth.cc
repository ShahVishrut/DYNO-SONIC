#include <chrono>
#include <iostream>
#include <vector>
#include <memory>
#include <cstring>

#include "src/dynamic/oram.h"
#include "src/utils/crypto.h"

using namespace dyno::crypto;
using namespace dyno::dynamic_stepping_path_oram;

bool is_po2(size_t x) {
    return x && !(x & (x - 1));
}

int main(int argc, char **argv) {
    int max_po2 = 20; // 2^20 defaults
    if (argc > 1) {
        max_po2 = std::stoi(argv[1]);
    }
    
    size_t target_inserts = 1ULL << max_po2;
    std::cout << "Starting progressive growth benchmark up to " << target_inserts << " inserts (2^" << max_po2 << ").\n";
    std::cout << "CSV FORMAT: Iteration,TreeCapacity,AvgInsertTimeMs,LastGrowTimeMs,LastSearchTimeMs\n";
    
    auto enc_key = GenerateKey();
    
    // Start at capacity 2
    auto oram = std::make_unique<ORam>(1, 256); 
    
    double total_insert_ms = 0;
    
    for (size_t i = 1; i <= target_inserts; ++i) {
        double grow_ms = 0;
        
        // Trigger Grow if at capacity
        if (oram->Size() == oram->Capacity()) {
            auto start = std::chrono::high_resolution_clock::now();
            oram->Grow(enc_key);
            auto end = std::chrono::high_resolution_clock::now();
            grow_ms = std::chrono::duration<double, std::milli>(end - start).count();
        }
        
        // Prepare dummy payload
        auto v = std::make_unique<uint8_t[]>(256);
        std::memset(v.get(), i % 255, 256);
        
        // Time Insert
        auto start = std::chrono::high_resolution_clock::now();
        oram->Insert(i, std::move(v), enc_key);
        auto end = std::chrono::high_resolution_clock::now();
        double insert_ms = std::chrono::duration<double, std::milli>(end - start).count();
        
        total_insert_ms += insert_ms;
        
        // Print stats at powers of two or every 10,000 inserts
        if (is_po2(i) || i % 10000 == 0 || i == target_inserts) {
            
            // Perform a sample Search to measure time
            start = std::chrono::high_resolution_clock::now();
            size_t search_key = (i / 2) + 1; // ensure > 0
            auto res = oram->Read(search_key, enc_key);
            end = std::chrono::high_resolution_clock::now();
            double search_ms = std::chrono::duration<double, std::milli>(end - start).count();
            
            double avg_insert = total_insert_ms / i;
            
            std::cout << i << "," 
                      << oram->Capacity() << "," 
                      << avg_insert << "," 
                      << grow_ms << ","
                      << search_ms << std::endl;
        }
    }
    
    std::cout << "Done!" << std::endl;
    return 0;
}
