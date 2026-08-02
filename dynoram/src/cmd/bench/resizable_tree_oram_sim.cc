#include <chrono>
#include <cstddef>
#include <memory>
#include <string>

#include "src/store/ram_store.h"
#include "src/utils/crypto.h"
#include "src/utils/measurements.h"

using duration = std::chrono::duration<double>;

using dyno::crypto::CiphertextLen;
using dyno::crypto::Decrypt;
using dyno::crypto::Encrypt;
using dyno::crypto::GenerateKey;
using dyno::crypto::Key;
using dyno::measurement::klock;
using dyno::measurement::Measurement;
using dyno::store::Store;
using dyno::store::RamStore;

int main(int argc, char **argv) {
  // Assuming N = 2^{30}; Third strategy --> 26 levels, interior bucket size =
  // 36; leaf bucket size = 130. We just simulate enc/dec of the blocks accessed
  // in one access + eviction.

  const int test_count = 10;
  const int eviction_factor = 4;
  const int nl_levels = 25;
  const size_t nl_buck_blocks = 36;
  const size_t l_buck_blocks = 130;
  auto ek = GenerateKey();

  size_t bs = 40;
  if (argc > 1)
    bs = std::stoi(argv[1]);

  size_t nl_buck_bytes = nl_buck_blocks * bs;
  size_t l_buck_bytes = l_buck_blocks * bs;

  auto nlstore = RamStore(nl_levels, CiphertextLen(nl_buck_bytes));
  auto lstore = RamStore(1, CiphertextLen(l_buck_bytes));

  auto ptext_buff = new uint8_t[l_buck_bytes];
  auto ctext_buff = new uint8_t[CiphertextLen(l_buck_bytes)];

  // Setup to ensure successful decryptions in the first round.
  bool ok = Encrypt(ptext_buff, nl_buck_bytes, ek, ctext_buff);
  assert(ok);
  for (int lvl = 0; lvl < nl_levels; ++lvl) {
    ok = nlstore.Write(lvl, ctext_buff);
    assert(ok);
  }
  ok = Encrypt(ptext_buff, l_buck_bytes, ek, ctext_buff);
  assert(ok);
  lstore.Write(0, ctext_buff);
  assert(ok);

  // Run!
  size_t byte_count = 0;
  size_t access_count = 0;
  auto start = klock::now();
  for (int t = 0; t < test_count; ++t) {
    // Path Access
    access_count += 2;
    // Non-leafs
    for (int lvl = 0; lvl < nl_levels; ++lvl) {
      access_count += 2;
      byte_count += 2 * CiphertextLen(nl_buck_bytes);
      // Read
      auto b = nlstore.Read(lvl);
      auto plen = Decrypt(b, CiphertextLen(nl_buck_bytes), ek, ptext_buff);
      assert(plen == nl_buck_bytes);
      // Write Back
      ok = Encrypt(ptext_buff, nl_buck_bytes, ek, ctext_buff);
      assert(ok);
      ok = nlstore.Write(lvl, ctext_buff);
      assert(ok);
    }
    // Leaf
    access_count += 2;
    byte_count += 2 * CiphertextLen(l_buck_bytes);
    // Read
    auto b = lstore.Read(0);
    auto plen = Decrypt(b, CiphertextLen(l_buck_bytes), ek, ptext_buff);
    assert(plen == l_buck_bytes);
    // Write Back
    ok = Encrypt(ptext_buff, l_buck_bytes, ek, ctext_buff);
    assert(ok);
    ok = lstore.Write(0, ctext_buff);
    assert(ok);

    // Eviction
    for (int lvl = 0; lvl < nl_levels - 1; ++lvl) {
      auto ef = eviction_factor > (1 << lvl) ? (1 << lvl) : eviction_factor;
      for (int e = 0; e < ef; ++e) { // Buck to evict from and two children
        access_count += 4; // 2 for this level (r/w), one for next level (r/w).
        for (int c = 0; c < 3; ++c) {
          byte_count += 2 * CiphertextLen(nl_buck_bytes);
          // Read
          b = nlstore.Read((c == 0 ? lvl : lvl + 1));
          plen = Decrypt(b, CiphertextLen(nl_buck_bytes), ek, ptext_buff);
          assert(plen == nl_buck_bytes);
          // Write Back
          ok = Encrypt(ptext_buff, nl_buck_bytes, ek, ctext_buff);
          assert(ok);
          ok = nlstore.Write((c == 0 ? lvl : lvl + 1), ctext_buff);
          assert(ok);
        }
      }
    }

    // Last level:
    for (int e = 0; e < eviction_factor; ++e) {
      access_count += 4;
      // Read
      byte_count += 2 * CiphertextLen(nl_buck_bytes);
      b = nlstore.Read(nl_levels - 1);
      plen = Decrypt(b, CiphertextLen(nl_buck_bytes), ek, ptext_buff);
      assert(plen == nl_buck_bytes);
      // Write Back
      ok = Encrypt(ptext_buff, nl_buck_bytes, ek, ctext_buff);
      assert(ok);
      ok = nlstore.Write(nl_levels - 1, ctext_buff);
      assert(ok);
      for (int c = 0; c < 1; ++c) {
        byte_count += 2 * CiphertextLen(l_buck_bytes);
        b = lstore.Read(0);
        plen = Decrypt(b, CiphertextLen(l_buck_bytes), ek, ptext_buff);
        assert(plen == l_buck_bytes);
        // Write Back
        ok = Encrypt(ptext_buff, l_buck_bytes, ek, ctext_buff);
        assert(ok);
        ok = lstore.Write(0, ctext_buff);
        assert(ok);
      }
    }
  }
  duration d = klock::now() - start;
  Measurement m{klock::now() - start,
                access_count / test_count,
                byte_count / test_count};
  std::cout << m << std::endl;
}
