#include "fft_8_1.cl"
#include "fft_8_2.cl"

#pragma OPENCL EXTENSION cl_intel_channels : enable

#include "../host/inc/fft_config.h"

#define min_1(a,b) (a<b?a:b)

#define LOGPOINTS_1       3
#define POINTS_1          (1 << LOGPOINTS_1)
#define NUM_FETCHES_1     (1 << (LOGN - LOGPOINTS_1))

// Log of how much to fetch at once for one area of input buffer.
// LOG_CONT_FACTOR_LIMIT computation makes sure that C_LEN below
// is non-negative. Keep it bounded by 6, as going larger will waste
// on-chip resources but won't give performance gains.
#define LOG_CONT_FACTOR_LIMIT1_1 (LOGN - (2 * (LOGPOINTS_1)))
#define LOG_CONT_FACTOR_LIMIT2_1 (((LOG_CONT_FACTOR_LIMIT1_1) >= 0) ? (LOG_CONT_FACTOR_LIMIT1_1) : 0)
#define LOG_CONT_FACTOR_1        (((LOG_CONT_FACTOR_LIMIT2_1) <= 6) ? (LOG_CONT_FACTOR_LIMIT1_1) : 6)
#define CONT_FACTOR_1            (1 << LOG_CONT_FACTOR_1)

// Need some depth to our channels to accommodate their bursty filling.
channel float2 chanin_1[8] __attribute__((depth(CONT_FACTOR_1*8)));

uint bit_reversed_1(uint x, uint bits) {
  uint y = 0;
  #pragma unroll
  for (uint i = 0; i < bits; i++) {
    y <<= 1;
    y |= x & 1;
    x >>= 1;
  }
  y &= ((1 << bits) - 1);
  return y;
}

// fetch N points as follows:
// - each thread will load 8 consecutive values
// - load CONT_FACTOR consecutive loads (8 values each), then jump by N/8, and load next
//   CONT_FACTOR consecutive values.
// - Once load CONT_FACTOR values starting at 7N/8, send CONT_FACTOR values
//   into the channel to the fft kernel.
// - start process again.
// This way, only need 8xCONT_FACTOR local memory buffer, instead of 8xN.
//
// Group index is used as follows ( 0 to CONT_FACTOR, iteration num )
//
//  64K values = 2^16, num_fetches=2^13, 2^6 = CONT_FACTOR, 2^7=num_Fetches / cont_factor
//
// <   C ><B><  A >
// 5432109876543210
//  A -- fetch within contiguous block
//  B -- B * N/8 region selector
//  C -- num times fetch cont_factor * 8 values (or num times fill the buffer)

// INPUT GID POINTS
// C_LEN must be at least 0. Can't be negative.
#define A_START_1 0
#define A_END_1   (LOG_CONT_FACTOR_1 + LOGPOINTS_1 - 1)

#define B_START_1 (A_END_1 + 1)
#define B_END_1   (B_START_1 + LOGPOINTS_1 - 1)

#define C_START_1 (B_END_1 + 1)
#define C_END_1   (LOGN - 1)

#define D_START_1 (C_END_1 + 1)
#define D_END_1   31

#define A_LEN_1   (A_END_1 - A_START_1 + 1)
#define B_LEN_1   (B_END_1 - B_START_1 + 1)
#define C_LEN_1   (C_END_1 - C_START_1 + 1)
#define D_LEN_1   (D_END_1 - D_START_1 + 1)
#define EXTRACT_1(id,start,len) ((id >> start) & ((1 << len) - 1))

uint permute_gid_1 (uint gid) {
  uint result_1 = 0;
  // result_1[31:16]= gid[31:16] = D_1
  // result_1[15:13] = gid[10:8] = C_1
  // result_1[12:8]  = gid[15:11] = B_1
  // result_1[7:0]  = gid[10:0] = A_1

  uint A_1 = EXTRACT_1(gid, A_START_1, A_LEN_1);
  uint B_1 = EXTRACT_1(gid, B_START_1, B_LEN_1);
  uint C_1 = EXTRACT_1(gid, C_START_1, C_LEN_1);
  uint D_1 = EXTRACT_1(gid, D_START_1, D_LEN_1);

  // swap B and C
  uint new_c_start_1 = A_END_1 + 1;
  uint new_b_start_1 = new_c_start_1 + C_LEN_1;
  result_1 = (D_1 << D_START_1) | (B_1 << new_b_start_1) | (C_1 << new_c_start_1) | (A_1 << A_START_1);
  return result_1;
}

// group dimension (N/(8*CONT_FACTOR), num_iterations)
__attribute__((reqd_work_group_size(CONT_FACTOR_1 * POINTS_1, 1, 1)))
kernel void fetch_1 (global float2 * restrict src) {

  const int N = (1 << LOGN);
  // Each thread will fetch POINTS points. Need POINTS times to pass to FFT.
  const int BUF_SIZE_1 = 1 << (LOG_CONT_FACTOR_1 + LOGPOINTS_1 + LOGPOINTS_1);

  // Local memory for CONT_FACTOR * POINTS points
  local float2 buf_1[BUF_SIZE_1];

  uint iteration = get_global_id(1);
  uint group_per_iter = get_global_id(0);

  // permute global addr but not the local addr
  uint global_addr = iteration * N + group_per_iter;
  global_addr = permute_gid_1 (global_addr << LOGPOINTS_1);
  uint lid = get_local_id(0);
  uint local_addr_1 = lid << LOGPOINTS_1;

  #pragma unroll
  for (uint k = 0; k < POINTS_1; k++) {
    buf_1[local_addr_1 + k] = src[global_addr + k];
  }

  barrier (CLK_LOCAL_MEM_FENCE);

  #pragma unroll
  for (uint k = 0; k < POINTS_1; k++) {
    uint buf_addr = bit_reversed_1(k,3) * CONT_FACTOR_1 * POINTS_1 + lid;
    write_channel_intel (chanin_1[k], buf_1[buf_addr]);
  }
}


/* 'src' and 'dest' point to the input and output buffers in global memory;
 * using restrict pointers as there are no dependencies between the buffers
 * 'count' represents the number of 4k sets to process
 * 'inverse' toggles between the direct and the inverse transform
 */

kernel void fft1d_1(global float2 * restrict dest,
                  int count, int inverse) {

  const int N = (1 << LOGN);

  /* The FFT engine requires a sliding window array for data reordering; data
   * stored in this array is carried across loop iterations and shifted by one
   * element every iteration; all loop dependencies derived from the uses of
   * this array are simple transfers between adjacent array elements
   */

  float2 fft_delay_elements[N + 8 * (LOGN - 2)];

  /* This is the main loop. It runs 'count' back-to-back FFT transforms
   * In addition to the 'count * (N / 8)' iterations, it runs 'N / 8 - 1'
   * additional iterations to drain the last outputs
   * (see comments attached to the FFT engine)
   *
   * The compiler leverages pipeline parallelism by overlapping the
   * iterations of this loop - launching one iteration every clock cycle
   */

  for (unsigned i = 0; i < count * (N / 8) + N / 8 - 1; i++) {

    /* As required by the FFT engine, gather input data from 8 distinct
     * segments of the input buffer; for simplicity, this implementation
     * does not attempt to coalesce memory accesses and this leads to
     * higher resource utilization (see the fft2d example for advanced
     * memory access techniques)
     */

    int base = (i / (N / 8)) * N;
    int offset = i % (N / 8);

    float2x8_1 data;
    // Perform memory transfers only when reading data in range
    if (i < count * (N / 8)) {
      data.i0 = read_channel_intel(chanin_1[0]);
      data.i1 = read_channel_intel(chanin_1[1]);
      data.i2 = read_channel_intel(chanin_1[2]);
      data.i3 = read_channel_intel(chanin_1[3]);
      data.i4 = read_channel_intel(chanin_1[4]);
      data.i5 = read_channel_intel(chanin_1[5]);
      data.i6 = read_channel_intel(chanin_1[6]);
      data.i7 = read_channel_intel(chanin_1[7]);
    } else {
      data.i0 = data.i1 = data.i2 = data.i3 =
                data.i4 = data.i5 = data.i6 = data.i7 = 0;
    }

    // Perform one step of the FFT engine
    data = fft_step_1(data, i % (N / 8), fft_delay_elements, inverse, LOGN);

    /* Store data back to memory. FFT engine outputs are delayed by
     * N / 8 - 1 steps, hence gate writes accordingly
     */

    if (i >= N / 8 - 1) {
      int base = 8 * (i - (N / 8 - 1));

      // These consecutive accesses will be coalesced by the compiler
      dest[base] = data.i0;
      dest[base + 1] = data.i1;
      dest[base + 2] = data.i2;
      dest[base + 3] = data.i3;
      dest[base + 4] = data.i4;
      dest[base + 5] = data.i5;
      dest[base + 6] = data.i6;
      dest[base + 7] = data.i7;
    }
  }
}

#define min(a,b) (a<b?a:b)

#define LOGPOINTS       3
#define POINTS          (1 << LOGPOINTS)
#define NUM_FETCHES     (1 << (LOGN - LOGPOINTS))

// Log of how much to fetch at once for one area of input buffer.
// LOG_CONT_FACTOR_LIMIT computation makes sure that C_LEN below
// is non-negative. Keep it bounded by 6, as going larger will waste
// on-chip resources but won't give performance gains.
#define LOG_CONT_FACTOR_LIMIT1 (LOGN - (2 * (LOGPOINTS)))
#define LOG_CONT_FACTOR_LIMIT2 (((LOG_CONT_FACTOR_LIMIT1) >= 0) ? (LOG_CONT_FACTOR_LIMIT1) : 0)
#define LOG_CONT_FACTOR        (((LOG_CONT_FACTOR_LIMIT2) <= 6) ? (LOG_CONT_FACTOR_LIMIT1) : 6)
#define CONT_FACTOR            (1 << LOG_CONT_FACTOR)

// Need some depth to our channels to accommodate their bursty filling.
channel float2 chanin[8] __attribute__((depth(CONT_FACTOR*8)));

uint bit_reversed(uint x, uint bits) {
  uint y = 0;
  #pragma unroll
  for (uint i = 0; i < bits; i++) {
    y <<= 1;
    y |= x & 1;
    x >>= 1;
  }
  y &= ((1 << bits) - 1);
  return y;
}

// fetch N points as follows:
// - each thread will load 8 consecutive values
// - load CONT_FACTOR consecutive loads (8 values each), then jump by N/8, and load next
//   CONT_FACTOR consecutive values.
// - Once load CONT_FACTOR values starting at 7N/8, send CONT_FACTOR values
//   into the channel to the fft kernel.
// - start process again.
// This way, only need 8xCONT_FACTOR local memory buffer, instead of 8xN.
//
// Group index is used as follows ( 0 to CONT_FACTOR, iteration num )
//
//  64K values = 2^16, num_fetches=2^13, 2^6 = CONT_FACTOR, 2^7=num_Fetches / cont_factor
//
// <   C ><B><  A >
// 5432109876543210
//  A -- fetch within contiguous block
//  B -- B * N/8 region selector
//  C -- num times fetch cont_factor * 8 values (or num times fill the buffer)

// INPUT GID POINTS
// C_LEN must be at least 0. Can't be negative.
#define A_START 0
#define A_END   (LOG_CONT_FACTOR + LOGPOINTS - 1)

#define B_START (A_END + 1)
#define B_END   (B_START + LOGPOINTS - 1)

#define C_START (B_END + 1)
#define C_END   (LOGN - 1)

#define D_START (C_END + 1)
#define D_END   31

#define A_LEN   (A_END - A_START + 1)
#define B_LEN   (B_END - B_START + 1)
#define C_LEN   (C_END - C_START + 1)
#define D_LEN   (D_END - D_START + 1)
#define EXTRACT(id,start,len) ((id >> start) & ((1 << len) - 1))

uint permute_gid (uint gid) {
  uint result = 0;
  // result[31:16]= gid[31:16] = D
  // result[15:13] = gid[10:8] = C
  // result[12:8]  = gid[15:11] = B
  // result[7:0]  = gid[10:0] = A

  uint A = EXTRACT(gid, A_START, A_LEN);
  uint B = EXTRACT(gid, B_START, B_LEN);
  uint C = EXTRACT(gid, C_START, C_LEN);
  uint D = EXTRACT(gid, D_START, D_LEN);

  // swap B and C
  uint new_c_start = A_END + 1;
  uint new_b_start = new_c_start + C_LEN;
  result = (D << D_START) | (B << new_b_start) | (C << new_c_start) | (A << A_START);
  return result;
}

// group dimension (N/(8*CONT_FACTOR), num_iterations)
__attribute__((reqd_work_group_size(CONT_FACTOR * POINTS, 1, 1)))
kernel void fetch (global float2 * restrict src) {

  const int N = (1 << LOGN);
  // Each thread will fetch POINTS points. Need POINTS times to pass to FFT.
  const int BUF_SIZE = 1 << (LOG_CONT_FACTOR + LOGPOINTS + LOGPOINTS);

  // Local memory for CONT_FACTOR * POINTS points
  local float2 buf[BUF_SIZE];

  uint iteration = get_global_id(1);
  uint group_per_iter = get_global_id(0);

  // permute global addr but not the local addr
  uint global_addr = iteration * N + group_per_iter;
  global_addr = permute_gid (global_addr << LOGPOINTS);
  uint lid = get_local_id(0);
  uint local_addr = lid << LOGPOINTS;

  #pragma unroll
  for (uint k = 0; k < POINTS; k++) {
    buf[local_addr + k] = src[global_addr + k];
  }

  barrier (CLK_LOCAL_MEM_FENCE);

  #pragma unroll
  for (uint k = 0; k < POINTS; k++) {
    uint buf_addr = bit_reversed(k,3) * CONT_FACTOR * POINTS + lid;
    write_channel_intel (chanin[k], buf[buf_addr]);
  }
}


/* 'src' and 'dest' point to the input and output buffers in global memory;
 * using restrict pointers as there are no dependencies between the buffers
 * 'count' represents the number of 4k sets to process
 * 'inverse' toggles between the direct and the inverse transform
 */

kernel void fft1d_2(global float2 * restrict dest,
                  int count, int inverse) {

  const int N = (1 << LOGN);

  /* The FFT engine requires a sliding window array for data reordering; data
   * stored in this array is carried across loop iterations and shifted by one
   * element every iteration; all loop dependencies derived from the uses of
   * this array are simple transfers between adjacent array elements
   */

  float2 fft_delay_elements[N + 8 * (LOGN - 2)];

  /* This is the main loop. It runs 'count' back-to-back FFT transforms
   * In addition to the 'count * (N / 8)' iterations, it runs 'N / 8 - 1'
   * additional iterations to drain the last outputs
   * (see comments attached to the FFT engine)
   *
   * The compiler leverages pipeline parallelism by overlapping the
   * iterations of this loop - launching one iteration every clock cycle
   */

  for (unsigned i = 0; i < count * (N / 8) + N / 8 - 1; i++) {

    /* As required by the FFT engine, gather input data from 8 distinct
     * segments of the input buffer; for simplicity, this implementation
     * does not attempt to coalesce memory accesses and this leads to
     * higher resource utilization (see the fft2d example for advanced
     * memory access techniques)
     */

    int base = (i / (N / 8)) * N;
    int offset = i % (N / 8);

    float2x8 data;
    // Perform memory transfers only when reading data in range
    if (i < count * (N / 8)) {
      data.i0 = read_channel_intel(chanin[0]);
      data.i1 = read_channel_intel(chanin[1]);
      data.i2 = read_channel_intel(chanin[2]);
      data.i3 = read_channel_intel(chanin[3]);
      data.i4 = read_channel_intel(chanin[4]);
      data.i5 = read_channel_intel(chanin[5]);
      data.i6 = read_channel_intel(chanin[6]);
      data.i7 = read_channel_intel(chanin[7]);
    } else {
      data.i0 = data.i1 = data.i2 = data.i3 =
                data.i4 = data.i5 = data.i6 = data.i7 = 0;
    }

    // Perform one step of the FFT engine
    data = fft_step(data, i % (N / 8), fft_delay_elements, inverse, LOGN);

    /* Store data back to memory. FFT engine outputs are delayed by
     * N / 8 - 1 steps, hence gate writes accordingly
     */

    if (i >= N / 8 - 1) {
      int base = 8 * (i - (N / 8 - 1));

      // These consecutive accesses will be coalesced by the compiler
      dest[base] = data.i0;
      dest[base + 1] = data.i1;
      dest[base + 2] = data.i2;
      dest[base + 3] = data.i3;
      dest[base + 4] = data.i4;
      dest[base + 5] = data.i5;
      dest[base + 6] = data.i6;
      dest[base + 7] = data.i7;
    }
  }
}



