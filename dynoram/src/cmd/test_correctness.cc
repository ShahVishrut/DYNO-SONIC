#include <iostream>
#include <cassert>
#include <string>
#include <memory>
#include <cstring>
#include "src/dynamic/oheap.h"
#include "src/dynamic/omap.h"
#include "src/utils/crypto.h"

using namespace dyno::crypto;

void TestOMap() {
    std::cout << "Testing OMap correctness...\n";
    auto enc_key = GenerateKey();
    
    // start with capacity 2^1 = 2
    auto omap = std::make_unique<dyno::dynamic_stepping_path_omap::OMap>(1, 8); 
    
    // Insert some keys
    try {
        for (int i = 1; i <= 10; ++i) {
            std::cout << "Inserting " << i << "\n";
            if (omap->Size() == omap->Capacity()) {
                omap->Grow(enc_key);
            }
        auto v = std::make_unique<uint8_t[]>(8);
        std::memset(v.get(), i * 5, 8);
        omap->Insert(i, std::move(v), enc_key);
    }
    
    // Read and verify
    for (int i = 1; i <= 10; ++i) {
        std::cout << "Reading " << i << "\n";
        auto res = omap->Read(i, enc_key);
        assert(res.get() != nullptr && "Read should return a valid value!");
        if (res.get()[0] != i * 5) {
            std::cerr << "OMap Read mismatch for key " << i << "! Expected " << i * 5 << " but got " << (int)res.get()[0] << "\n";
        }
        assert(res.get()[0] == i * 5 && "Value should match what was inserted!");
    }
    
    // Test ReadAndRemove
    for (int i = 10; i >= 1; --i) {
        auto res = omap->ReadAndRemove(i, enc_key);
        assert(res != nullptr && "Value should not be null!");
        assert(res.get()[0] == i * 5 && "Value should match what was inserted!");
        
        // Ensure it's removed
        auto res2 = omap->Read(i, enc_key);
        // OMap doesn't delete, it just zeroes out the value
        assert(res2.get()[0] == 0 && "Value should be zeroed after deletion!");
    }
    } catch (const std::exception& e) {
        std::cerr << "CAUGHT EXCEPTION: " << e.what() << "\n";
        return;
    }
    
    std::cout << "OMap correctness test passed!\n";
}

void TestOHeap() {
    std::cout << "Testing OHeap correctness...\n";
    auto enc_key = GenerateKey();
    
    // start with capacity 2^1 = 2
    auto oheap = std::make_unique<dyno::dynamic_stepping_path_oheap::OHeap>(1, 8);
    
    // Insert out of order
    int keys[] = {15, 3, 9, 12, 1, 7, 5, 2, 8, 4};
    for (int k : keys) {
        if (oheap->Size() == oheap->Capacity()) {
            oheap->Grow(enc_key);
        }
        auto v = std::make_unique<uint8_t[]>(8);
        std::memset(v.get(), k, 8);
        oheap->Insert(k, std::move(v), enc_key, false);
    }
    
    // Extract min should come out sorted!
    int expected[] = {1, 2, 3, 4, 5, 7, 8, 9, 12, 15};
    for (int exp : expected) {
        oheap->FindMin(enc_key, false); // Required for oblivious priority queue algorithms
        auto res = oheap->ExtractMin(enc_key);
        if (res.meta_.key_ != exp) {
            std::cerr << "OHeap ExtractMin mismatch! Expected " << exp << ", got " << res.meta_.key_ << "\n";
        }
        assert(res.meta_.key_ == exp && "Extracted key should match expected sorted order!");
        assert(res.val_.get()[0] == exp && "Extracted value should match expected value!");
    }
    std::cout << "OHeap tests passed!\n";
}

int main() {
    TestOMap();
    TestOHeap();
    std::cout << "\n✅ All correctness tests passed successfully!\n";
    return 0;
}
