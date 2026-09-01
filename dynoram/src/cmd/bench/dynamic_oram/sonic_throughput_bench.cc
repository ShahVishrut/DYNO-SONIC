#include <iostream>
#include <iomanip>
#include <string>
#include "src/dynamic/sonic_adapter.h"

using namespace dyno::dynamic_stepping_path_oram;

void MeasurePlainSonicThroughput(double target_sla_ms) {
    std::cout << "Starting plain SONIC benchmark with target SLA = " << target_sla_ms << " ms\n";
    std::cout << "CSV FORMAT: TestName,BatchSize,LatencyMs,ThroughputOpsSec\n";

    // Need a dummy file or we can use the default adapter constructor.
    // However, SonicORamAdapter expects OpenSSL initialization, which usually happens in main or isn't strictly needed for benchmarking here.
    // Actually, dyno uses OpenSSL in tests but Sonic internally might not require it for pure insert/access testing, it uses ZingORAM.
    // Wait, SonicORamAdapter's constructor uses crypto, so we might need OpenSSL.
    
    // Create the adapter with 2^24 elements (same as throughput_bench.cc).
    size_t capacity = 1ULL << 24;
    SonicORamAdapter adapter(capacity, 16, true);

    // Warmup / Insert some blocks to make the pos_map lookups realistic.
    std::cout << "Initializing ORAM to a baseline state...\n";
    size_t init_batch_size = 200000;
    adapter.SpinlockSonicBenchmark(0, init_batch_size, false);
    std::cout << "Initialization complete. Running tests...\n\n";

    // Helper to find the max batch size that fits in target_sla_ms.
    auto RunBenchmarkType = [&](const std::string& test_name, int work_type, bool steady_state) {
        std::cout << "=============================================\n";
        std::cout << test_name << "\n";
        std::cout << "=============================================\n";

        size_t batch_size = 131072; // Start size
        double last_ms = 0;
        size_t last_batch_size = 0;

        while (true) {
            double current_ms = adapter.SpinlockSonicBenchmark(work_type, batch_size, steady_state);
            std::cout << "    ... Testing batch size: " << batch_size << " -> " << current_ms << " ms\n";

            if (current_ms > target_sla_ms * 1.5) {
                // If it dramatically overshot, interpolate backwards.
                if (last_batch_size > 0 && current_ms > last_ms) {
                    double slope = (current_ms - last_ms) / (batch_size - last_batch_size);
                    batch_size = last_batch_size + (target_sla_ms - last_ms) / slope;
                } else {
                    batch_size /= 2;
                }
            } else if (current_ms > target_sla_ms) {
                // Done.
                double ops_sec = (batch_size / current_ms) * 1000.0;
                std::cout << test_name << "," << batch_size << "," << current_ms << "," << std::fixed << std::setprecision(2) << ops_sec << "\n";
                break;
            } else {
                last_ms = current_ms;
                last_batch_size = batch_size;
                // Scale up linearly based on ratio to target.
                double ratio = target_sla_ms / current_ms;
                batch_size = static_cast<size_t>(batch_size * std::max(1.1, std::min(ratio, 2.0)));
            }
        }
    };

    RunBenchmarkType("TestPlainSonicBurstThroughput", 0, false);
    RunBenchmarkType("TestPlainSonicSteadyAccessThroughput", 1, true);
    RunBenchmarkType("TestPlainSonicSteadyInsertThroughput", 0, true);
}

int main(int argc, char** argv) {
    double target_sla_ms = 2000.0;
    if (argc > 1) {
        target_sla_ms = std::stod(argv[1]);
    }
    MeasurePlainSonicThroughput(target_sla_ms);
    return 0;
}
