#include <stdio.h>
#include <string.h>
#include <assert.h>

#include <unistd.h>
#include <pwd.h>
#include <array>
#include <iostream>
#include <cstring>
#include <fstream>
#include <stdexcept>

using namespace std;
#define MAX_PATH FILENAME_MAX

#include "sgx_urts.h"
#include "sgx_uae_service.h"
#include "App.h"
#include "Enclave_u.h"
#include "OMAP/RAMStoreEnclaveInterface.h"
#include "../Common/Common.h"
#include "OMAP/Node.h"
/*
 * Copyright (C) 2011-2018 Intel Corporation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in
 *     the documentation and/or other materials provided with the
 *     distribution.
 *   * Neither the name of Intel Corporation nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

// This sample is confined to the communication between a SGX client platform
// and an ISV Application Server.

#include <chrono>
#include <stdio.h>
#include <limits.h>
#include <unistd.h>
// Needed for definition of remote attestation messages.
#include "remote_attestation_result.h"

#include "Enclave_u.h"

// Needed to call untrusted key exchange library APIs, i.e. sgx_ra_proc_msg2.
#include "sgx_ukey_exchange.h"

// Needed to get service provider's information, in your real project, you will
// need to talk to real server.
#include "network_ra.h"

// Needed to create enclave and do ecall.
#include "sgx_urts.h"

// Needed to query extended epid group id.
#include "sgx_uae_service.h"

#include "../service_provider/service_provider.h"

#ifndef SAFE_FREE
#define SAFE_FREE(ptr)   \
  {                      \
    if (NULL != (ptr)) { \
      free(ptr);         \
      (ptr) = NULL;      \
    }                    \
  }
#endif

// In addition to generating and sending messages, this application
// can use pre-generated messages to verify the generation of
// messages and the information flow.
#include "sample_messages.h"

#define ENCLAVE_PATH "isv_enclave.signed.so"

uint8_t* msg1_samples[] = {msg1_sample1, msg1_sample2};
uint8_t* msg2_samples[] = {msg2_sample1, msg2_sample2};
uint8_t* msg3_samples[] = {msg3_sample1, msg3_sample2};
uint8_t* attestation_msg_samples[] = {attestation_msg_sample1,
                                      attestation_msg_sample2};

// Some utility functions to output some of the data structures passed between
// the ISV app and the remote attestation service provider.

void PRINT_BYTE_ARRAY(FILE* file, void* mem, uint32_t len) {
  if (!mem || !len) {
    fprintf(file, "\n( null )\n");
    return;
  }
  uint8_t* array = (uint8_t*)mem;
  fprintf(file, "%u bytes:\n{\n", len);
  uint32_t i = 0;
  for (i = 0; i < len - 1; i++) {
    fprintf(file, "0x%x, ", array[i]);
    if (i % 8 == 7) fprintf(file, "\n");
  }
  fprintf(file, "0x%x ", array[i]);
  fprintf(file, "\n}\n");
}

void PRINT_ATTESTATION_SERVICE_RESPONSE(FILE* file,
                                        ra_samp_response_header_t* response) {
  if (!response) {
    fprintf(file, "\t\n( null )\n");
    return;
  }

  fprintf(file, "RESPONSE TYPE:   0x%x\n", response->type);
  fprintf(file, "RESPONSE STATUS: 0x%x 0x%x\n", response->status[0],
          response->status[1]);
  fprintf(file, "RESPONSE BODY SIZE: %u\n", response->size);

  if (response->type == TYPE_RA_MSG2) {
    sgx_ra_msg2_t* p_msg2_body = (sgx_ra_msg2_t*)(response->body);

    fprintf(file, "MSG2 gb - ");
    PRINT_BYTE_ARRAY(file, &(p_msg2_body->g_b), sizeof(p_msg2_body->g_b));

    fprintf(file, "MSG2 spid - ");
    PRINT_BYTE_ARRAY(file, &(p_msg2_body->spid), sizeof(p_msg2_body->spid));

    fprintf(file, "MSG2 quote_type : %hx\n", p_msg2_body->quote_type);

    fprintf(file, "MSG2 kdf_id : %hx\n", p_msg2_body->kdf_id);

    fprintf(file, "MSG2 sign_gb_ga - ");
    PRINT_BYTE_ARRAY(file, &(p_msg2_body->sign_gb_ga),
                     sizeof(p_msg2_body->sign_gb_ga));

    fprintf(file, "MSG2 mac - ");
    PRINT_BYTE_ARRAY(file, &(p_msg2_body->mac), sizeof(p_msg2_body->mac));

    fprintf(file, "MSG2 sig_rl - ");
    PRINT_BYTE_ARRAY(file, &(p_msg2_body->sig_rl), p_msg2_body->sig_rl_size);
  } else if (response->type == TYPE_RA_ATT_RESULT) {
    sample_ra_att_result_msg_t* p_att_result =
        (sample_ra_att_result_msg_t*)(response->body);
    fprintf(file, "ATTESTATION RESULT MSG platform_info_blob - ");
    PRINT_BYTE_ARRAY(file, &(p_att_result->platform_info_blob),
                     sizeof(p_att_result->platform_info_blob));

    fprintf(file, "ATTESTATION RESULT MSG mac - ");
    PRINT_BYTE_ARRAY(file, &(p_att_result->mac), sizeof(p_att_result->mac));

    fprintf(file, "ATTESTATION RESULT MSG secret.payload_tag - %u bytes\n",
            p_att_result->secret.payload_size);

    fprintf(file, "ATTESTATION RESULT MSG secret.payload - ");
    PRINT_BYTE_ARRAY(file, p_att_result->secret.payload,
                     p_att_result->secret.payload_size);
  } else {
    fprintf(file,
            "\nERROR in printing out the response. "
            "Response of type not supported %d\n",
            response->type);
  }
}

/* Global EID shared by multiple threads */
sgx_enclave_id_t global_eid = 0;

typedef struct _sgx_errlist_t {
  sgx_status_t err;
  const char* msg;
  const char* sug; /* Suggestion */
} sgx_errlist_t;

/* Error code returned by sgx_create_enclave */
static sgx_errlist_t sgx_errlist[] = {
    {SGX_ERROR_UNEXPECTED, "Unexpected error occurred.", NULL},
    {SGX_ERROR_INVALID_PARAMETER, "Invalid parameter.", NULL},
    {SGX_ERROR_OUT_OF_MEMORY, "Out of memory.", NULL},
    {SGX_ERROR_ENCLAVE_LOST, "Power transition occurred.",
     "Please refer to the sample \"PowerTransition\" for details."},
    {SGX_ERROR_INVALID_ENCLAVE, "Invalid enclave image.", NULL},
    {SGX_ERROR_INVALID_ENCLAVE_ID, "Invalid enclave identification.", NULL},
    {SGX_ERROR_INVALID_SIGNATURE, "Invalid enclave signature.", NULL},
    {SGX_ERROR_OUT_OF_EPC, "Out of EPC memory.", NULL},
    {SGX_ERROR_NO_DEVICE, "Invalid SGX device.",
     "Please make sure SGX module is enabled in the BIOS, and install SGX "
     "driver afterwards."},
    {SGX_ERROR_MEMORY_MAP_CONFLICT, "Memory map conflicted.", NULL},
    {SGX_ERROR_INVALID_METADATA, "Invalid enclave metadata.", NULL},
    {SGX_ERROR_DEVICE_BUSY, "SGX device was busy.", NULL},
    {SGX_ERROR_INVALID_VERSION, "Enclave version was invalid.", NULL},
    {SGX_ERROR_INVALID_ATTRIBUTE, "Enclave was not authorized.", NULL},
    {SGX_ERROR_ENCLAVE_FILE_ACCESS, "Can't open enclave file.", NULL},
    {SGX_ERROR_NDEBUG_ENCLAVE,
     "The enclave is signed as product enclave, and can not be created as "
     "debuggable enclave.",
     NULL},
};

/* Check error conditions for loading enclave */
void print_error_message(sgx_status_t ret) {
  size_t idx = 0;
  size_t ttl = sizeof sgx_errlist / sizeof sgx_errlist[0];

  for (idx = 0; idx < ttl; idx++) {
    if (ret == sgx_errlist[idx].err) {
      if (NULL != sgx_errlist[idx].sug)
        printf("Info: %s\n", sgx_errlist[idx].sug);
      printf("Error: %s\n", sgx_errlist[idx].msg);
      break;
    }
  }

  if (idx == ttl) printf("Error: Unexpected error occurred.\n");
}

/* Initialize the enclave:
 *   Call sgx_create_enclave to initialize an enclave instance
 */
int initialize_enclave(void) {
  sgx_launch_token_t token = {0};
  sgx_status_t ret = SGX_ERROR_UNEXPECTED;
  int updated = 0;

  /* Call sgx_create_enclave to initialize an enclave instance */
  /* Debug Support: set 2nd parameter to 1 */
  ret = sgx_create_enclave(ENCLAVE_FILENAME, SGX_DEBUG_FLAG, &token, &updated,
                           &global_eid, NULL);
  if (ret != SGX_SUCCESS) {
    print_error_message(ret);
    return -1;
  }

  return 0;
}

std::string exec(const char* cmd) {
  char buffer[128];
  std::string result = "";
  FILE* pipe = popen(cmd, "r");
  if (!pipe) throw std::runtime_error("popen() failed!");
  try {
    while (fgets(buffer, sizeof buffer, pipe) != NULL) {
      result += buffer;
    }
  } catch (...) {
    pclose(pipe);
    throw;
  }
  pclose(pipe);
  return result;
}

/* Application entry */
#define _T(x) x

void bitonicSort(vector<Node*>* nodes, int step);
void bitonic_sort(vector<Node*>* nodes, int low, int n, int dir, int step);
void bitonic_merge(vector<Node*>* nodes, int low, int n, int dir, int step);
void compare_and_swap(Node* item_i, Node* item_j, int dir, int step);
int greatest_power_of_two_less_than(int n);

void bitonicSort(vector<Node*>* nodes, int step) {
  int len = nodes->size();
  bitonic_sort(nodes, 0, len, 1, step);
}

void bitonic_sort(vector<Node*>* nodes, int low, int n, int dir, int step) {
  if (n > 1) {
    int middle = n / 2;
    bitonic_sort(nodes, low, middle, !dir, step);
    bitonic_sort(nodes, low + middle, n - middle, dir, step);
    bitonic_merge(nodes, low, n, dir, step);
  }
}

void bitonic_merge(vector<Node*>* nodes, int low, int n, int dir, int step) {
  if (n > 1) {
    int m = greatest_power_of_two_less_than(n);

    for (int i = low; i < (low + n - m); i++) {
      if (i != (i + m)) {
        compare_and_swap((*nodes)[i], (*nodes)[i + m], dir, step);
      }
    }

    bitonic_merge(nodes, low, m, dir, step);
    bitonic_merge(nodes, low + m, n - m, dir, step);
  }
}

void compare_and_swap(Node* item_i, Node* item_j, int dir, int step) {
  if (step == 1) {
    if (dir == (item_i->key > item_j->key ? 1 : 0)) {
      std::swap(*item_i, *item_j);
    }
  } else {
    if (dir == (item_i->evictionNode > item_j->evictionNode ? 1 : 0)) {
      std::swap(*item_i, *item_j);
    }
  }
}

int greatest_power_of_two_less_than(int n) {
  int k = 1;
  while (k > 0 && k < n) {
    k = k << 1;
  }
  return k >> 1;
}

int permutationIterator = 0;
unsigned long long indx = 1;
map<unsigned long long, unsigned long long> permutation;

int sortedArrayToBST(vector<Node*>* nodes, long long start, long long end,
                     unsigned long long& pos, Bid& node) {
  if (start > end) {
    pos = -1;
    node = 0;
    return 0;
  }

  unsigned long long mid = (start + end) / 2;
  Node* root = (*nodes)[mid];

  int leftHeight =
      sortedArrayToBST(nodes, start, mid - 1, root->leftPos, root->leftID);

  int rightHeight =
      sortedArrayToBST(nodes, mid + 1, end, root->rightPos, root->rightID);
  root->pos = permutation[permutationIterator];
  permutationIterator++;
  root->height = max(leftHeight, rightHeight) + 1;
  pos = root->pos;
  node = root->key;
  return root->height;
}

void initializeORAM(long long maxSize, bytes<Key> secretkey, Bid& rootKey,
                    unsigned long long& rootPos, map<Bid, string>* pairs,
                    vector<long long>* indexes, vector<block>* ciphertexts,
                    size_t& blockCount, unsigned long long& storeBlockSize) {
  int depth = (int)(ceil(log2(maxSize)) - 1) + 1;
  int maxOfRandom = (long long)(pow(2, depth));

  int j = 0;
  int cnt = 0;
  for (int i = 0; i < maxOfRandom * 4; i++) {
    if (i % 1000000 == 0) {
      printf("%d/%d\n", i, maxOfRandom * 4);
    }
    if (cnt == 4) {
      j++;
      cnt = 0;
    }
    permutation[i] = (j + 1) % maxOfRandom;
    cnt++;
  }

  vector<Node*> nodes;
  for (auto pair : (*pairs)) {
    Node* node = new Node();
    node->key = pair.first;
    node->index = indx++;
    std::fill(node->value.begin(), node->value.end(), 0);
    std::copy(pair.second.begin(), pair.second.end(), node->value.begin());
    node->leftID = 0;
    node->leftPos = -1;
    node->rightPos = -1;
    node->rightID = 0;
    node->pos = 0;
    node->isDummy = false;
    node->height = 1;  // new node is initially added at leaf
    nodes.push_back(node);
  }

  bitonicSort(&nodes, 1);
  printf("Creating BST of %d Nodes\n", nodes.size());
  sortedArrayToBST(&nodes, 0, nodes.size() - 1, rootPos, rootKey);
  printf("Inserting in ORAM\n");

  int size = (int)nodes.size();
  for (int i = size; i < maxOfRandom * Z; i++) {
    Bid bid;
    bid = 9223372036854775807 + i;
    Node* node = new Node();
    node->key = bid;
    node->index = indx++;
    std::fill(node->value.begin(), node->value.end(), 0);
    std::copy(string("").begin(), string("").end(), node->value.begin());
    node->leftID = 0;
    node->leftPos = -1;
    node->rightPos = -1;
    node->rightID = 0;
    node->pos = 0;
    node->isDummy = false;
    node->height = 1;  // new node is initially added at leaf
    node->isDummy = true;
    node->pos = permutation[permutationIterator];
    permutationIterator++;
    nodes.push_back(node);
  }

  //----------------------------------------------------------------
  AES::Setup();
  unsigned long long bucketCount = maxOfRandom * 2 - 1;
  unsigned long long INF = 9223372036854775807 - (bucketCount);

  printf("Number of leaves:%lld\n", maxOfRandom);
  printf("depth:%lld\n", depth);

  unsigned long long nextDummyCounter = INF;
  unsigned long long blockSize = sizeof(Node);  // B
  printf("block size is:%d\n", blockSize);
  blockCount = (size_t)(Z * bucketCount);
  storeBlockSize =
      (size_t)(IV + AES::GetCiphertextLength((int)(Z * (blockSize))));
  unsigned long long clen_size = AES::GetCiphertextLength((int)(blockSize)*Z);
  unsigned long long plaintext_size = (blockSize)*Z;
  unsigned long long maxHeightOfAVLTree = (int)floor(log2(blockCount)) + 1;

  unsigned long long first_leaf = bucketCount / 2;

  Bucket* bucket = new Bucket();

  int i;
  printf("Setting Nodes Eviction ID\n");
  for (i = 0; i < nodes.size(); i++) {
    nodes[i]->evictionNode = nodes[i]->pos + first_leaf;
  }

  printf("Sorting\n");
  bitonicSort(&nodes, 2);

  vector<Bucket> buckets;

  long long first_bucket_of_last_level = bucketCount / 2;
  j = 0;

  for (unsigned int i = 0; i < nodes.size(); i++) {
    if (i % 100000 == 0) {
      printf("Creating Buckets:%d/%d\n", i, nodes.size());
    }
    Node* cureNode = nodes[i];
    long long curBucketID = nodes[i]->evictionNode;

    Block& curBlock = (*bucket)[j];
    curBlock.data.resize(blockSize, 0);

    std::array<byte_t, sizeof(Node)> data;

    const byte_t* begin =
        reinterpret_cast<const byte_t*>(std::addressof(*cureNode));
    const byte_t* end = begin + sizeof(Node);
    std::copy(begin, end, std::begin(data));

    block tmp(data.begin(), data.end());

    if (cureNode->isDummy) {
      curBlock.id = 0;
    } else {
      curBlock.id = cureNode->index;
    }
    for (int k = 0; k < tmp.size(); k++) {
      if (cureNode->isDummy == false) {
        curBlock.data[k] = tmp[k];
      }
    }
    delete cureNode;
    j++;

    if (j == Z) {
      (*indexes).push_back(curBucketID);
      buckets.push_back((*bucket));
      delete bucket;
      bucket = new Bucket();
      j = 0;
    }
  }

  for (int i = 0; i < first_bucket_of_last_level; i++) {
    if (i % 100000 == 0) {
      printf("Adding Upper Levels Dummy Buckets:%d/%d\n", i, nodes.size());
    }
    for (int z = 0; z < Z; z++) {
      Block& curBlock = (*bucket)[z];
      curBlock.id = 0;
      curBlock.data.resize(blockSize, 0);
    }
    (*indexes).push_back(i);
    buckets.push_back((*bucket));
    delete bucket;
    bucket = new Bucket();
  }
  delete bucket;

  for (int i = 0; i < (*indexes).size(); i++) {
    block buffer;
    for (int z = 0; z < Z; z++) {
      Block b = buckets[i][z];
      buffer.insert(buffer.end(), b.data.begin(), b.data.end());
    }
    block ciphertext =
        AES::Encrypt(secretkey, buffer, clen_size, plaintext_size);
    (*ciphertexts).push_back(ciphertext);
  }
}

void initializeNodes(bytes<Key> secretkey, map<Bid, string>* pairs,
                     vector<block>* ciphertexts) {
  vector<Node*> nodes;
  for (auto pair : (*pairs)) {
    Node* node = new Node();
    node->key = pair.first;
    node->index = 0;
    std::fill(node->value.begin(), node->value.end(), 0);
    std::copy(pair.second.begin(), pair.second.end(), node->value.begin());
    node->leftID = 0;
    node->leftPos = -1;
    node->rightPos = -1;
    node->rightID = 0;
    node->pos = 0;
    node->isDummy = false;
    node->height = 1;  // new node is initially added at leaf
    nodes.push_back(node);
  }

  unsigned long long blockSize = sizeof(Node);
  unsigned long long clen_size = AES::GetCiphertextLength((int)(blockSize));
  unsigned long long plaintext_size = (blockSize);

  for (int i = 0; i < nodes.size(); i++) {
    std::array<byte_t, sizeof(Node)> data;

    const byte_t* begin =
        reinterpret_cast<const byte_t*>(std::addressof((*nodes[i])));
    const byte_t* end = begin + sizeof(Node);
    std::copy(begin, end, std::begin(data));

    block buffer(data.begin(), data.end());
    block ciphertext =
        AES::Encrypt(secretkey, buffer, clen_size, plaintext_size);
    (*ciphertexts).push_back(ciphertext);
  }
}

int SGX_CDECL main(int argc, char* argv[]) {
  bool check_remote_attestation = false;
  double t;
  /* Initialize the enclave */
  if (initialize_enclave() < 0) {
    printf("Enter a character before exit ...\n");
    getchar();
    return -1;
  }
  /* My Codes */
  int maxSize = 4096;
  if (argc == 2) {
    maxSize = stoi(argv[1]);
  }
  //        ecall_measure_omap_speed(global_eid, &t, maxSize);
  //        ecall_measure_omap_setup_speed(global_eid, &t, maxSize);
  ecall_measure_omap_speed(global_eid, &t, maxSize);
  /* Destroy the enclave */
  sgx_destroy_enclave(global_eid);
  return 0;
}
