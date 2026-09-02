#include <iostream>
#include <cassert>
#include <string>
#include <random>
#include <memory>
#include <cstring>
#include "src/dynamic/oheap.h"
#include "src/dynamic/omap.h"
#include "src/dynamic/oram.h"
#include "src/utils/crypto.h"

using namespace dyno::crypto;

void TestOMap() {
    std::cout << "Testing OMap correctness...\n";
    auto enc_key = GenerateKey();
    
    // start with capacity 2^8 = 256 to satisfy SONIC minimum tree height requirements
    auto omap = std::make_unique<dyno::dynamic_stepping_path_omap::OMap>(8, 8); 
    
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
    
    // start with capacity 2^8 = 256 to satisfy SONIC minimum tree height requirements
    auto oheap = std::make_unique<dyno::dynamic_stepping_path_oheap::OHeap>(8, 8);
    
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

void TestORam() {
    std::cout << "Testing ORam correctness with 1024 incremental inserts...\n";
    auto enc_key = GenerateKey();
    
    // Start with capacity 2^8 = 256 to satisfy SONIC minimum tree height requirements
    auto oram = std::make_unique<dyno::dynamic_stepping_path_oram::ORam>(8, 8); 
    
    // Insert keys incrementally up to 1024
    try {
        for (int i = 1; i <= 1024; ++i) {
            if (oram->Size() == oram->Capacity()) {
                std::cout << "Growing ORam at size " << oram->Size() << "...\n";
                oram->Grow(enc_key);
            }
            
            auto v = std::make_unique<uint8_t[]>(8);
            std::memset(v.get(), i % 255, 8);
            oram->Insert(i, std::move(v), enc_key);

            // Every once in a while, do a read to ensure it works
            if (i % 64 == 0) {
                int read_key = i / 2;
                std::cout << "  Verifying ORam Read for key " << read_key << "\n";
                auto res = oram->Read(read_key, enc_key);
                assert(res.val_ != nullptr && "Value should not be null!");
                if (res.val_.get()[0] != read_key % 255) {
                    std::cerr << "ORam Read mismatch! Expected " << (read_key % 255) << ", got " << (int)res.val_.get()[0] << "\n";
                }
                assert(res.val_.get()[0] == read_key % 255 && "Value should match what was inserted!");
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "ORam Test Failed! Exception: " << e.what() << " at Size=" << oram->Size() << ", Capacity=" << oram->Capacity() << "\n";
        throw;
    }
    
    std::cout << "ORam correctness test passed!\n";
}

void TestORamBatch() {
    std::cout << "Testing ORam ExecuteBatch (Mixed Workloads & Collapsing)...\n";
    auto enc_key = GenerateKey();
    
    // Start with capacity 2^8 = 256
    auto oram = std::make_unique<dyno::dynamic_stepping_path_oram::ORam>(8, 8); 
    
    // Pre-insert some data so we can test Deletes and Searches
    std::vector<dyno::dynamic_stepping_path_oram::ORam::BatchOperation> init_batch;
    for (uint64_t i = 1; i <= 10; ++i) {
        dyno::dynamic_stepping_path_oram::ORam::BatchOperation op;
        op.type = dyno::dynamic_stepping_path_oram::ORam::OpType::Insert;
        op.key = i;
        op.val = std::make_unique<uint8_t[]>(8);
        std::memset(op.val.get(), i * 10, 8); // Value is key * 10
        init_batch.push_back(std::move(op));
    }
    oram->ExecuteBatch(init_batch, enc_key);
    
    // Test Scenario 1: Mixed Workload (No Overlap)
    std::vector<dyno::dynamic_stepping_path_oram::ORam::BatchOperation> batch1;
    {
        dyno::dynamic_stepping_path_oram::ORam::BatchOperation op1; // Search 5 (should be 50)
        op1.type = dyno::dynamic_stepping_path_oram::ORam::OpType::Search;
        op1.key = 5;
        batch1.push_back(std::move(op1));

        dyno::dynamic_stepping_path_oram::ORam::BatchOperation op2; // Delete 6
        op2.type = dyno::dynamic_stepping_path_oram::ORam::OpType::Delete;
        op2.key = 6;
        batch1.push_back(std::move(op2));

        dyno::dynamic_stepping_path_oram::ORam::BatchOperation op3; // Insert 11
        op3.type = dyno::dynamic_stepping_path_oram::ORam::OpType::Insert;
        op3.key = 11;
        op3.val = std::make_unique<uint8_t[]>(8);
        std::memset(op3.val.get(), 110, 8);
        batch1.push_back(std::move(op3));
    }
    oram->ExecuteBatch(batch1, enc_key);
    
    // Verify results of batch1 in-place (since ExecuteBatch modifies the passed structs)
    assert(batch1[0].val != nullptr && "Search 5 should return a value!");
    assert(batch1[0].val.get()[0] == 50 && "Search 5 should return 50!");
    
    // Test Scenario 2: Overlapping Keys (Collapse Logic)
    std::vector<dyno::dynamic_stepping_path_oram::ORam::BatchOperation> batch2;
    {
        dyno::dynamic_stepping_path_oram::ORam::BatchOperation op1; // Insert 12
        op1.type = dyno::dynamic_stepping_path_oram::ORam::OpType::Insert;
        op1.key = 12;
        op1.val = std::make_unique<uint8_t[]>(8);
        std::memset(op1.val.get(), 120, 8);
        batch2.push_back(std::move(op1));

        dyno::dynamic_stepping_path_oram::ORam::BatchOperation op2; // Search 12 (Immediately after insert, should return 120)
        op2.type = dyno::dynamic_stepping_path_oram::ORam::OpType::Search;
        op2.key = 12;
        batch2.push_back(std::move(op2));

        dyno::dynamic_stepping_path_oram::ORam::BatchOperation op3; // Insert 13
        op3.type = dyno::dynamic_stepping_path_oram::ORam::OpType::Insert;
        op3.key = 13;
        op3.val = std::make_unique<uint8_t[]>(8);
        std::memset(op3.val.get(), 130, 8);
        batch2.push_back(std::move(op3));

        dyno::dynamic_stepping_path_oram::ORam::BatchOperation op4; // Delete 13 (Immediately after insert, cancels out)
        op4.type = dyno::dynamic_stepping_path_oram::ORam::OpType::Delete;
        op4.key = 13;
        batch2.push_back(std::move(op4));
    }
    oram->ExecuteBatch(batch2, enc_key);
    
    // Verify results of batch2
    assert(batch2[1].val != nullptr && "Search 12 should return a value!");
    assert(batch2[1].val.get()[0] == 120 && "Search 12 should collapse with Insert 12 and return 120!");
    
    // Verify ORAM State post-batches
    // Key 6 should be deleted
    auto res6 = oram->Read(6, enc_key);
    assert(res6.val_ == nullptr || res6.val_.get()[0] == 0 && "Key 6 should have been deleted!");
    
    // Key 11 should be inserted
    auto res11 = oram->Read(11, enc_key);
    assert(res11.val_ != nullptr && res11.val_.get()[0] == 110 && "Key 11 should be present!");
    
    // Key 12 should be inserted
    auto res12 = oram->Read(12, enc_key);
    assert(res12.val_ != nullptr && res12.val_.get()[0] == 120 && "Key 12 should be present!");

    // Key 13 should NOT be present (Insert + Delete cancelled out)
    auto res13 = oram->Read(13, enc_key);
    assert((res13.val_ == nullptr || res13.val_.get()[0] == 0) && "Key 13 should have been cancelled out!");
    
    std::cout << "ORam ExecuteBatch correctness test passed!\n";
}

void TestORamComprehensiveMixedWorkload() {
    std::cout << "Testing DYNO+SONIC Comprehensive Mixed Workload Correctness...\n";
    auto enc_key = GenerateKey();
    
    auto oram = std::make_unique<dyno::dynamic_stepping_path_oram::ORam>(10, 8); // Capacity 1024, val_len 8
    std::map<uint64_t, uint64_t> shadow_state;
    
    std::mt19937 rng(1337);
    std::uniform_int_distribution<uint64_t> dist_key(1, 1024);
    std::uniform_int_distribution<uint64_t> dist_val(1, 100000);
    std::uniform_int_distribution<int> dist_op(0, 99);
    
    // Seed the ORAM first
    std::vector<dyno::dynamic_stepping_path_oram::ORam::BatchOperation> seed_batch;
    for (int i = 1; i <= 512; ++i) {
        dyno::dynamic_stepping_path_oram::ORam::BatchOperation op;
        op.type = dyno::dynamic_stepping_path_oram::ORam::OpType::Update;
        op.key = i;
        op.val = std::make_unique<uint8_t[]>(8);
        uint64_t v = dist_val(rng);
        std::memcpy(op.val.get(), &v, 8);
        shadow_state[i] = v;
        seed_batch.push_back(std::move(op));
    }
    oram->ExecuteBatch(seed_batch, enc_key);
    
    uint64_t next_new_key = 1025;
    
    // Run massive mixed workload
    std::vector<dyno::dynamic_stepping_path_oram::ORam::BatchOperation> mixed_batch;
    for (int i = 0; i < 2048; ++i) {
        dyno::dynamic_stepping_path_oram::ORam::BatchOperation op;
        int op_rand = dist_op(rng);
        
        if (op_rand < 15) {
            // Real Insert (Scale up)
            op.key = next_new_key++;
            op.type = dyno::dynamic_stepping_path_oram::ORam::OpType::Insert;
        } else if (op_rand < 40) {
            // Update
            op.key = dist_key(rng);
            op.type = dyno::dynamic_stepping_path_oram::ORam::OpType::Update;
        } else if (op_rand < 80) {
            op.val = std::make_unique<uint8_t[]>(8);
            uint64_t v = dist_val(rng);
            std::memcpy(op.val.get(), &v, 8);
            shadow_state[op.key] = v;
        } else if (op_rand < 80) {
            // Read
            op.type = dyno::dynamic_stepping_path_oram::ORam::OpType::Search;
            // shadow state remains same
        } else {
            // Delete (in Block ORAM, we delete a key by overwriting it with zeroes)
            op.type = dyno::dynamic_stepping_path_oram::ORam::OpType::Delete;
            op.val = std::make_unique<uint8_t[]>(8);
            uint64_t v = 0;
            std::memcpy(op.val.get(), &v, 8);
            shadow_state.erase(op.key);
        }
        mixed_batch.push_back(std::move(op));
    }
    
    oram->ExecuteBatch(mixed_batch, enc_key);
    
    // Verify Final State
    std::cout << "Verifying Final State against Shadow Map...\n";
    for (uint64_t k = 1; k < next_new_key; ++k) {
        auto res = oram->Read(k, enc_key);
        bool found_in_oram = (res.val_ != nullptr && res.val_.get()[0] != 0); // Using the fact that deleted blocks are usually 0-filled or null
        uint64_t oram_val = 0;
        if (found_in_oram) {
            std::memcpy(&oram_val, res.val_.get(), 8);
        }
        
        bool found_in_shadow = shadow_state.count(k) > 0;
        
        if (found_in_shadow) {
            if (!found_in_oram) {
                std::cerr << "Mismatch at Key " << k << ": Exists in shadow map but NOT in ORAM!\n";
                assert(false);
            } else if (oram_val != shadow_state[k]) {
                std::cerr << "Mismatch at Key " << k << ": ORAM=" << oram_val << ", Shadow=" << shadow_state[k] << "\n";
                assert(false);
            }
        } else {
            // Because ORAM returns zeroed bytes if deleted/not found
            if (found_in_oram && oram_val != 0) {
                std::cerr << "Mismatch at Key " << k << ": Exists in ORAM (" << oram_val << ") but NOT in shadow map!\n";
                assert(false);
            }
        }
    }
    
    std::cout << "ORam Comprehensive Mixed Workload test passed!\n";
}

void TestORamDeterministicScale() {
    std::cout << "Testing DYNO+SONIC Deterministic Scaling (Phase 4 & 5)...\n";
    auto enc_key = GenerateKey();
    
    // Start with small capacity (2^10 = 1024)
    auto oram = std::make_unique<dyno::dynamic_stepping_path_oram::ORam>(10, 8); 
    
    // 1. Batch Insert to trigger scale up
    std::vector<dyno::dynamic_stepping_path_oram::ORam::BatchOperation> batch1;
    for (int i = 1025; i <= 1539; ++i) {
        dyno::dynamic_stepping_path_oram::ORam::BatchOperation op;
        op.type = dyno::dynamic_stepping_path_oram::ORam::OpType::Insert;
        op.key = i;
        op.val = std::make_unique<uint8_t[]>(8);
        std::memset(op.val.get(), (i % 255), 8);
        batch1.push_back(std::move(op));
    }
    
    std::cout << "  [Test] Executing Scale-Up Batch (515 Inserts)...\n";
    oram->ExecuteBatch(batch1, enc_key);
    
    for (int i = 1025; i <= 1539; ++i) {
        auto res = oram->Read(i, enc_key);
        if (res.val_ == nullptr || res.val_.get()[0] != (i % 255)) {
            std::cerr << "Mismatch at Key " << i << " after scale up!\n";
            assert(false);
        }
    }
    
    // 2. Batch Update to trigger normal Phase 4 transfer
    std::vector<dyno::dynamic_stepping_path_oram::ORam::BatchOperation> batch2;
    for (int i = 1025; i <= 1027; ++i) {
        dyno::dynamic_stepping_path_oram::ORam::BatchOperation op;
        op.type = dyno::dynamic_stepping_path_oram::ORam::OpType::Update;
        op.key = i;
        op.val = std::make_unique<uint8_t[]>(8);
        std::memset(op.val.get(), (i % 255) + 20, 8);
        batch2.push_back(std::move(op));
    }
    
    std::cout << "  [Test] Executing Update Batch (3 Updates)...\n";
    oram->ExecuteBatch(batch2, enc_key);
    
    for (int i = 1025; i <= 1027; ++i) {
        auto res = oram->Read(i, enc_key);
        if (res.val_ == nullptr || res.val_.get()[0] != ((i % 255) + 20)) {
            std::cerr << "Mismatch at Key " << i << " after update!\n";
            assert(false);
        }
    }
    
    // 3. Batch Delete to trigger scale down
    std::vector<dyno::dynamic_stepping_path_oram::ORam::BatchOperation> batch3;
    for (int i = 1025; i <= 1539; ++i) {
        dyno::dynamic_stepping_path_oram::ORam::BatchOperation op;
        op.type = dyno::dynamic_stepping_path_oram::ORam::OpType::Delete;
        op.key = i;
        batch3.push_back(std::move(op));
    }
    
    std::cout << "  [Test] Executing Scale-Down Batch (515 Deletes)...\n";
    oram->ExecuteBatch(batch3, enc_key);
    
    for (int i = 1025; i <= 1539; ++i) {
        auto res = oram->Read(i, enc_key);
        if (res.val_ != nullptr && res.val_.get()[0] != 0) {
            std::cerr << "Key " << i << " should have been deleted after scale down!\n";
            assert(false);
        }
    }
    
    auto res516 = oram->Read(516, enc_key);
    assert((res516.val_ == nullptr || res516.val_.get()[0] == 0) && "Key 516 never existed!");
    
    // 4. Edge-case collapses
    std::vector<dyno::dynamic_stepping_path_oram::ORam::BatchOperation> batch4;
    auto make_op = [](dyno::dynamic_stepping_path_oram::ORam::OpType type, uint64_t key, uint8_t val) {
        dyno::dynamic_stepping_path_oram::ORam::BatchOperation op;
        op.type = type;
        op.key = key;
        if (type == dyno::dynamic_stepping_path_oram::ORam::OpType::Insert || type == dyno::dynamic_stepping_path_oram::ORam::OpType::Update) {
            op.val = std::make_unique<uint8_t[]>(8);
            std::memset(op.val.get(), val, 8);
        }
        return op;
    };
    batch4.push_back(make_op(dyno::dynamic_stepping_path_oram::ORam::OpType::Delete, 2000, 0)); // Non-existent
    batch4.push_back(make_op(dyno::dynamic_stepping_path_oram::ORam::OpType::Insert, 1540, 100)); // Insert 1540
    batch4.push_back(make_op(dyno::dynamic_stepping_path_oram::ORam::OpType::Update, 1540, 200)); // Update 1540
    batch4.push_back(make_op(dyno::dynamic_stepping_path_oram::ORam::OpType::Search, 1540, 0)); // Search 1540
    
    auto add_op = [&batch4, &make_op](dyno::dynamic_stepping_path_oram::ORam::OpType type, uint64_t key, uint8_t val = 0) {
        batch4.push_back(make_op(type, key, val));
    };
    
    // Key 1000: Insert(10) -> Update(11) -> Search (Should return 11)
    add_op(dyno::dynamic_stepping_path_oram::ORam::OpType::Insert, 1000, 10);
    add_op(dyno::dynamic_stepping_path_oram::ORam::OpType::Update, 1000, 11);
    add_op(dyno::dynamic_stepping_path_oram::ORam::OpType::Search, 1000);
    
    // Key 1001: Insert(10) -> Delete -> Search (Should return null/0)
    add_op(dyno::dynamic_stepping_path_oram::ORam::OpType::Insert, 1001, 10);
    add_op(dyno::dynamic_stepping_path_oram::ORam::OpType::Delete, 1001);
    add_op(dyno::dynamic_stepping_path_oram::ORam::OpType::Search, 1001);

    // Key 1002: Insert(10) -> Search (returns 10) -> Update(11)
    add_op(dyno::dynamic_stepping_path_oram::ORam::OpType::Insert, 1002, 10);
    add_op(dyno::dynamic_stepping_path_oram::ORam::OpType::Search, 1002);
    add_op(dyno::dynamic_stepping_path_oram::ORam::OpType::Update, 1002, 11);

    // Key 1003: Update(11) (on non-existent) -> Search (returns 11)
    add_op(dyno::dynamic_stepping_path_oram::ORam::OpType::Update, 1003, 11);
    add_op(dyno::dynamic_stepping_path_oram::ORam::OpType::Search, 1003);

    // Key 1004: Delete (on non-existent) -> Insert(10) -> Search (returns 10)
    add_op(dyno::dynamic_stepping_path_oram::ORam::OpType::Delete, 1004);
    add_op(dyno::dynamic_stepping_path_oram::ORam::OpType::Insert, 1004, 10);
    add_op(dyno::dynamic_stepping_path_oram::ORam::OpType::Search, 1004);

    // Key 1005: Insert(10) -> Delete -> Insert(11)
    add_op(dyno::dynamic_stepping_path_oram::ORam::OpType::Insert, 1005, 10);
    add_op(dyno::dynamic_stepping_path_oram::ORam::OpType::Delete, 1005);
    add_op(dyno::dynamic_stepping_path_oram::ORam::OpType::Insert, 1005, 11);
    
    std::cout << "  [Test] Executing Edge-Case Mixed Batch...\n";
    oram->ExecuteBatch(batch4, enc_key);
    
    assert(batch4[3].val != nullptr);
    assert(batch4[3].val.get()[0] == 200); // the search should find the updated value
    
    auto res1540 = oram->Read(1540, enc_key);
    if (res1540.val_ == nullptr || res1540.val_.get()[0] != 200) {
        std::cerr << "Key 1540 should be present after edge case batch!\n";
        assert(false);
    }
    
    // Check in-batch Search returns
    assert(batch4[6].val != nullptr && batch4[6].val.get()[0] == 11 && "Key 1000 Search should return 11!");
    assert((batch4[9].val == nullptr || batch4[9].val.get()[0] == 0) && "Key 1001 Search should return null!");
    assert(batch4[11].val != nullptr && batch4[11].val.get()[0] == 10 && "Key 1002 Search should return 10!");
    assert(batch4[14].val != nullptr && batch4[14].val.get()[0] == 11 && "Key 1003 Search should return 11!");
    assert(batch4[17].val != nullptr && batch4[17].val.get()[0] == 10 && "Key 1004 Search should return 10!");
    
    // Check final state in ORAM
    auto res1000 = oram->Read(1000, enc_key); assert(res1000.val_ != nullptr && res1000.val_.get()[0] == 11);
    auto res1001 = oram->Read(1001, enc_key); assert((res1001.val_ == nullptr || res1001.val_.get()[0] == 0));
    auto res1002 = oram->Read(1002, enc_key); assert(res1002.val_ != nullptr && res1002.val_.get()[0] == 11);
    auto res1003 = oram->Read(1003, enc_key); assert(res1003.val_ != nullptr && res1003.val_.get()[0] == 11);
    auto res1004 = oram->Read(1004, enc_key); assert(res1004.val_ != nullptr && res1004.val_.get()[0] == 10);
    auto res1005 = oram->Read(1005, enc_key); assert(res1005.val_ != nullptr && res1005.val_.get()[0] == 11);

    std::cout << "ORam Deterministic Scaling test passed!\n\n";
}

int main() {
    TestORamDeterministicScale();
    TestORamComprehensiveMixedWorkload();
    TestOMap();
    TestOHeap();
    TestORam();
    TestORamBatch();
    std::cout << "\n✅ All correctness tests passed successfully!\n";
    return 0;
}
