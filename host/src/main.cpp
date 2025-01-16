// Copyright (C) 2013-2020 Altera Corporation, San Jose, California, USA. All rights reserved.
// Permission is hereby granted, free of charge, to any person obtaining a copy of this
// software and associated documentation files (the "Software"), to deal in the Software
// without restriction, including without limitation the rights to use, copy, modify, merge,
// publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to
// whom the Software is furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in all copies or
// substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
// OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
// HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
// WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
// OTHER DEALINGS IN THE SOFTWARE.
// 
// This agreement shall be governed in all respects by the laws of the State of California and
// by the laws of the United States of America.

///////////////////////////////////////////////////////////////////////////////////
// This OpenCL application executes a 1D FFT transform on an Altera FPGA.
// The kernel is defined in a device/fft1d.cl file.  The Altera 
// Offline Compiler tool ('aoc') compiles the kernel source into a 'fft1d.aocx' 
// file containing a hardware programming image for the FPGA.  The host program 
// provides the contents of the .aocx file to the clCreateProgramWithBinary OpenCL
// API for runtime programming of the FPGA.
//
// When compiling this application, ensure that the Intel(R) FPGA SDK for OpenCL(TM)
// is properly installed.
///////////////////////////////////////////////////////////////////////////////////

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#define _USE_MATH_DEFINES
#include <math.h>
#include <cstring>
#include <CL/opencl.h>
#include <CL/cl_ext_intelfpga.h>
#include "AOCLUtils/aocl_utils.h"
#include "fft_config.h"

#include <iostream>

// the above header defines log of the FFT size hardcoded in the kernel
// compute N as 2^LOGN
#define N (1 << LOGN)

using namespace aocl_utils;
using namespace std;


#define STRING_BUFFER_LEN 1024

typedef struct {
  double start_init;
  double end_init;
  double start_fft;
  double end_fft;
  double start_verify;
  double end_verify;
  double start_fft_setup;
  double end_fft_setup;
} timing_data;

void timing_report(const std::string &pre, timing_data &t)
{
    cout << "Timing report " << pre << std::endl;
    double init_time = t.end_init - t.start_init;
    double fft_setup_time = t.end_fft_setup - t.start_fft_setup;
    double fft_runtime = t.end_fft - t.start_fft;
    double fft_verify = t.end_verify - t.start_verify;

    cout << "Initialization: " << init_time * 1000 << "ms." << endl;
    cout << "FFT setup:\t" << fft_setup_time * 1000 << "ms." << endl;
    cout << "FFT run:\t\t" << fft_runtime * 1000 << "ms." << endl;
    cout << "FFT verify:\t\t" << fft_verify * 1000 << "ms." << endl;
}


// ACL runtime configuration
static cl_platform_id platform = NULL;
static cl_device_id device = NULL;
static cl_context context = NULL;
static cl_command_queue queue = NULL;
static cl_command_queue queue1 = NULL;
static cl_kernel kernel = NULL;
static cl_kernel kernel1 = NULL;
static cl_command_queue queue_1 = NULL;
static cl_command_queue queue1_1 = NULL;
static cl_kernel kernel_1 = NULL;
static cl_kernel kernel1_1 = NULL;
static cl_program program = NULL;
static cl_int status = 0;
static timing_data td;

// Control whether the emulator should be used.
static bool use_emulator = false;

// FFT operates with complex numbers - store them in a struct
typedef struct {
  double x;
  double y;
} double2;

typedef struct {
  float x;
  float y;
} float2;

// Function prototypes
bool init();
void cleanup();
static void test_fft(int iterations, bool inverse);
static int coord(int iteration, int i);
static void fourier_transform_gold(bool inverse, int lognr_points, double2 * data);
static void fourier_stage(int lognr_points, double2 * data);


// Host memory buffers
float2 *h_inData, *h_outData, *h_inData_1, *h_outData_1;
double2 *h_verify;

// Device memory buffers
cl_mem d_inData, d_outData, d_inData_1, d_outData_1;

// Entry point.
int main(int argc, char **argv) {
  int iterations = 2000;

  Options options(argc, argv);

  // Optional argument to set the number of iterations.
  if(options.has("n")) {
    iterations = options.get<int>("n");
  }

  // Optional argument to specify whether the emulator should be used.
  if(options.has("emulator")) {
    use_emulator = options.get<bool>("emulator");
  }

  cout << "Running with " << iterations << " iterations." << endl;


  if(!init()) {
    return false;
  }

  if (iterations <= 0) {
	printf("ERROR: Invalid number of iterations\n\nUsage: %s [-N=<#>]\n\tN: number of iterations to run (default 2000)\n", argv[0]);
	return false;
  }

  // Allocate host memory

  h_inData = (float2 *)alignedMalloc(sizeof(float2) * N * iterations);
  h_outData = (float2 *)alignedMalloc(sizeof(float2) * N * iterations);
  h_inData_1 = (float2 *)alignedMalloc(sizeof(float2) * N * iterations);
  h_outData_1 = (float2 *)alignedMalloc(sizeof(float2) * N * iterations);
  h_verify = (double2 *)alignedMalloc(sizeof(double2) * N * iterations);
  if (!(h_inData && h_outData && h_verify && h_outData_1)) {
    printf("ERROR: Couldn't create host buffers\n");
    return false;
  }


  if ( options.has("mode")){
     if (options.get<std::string>("mode") == "normal") {
       cout << "AAA " << iterations << endl;
       test_fft(iterations, false);
       timing_report("FORWARD", td);
     } else if (options.get<std::string>("mode") == "inverse") {
       cout << "BBB " << iterations << endl;
       test_fft(iterations, true);
       timing_report("INVERSE", td);
     }
  } else{
     test_fft(iterations, false); // test FFT transform running a sequence of iterations x 4k points transforms
     timing_report("FORWARD", td);
     test_fft(iterations, true); // test inverse FFT transform - same setup as above
     timing_report("INVERSE", td);
  }

  // Free the resources allocated
  cleanup();

  return 0;
}

void test_fft(int iterations, bool inverse) {
  printf("Launching");
  if (inverse) 
	printf(" inverse");
  printf(" FFT transform for %d iterations\n", iterations);

  td.start_fft_setup = getCurrentTimestamp();

  // Initialize input and produce verification data
  for (int i = 0; i < iterations; i++) {
    for (int j = 0; j < N; j++) {
      h_verify[coord(i, j)].x = h_inData[coord(i, j)].x  = (float)((double)rand() / (double)RAND_MAX);
      h_verify[coord(i, j)].y = h_inData[coord(i, j)].y   = (float)((double)rand() / (double)RAND_MAX);
    }
  }

  // Create device buffers - assign the buffers in different banks for more efficient
  // memory access 
 
  d_inData = clCreateBuffer(context, CL_MEM_READ_WRITE, sizeof(float2) * N * iterations, NULL, &status);
  checkError(status, "Failed to allocate input device buffer\n");

  d_inData_1 = clCreateBuffer(context, CL_MEM_READ_WRITE, sizeof(float2) * N * iterations, NULL, &status);
  checkError(status, "Failed to allocate input device buffer 1\n");

  d_outData = clCreateBuffer(context, CL_MEM_READ_WRITE | CL_CHANNEL_2_INTELFPGA, sizeof(float2) * N * iterations, NULL, &status);
  checkError(status, "Failed to allocate output device buffer\n");

  d_outData_1 = clCreateBuffer(context, CL_MEM_READ_WRITE | CL_CHANNEL_2_INTELFPGA, sizeof(float2) * N * iterations, NULL, &status);
  checkError(status, "Failed to allocate output device buffer 1\n");

  // Copy data from host to device

  status = clEnqueueWriteBuffer(queue1, d_inData, CL_TRUE, 0, sizeof(float2) * N * iterations, h_inData, 0, NULL, NULL);
  checkError(status, "Failed to copy data to device");

  status = clEnqueueWriteBuffer(queue1_1, d_inData_1, CL_TRUE, 0, sizeof(float2) * N * iterations, h_inData, 0, NULL, NULL);
  checkError(status, "Failed to copy data to device with queue_1");

  // Can't pass bool to device, so convert it to int
  int inverse_int = inverse;

  // Set the kernel arguments

  status = clSetKernelArg(kernel1, 0, sizeof(cl_mem), (void *)&d_inData);
  checkError(status, "Failed to set kernel1 arg 0");

  status = clSetKernelArg(kernel1_1, 0, sizeof(cl_mem), (void *)&d_inData_1);
  checkError(status, "Failed to set kernel1_1 arg 0");

  status = clSetKernelArg(kernel, 0, sizeof(cl_mem), (void *)&d_outData);
  checkError(status, "Failed to set kernel arg 0");

  status = clSetKernelArg(kernel_1, 0, sizeof(cl_mem), (void *)&d_outData_1);
  checkError(status, "Failed to set kernel_1 arg 0");

  status = clSetKernelArg(kernel, 1, sizeof(cl_int), (void*)&iterations);
  checkError(status, "Failed to set kernel arg 1");

  status = clSetKernelArg(kernel_1, 1, sizeof(cl_int), (void*)&iterations);
  checkError(status, "Failed to set kernel_1 arg 1");

  status = clSetKernelArg(kernel, 2, sizeof(cl_int), (void*)&inverse_int);
  checkError(status, "Failed to set kernel arg 2");

  status = clSetKernelArg(kernel_1, 2, sizeof(cl_int), (void*)&inverse_int);
  checkError(status, "Failed to set kernel_1 arg 2");

  printf(inverse ? "\tInverse FFT" : "\tFFT");
  printf(" kernel initialization is complete.\n");

  td.end_fft_setup = getCurrentTimestamp();

  // Get the iterationstamp to evaluate performance
  td.start_fft = getCurrentTimestamp();

  // Launch the kernel - we launch a single work item hence enqueue a task
  status = clEnqueueTask(queue, kernel, 0, NULL, NULL);
  checkError(status, "Failed to launch kernel");

  status = clEnqueueTask(queue_1, kernel_1, 0, NULL, NULL);
  checkError(status, "Failed to launch kernel_1");

  size_t ls = N/8;
  size_t gs = iterations * ls;
  status = clEnqueueNDRangeKernel(queue1, kernel1, 1, NULL, &gs, &ls, 0, NULL, NULL);
  checkError(status, "Failed to launch fetch kernel1");

  status = clEnqueueNDRangeKernel(queue1_1, kernel1_1, 1, NULL, &gs, &ls, 0, NULL, NULL);
  checkError(status, "Failed to launch fetch kernel1_1");

  // Wait for command queue to complete pending events
  status = clFinish(queue);
  checkError(status, "Failed to finish queue");
  status = clFinish(queue1);
  checkError(status, "Failed to finish queue1");

  status = clFinish(queue_1);
  checkError(status, "Failed to finish queue_1");
  status = clFinish(queue1_1);
  checkError(status, "Failed to finish queue1_1");


  // Record execution time
  td.end_fft = getCurrentTimestamp();

  // Copy results from device to host
  status = clEnqueueReadBuffer(queue, d_outData, CL_TRUE, 0, sizeof(float2) * N * iterations, h_outData, 0, NULL, NULL);
  checkError(status, "Failed to copy data from device queue");

  status = clEnqueueReadBuffer(queue_1, d_outData_1, CL_TRUE, 0, sizeof(float2) * N * iterations, h_outData_1, 0, NULL, NULL);
  checkError(status, "Failed to copy data from device queue_1");

  double time = td.start_fft - td.end_fft;

  // Pick randomly a few iterations and check SNR
  td.start_verify = getCurrentTimestamp();

  double fpga_snr = 200;
  double fpga_snr_1 = 200;
  for (int i = 0; i < iterations; i+= rand() % 20 + 1) {
    fourier_transform_gold(inverse, LOGN, h_verify + coord(i, 0));
    double mag_sum = 0;
    double noise_sum = 0;
    double noise_sum_1 = 0;
    for (int j = 0; j < N; j++) {
      double magnitude = (double)h_verify[coord(i, j)].x * (double)h_verify[coord(i, j)].x +  
                              (double)h_verify[coord(i, j)].y * (double)h_verify[coord(i, j)].y;

      double noise = (h_verify[coord(i, j)].x - (double)h_outData[coord(i, j)].x) * (h_verify[coord(i, j)].x - (double)h_outData[coord(i, j)].x) +  
                          (h_verify[coord(i, j)].y - (double)h_outData[coord(i, j)].y) * (h_verify[coord(i, j)].y - (double)h_outData[coord(i, j)].y);

      double noise_1 = (h_verify[coord(i, j)].x - (double)h_outData_1[coord(i, j)].x) * (h_verify[coord(i, j)].x - (double)h_outData_1[coord(i, j)].x) +
                       (h_verify[coord(i, j)].y - (double)h_outData_1[coord(i, j)].y) * (h_verify[coord(i, j)].y - (double)h_outData_1[coord(i, j)].y);

        mag_sum += magnitude;
        noise_sum += noise;
        noise_sum_1 += noise_1;

    }
    double db = 10 * log(mag_sum / noise_sum) / log(10.0);
    // find minimum SNR across all iterations
    if (db < fpga_snr) fpga_snr = db;
    db = 10 * log(mag_sum / noise_sum_1) / log(10.0);
    if (db < fpga_snr_1) fpga_snr_1 = db;
  }
  td.end_verify = getCurrentTimestamp();

  printf("\tSignal to noise ratio on output sample 0: %f --> %s\n\n", fpga_snr, fpga_snr > 125 ? "PASSED" : "FAILED");
  printf("\tSignal to noise ratio on output sample 1: %f --> %s\n\n", fpga_snr_1, fpga_snr_1 > 125 ? "PASSED" : "FAILED");
}


/////// HELPER FUNCTIONS ///////

// provides a linear offset in the input array
int coord(int iteration, int i) {
  return iteration * N + i;
}


// Reference Fourier transform
void fourier_transform_gold(bool inverse, const int lognr_points, double2 *data) {
   const int nr_points = 1 << lognr_points;

   // The inverse requires swapping the real and imaginary component
   
   if (inverse) {
      for (int i = 0; i < nr_points; i++) {
         double tmp = data[i].x;
         data[i].x = data[i].y;
         data[i].y = tmp;;
      }
   }
   // Do a FT recursively
   fourier_stage(lognr_points, data);

   // The inverse requires swapping the real and imaginary component
   if (inverse) {
      for (int i = 0; i < nr_points; i++) {
         double tmp = data[i].x;
         data[i].x = data[i].y;
         data[i].y = tmp;;
      }
   }

   // Do the bit reversal

   double2 *temp = (double2 *)alloca(sizeof(double2) * nr_points);
   for (int i = 0; i < nr_points; i++) temp[i] = data[i];
   for (int i = 0; i < nr_points; i++) {
      int fwd = i;
      int bit_rev = 0;
      for (int j = 0; j < lognr_points; j++) {
         bit_rev <<= 1;
         bit_rev |= fwd & 1;
         fwd >>= 1;
      }
      data[i] = temp[bit_rev];
   }
}

void fourier_stage(int lognr_points, double2 *data) {
   int nr_points = 1 << lognr_points;
   if (nr_points == 1) return;
   double2 *half1 = (double2 *)alloca(sizeof(double2) * nr_points / 2);
   double2 *half2 = (double2 *)alloca(sizeof(double2) * nr_points / 2);
   for (int i = 0; i < nr_points / 2; i++) {
      half1[i] = data[2 * i];
      half2[i] = data[2 * i + 1];
   }
   fourier_stage(lognr_points - 1, half1);
   fourier_stage(lognr_points - 1, half2);
   for (int i = 0; i < nr_points / 2; i++) {
      data[i].x = half1[i].x + cos (2 * M_PI * i / nr_points) * half2[i].x + sin (2 * M_PI * i / nr_points) * half2[i].y;
      data[i].y = half1[i].y - sin (2 * M_PI * i / nr_points) * half2[i].x + cos (2 * M_PI * i / nr_points) * half2[i].y;
      data[i + nr_points / 2].x = half1[i].x - cos (2 * M_PI * i / nr_points) * half2[i].x - sin (2 * M_PI * i / nr_points) * half2[i].y;
      data[i + nr_points / 2].y = half1[i].y + sin (2 * M_PI * i / nr_points) * half2[i].x - cos (2 * M_PI * i / nr_points) * half2[i].y;
   }
}

bool init() {
  td.start_init = getCurrentTimestamp();

  cl_int status;

  if(!setCwdToExeDir()) {
    return false;
  }

  // Get the OpenCL platform.
  if (use_emulator) {
    platform = findPlatform("Intel(R) FPGA Emulation Platform for OpenCL(TM)");
  } else {
    platform = findPlatform("Intel(R) FPGA SDK for OpenCL(TM)");
  }
  if(platform == NULL) {
    printf("ERROR: Unable to find Intel(R) FPGA OpenCL platform\n");
    return false;
  }

  // Query the available OpenCL devices.
  scoped_array<cl_device_id> devices;
  cl_uint num_devices;

  devices.reset(getDevices(platform, CL_DEVICE_TYPE_ALL, &num_devices));

  // We'll just use the first device.
  device = devices[0];

  // Create the context.
  context = clCreateContext(NULL, 1, &device, &oclContextCallback, NULL, &status);
  checkError(status, "Failed to create context");

  // Create the command queue.
  queue = clCreateCommandQueue(context, device, CL_QUEUE_PROFILING_ENABLE, &status);
  checkError(status, "Failed to create command queue");
  queue1 = clCreateCommandQueue(context, device, CL_QUEUE_PROFILING_ENABLE, &status);
  checkError(status, "Failed to create command queue");


  // Create the program.
  std::string binary_file = getBoardBinaryFile("fft1d", device);
  printf("Using AOCX: %s\n\n", binary_file.c_str());
  program = createProgramFromBinary(context, binary_file.c_str(), &device, 1);

  // Build the program that was just created.
  status = clBuildProgram(program, 0, NULL, "", NULL, NULL);
  checkError(status, "Failed to build program");

  // Create the kernel - name passed in here must match kernel name in the
  // original CL file, that was compiled into an AOCX file using the AOC tool
  kernel = clCreateKernel(program, "fft1d", &status);
  checkError(status, "Failed to create kernel");

  kernel1 = clCreateKernel(program, "fetch", &status);
  checkError(status, "Failed to create fetch kernel");

  kernel_1 = clCreateKernel(program, "fft1d_1", &status);
  checkError(status, "Failed to create kernel");

  kernel1_1 = clCreateKernel(program, "fetch_1", &status);
  checkError(status, "Failed to create fetch kernel");

  td.end_init = getCurrentTimestamp();

  return true;
}

// Free the resources allocated during initialization
void cleanup() {
  if(kernel) 
    clReleaseKernel(kernel);  
  if(kernel1) 
    clReleaseKernel(kernel1);  
  if(program) 
    clReleaseProgram(program);
  if(queue) 
    clReleaseCommandQueue(queue);
  if(queue1) 
    clReleaseCommandQueue(queue1);
  if (h_verify)
	alignedFree(h_verify);

  if(h_inData)
	alignedFree(h_inData);
  if (h_outData)
	alignedFree(h_outData);
  if (d_inData)
	clReleaseMemObject(d_inData);
  if (d_outData) 
	clReleaseMemObject(d_outData);

  if(context)
    clReleaseContext(context);
}



