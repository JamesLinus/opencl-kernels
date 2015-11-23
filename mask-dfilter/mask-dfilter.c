/*
 *  Simple OpenCL demo program
 *
 *  Copyright (C) 2009  Clifford Wolf <clifford@clifford.at>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *  gcc -o cldemo -std=gnu99 -Wall -I/usr/include/nvidia-current cldemo.c -lOpenCL
 *
 */

#include <CL/cl.h>

#include <iostream>
#include <fstream>
#include <sstream>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#define NUM_DATA 66

#define CL_CHECK(_expr)                                                         \
   do {                                                                         \
     cl_int _err = _expr;                                                       \
     if (_err == CL_SUCCESS)                                                    \
       break;                                                                   \
     fprintf(stderr, "OpenCL Error: '%s' returned %d!\n", #_expr, (int)_err);   \
     abort();                                                                   \
   } while (0)

#define CL_CHECK_ERR(_expr)                                                     \
   ({                                                                           \
     cl_int _err = CL_INVALID_VALUE;                                            \
     typeof(_expr) _ret = _expr;                                                \
     if (_err != CL_SUCCESS) {                                                  \
       fprintf(stderr, "OpenCL Error: '%s' returned %d!\n", #_expr, (int)_err); \
       abort();                                                                 \
     }                                                                          \
     _ret;                                                                      \
   })

void pfn_notify(const char *errinfo, const void *private_info, size_t cb, void *user_data)
{
	fprintf(stderr, "OpenCL Error (via pfn_notify): %s\n", errinfo);
}
// inlcude pocl float to half conversions
typedef union 
{
  int32_t i;
  float f;
} FloatConvUnion;
cl_half 
poclu_float_to_cl_half(float value) 
{
  FloatConvUnion u;
  u.f = value;
  cl_half half = (u.i >> 16) & 0x8000; // sign
  cl_half fraction = (u.i >> 12) & 0x007ff; // fraction with extra bit for rounding
  cl_half exponent = (u.i >> 23)  & 0xff; // exponent
  
  if(exponent < 0x0067) // Return signed zero if zero or value is too small for denormal half
    return half;

  if(exponent > 0x008e){// value was NaN or Inf
    half |= 0x7c00u; // Make into inf
    half |= exponent == 255 && (u.i & 0x007fffffu); // If value was NaN make this into NaN
    return half;
  }

  if(exponent < 0x0071){// Denormal
    fraction |= 0x0800u;

    // rounding
    half |= (fraction >> (0x0072 - exponent)) + ((fraction >> (0x0071 - exponent)) & 1);
    return half;
  }

  half |= ((exponent - 0x0070) << 10) | (fraction >> 1);
  half += fraction & 1;// rounding
  return half;
}
#ifndef INFINITY
#define INFINITY 1.0/0.0
#endif

#ifndef NAN
#define NAN 0.0/0.0
#endif

float
poclu_cl_half_to_float(cl_half value) 
{
  if (value == 0xFC00) {
    return -INFINITY;
  }
  if (value == 0x7C00) {
    return INFINITY;
  }

  int sgn = ((value & 0x8000) >> 15);
  int exp = (value & 0x7C00) >> 10;
  int mant = value & 0x03FF;

  if (exp == 0x1F && mant != 0) {
    return NAN;
  }

  float v = (exp == 0) ? mant : mant | 0x0400; // 1.x if not denormal
  v /= 0x400;
  float mul = exp2((float)exp - 15);
  v *= mul;
  if (sgn) {
    v *= -1;
  }
  return v;
}

///
//  Create an OpenCL program from the kernel source file
//
cl_program CreateProgram(cl_context context, cl_device_id device, const char* fileName)
{
    cl_int errNum;
    cl_program program;

    std::ifstream kernelFile(fileName, std::ios::in);
    if (!kernelFile.is_open())
    {
        std::cerr << "Failed to open file for reading: " << fileName << std::endl;
        return NULL;
    }

    std::ostringstream oss;
    oss << kernelFile.rdbuf();

    std::string srcStdStr = oss.str();
    const char *srcStr = srcStdStr.c_str();
    program = clCreateProgramWithSource(context, 1,
                                        (const char**)&srcStr,
                                        NULL, NULL);
    if (program == NULL)
    {
        std::cerr << "Failed to create CL program from source." << std::endl;
        return NULL;
    }

    errNum = clBuildProgram(program, 0, NULL, NULL, NULL, NULL);
    if (errNum != CL_SUCCESS)
    {
        // Determine the reason for the error
        char buildLog[16384];
        clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG,
                              sizeof(buildLog), buildLog, NULL);

        std::cerr << "Error in kernel: " << std::endl;
        std::cerr << buildLog;
        clReleaseProgram(program);
        return NULL;
    }

    return program;
}

//
///
//  Retreive program binary for all of the devices attached to the
//  program an and store the one for the device passed in
//
bool SaveProgramBinary(cl_program program, cl_device_id device, const char* fileName)
{
    //cl_uint numDevices = malloc(sizeof(cl_uint));
    //cl_uint* numDevices = malloc(sizeof(cl_uint));
    cl_int errNum;

    printf("try getting program info\n");
    // 1 - Query for number of devices attached to program
    /*errNum = clGetProgramInfo(program, CL_PROGRAM_NUM_DEVICES, sizeof(cl_uint),
                              &numDevices, NULL);
    printf("Got program_num_devices\n");
    if (errNum != CL_SUCCESS)
    {
        std::cerr << "Error querying for number of devices." << std::endl;
        return false;
    }*/

    // 2 - Get all of the Device IDs
    cl_device_id *devices = new cl_device_id[1];
    errNum = clGetProgramInfo(program, CL_PROGRAM_DEVICES,
                              sizeof(cl_device_id) * 1,
                              devices, NULL);
    printf("Got program_devices\n");
    if (errNum != CL_SUCCESS)
    {
        std::cerr << "Error querying for devices." << std::endl;
        delete [] devices;
        return false;
    }

    // 3 - Determine the size of each program binary
    size_t *programBinarySizes = new size_t [1];
    errNum = clGetProgramInfo(program, CL_PROGRAM_BINARY_SIZES,
                              sizeof(size_t) * 1,
                              programBinarySizes, NULL);
    printf("Got program_binary_sizes\n");
    if (errNum != CL_SUCCESS)
    {
        std::cerr << "Error querying for program binary sizes." << std::endl;
        delete [] devices;
        delete [] programBinarySizes;
        return false;
    }

    unsigned char **programBinaries = new unsigned char*[1];
    for (cl_uint i = 0; i < 1; i++)
    {
        programBinaries[i] = new unsigned char[programBinarySizes[i]];
    }

    // 4 - Get all of the program binaries
    errNum = clGetProgramInfo(program, CL_PROGRAM_BINARIES, sizeof(unsigned char*) * 1,
                              programBinaries, NULL);
    printf("Got program_binarys\n");
    if (errNum != CL_SUCCESS)
    {
        std::cerr << "Error querying for program binaries" << std::endl;

        delete [] devices;
        delete [] programBinarySizes;
        for (cl_uint i = 0; i < 1; i++)
        {
            delete [] programBinaries[i];
        }
        delete [] programBinaries;
        return false;
    }

    // 5 - Finally store the binaries for the device requested out to disk for future reading.
    for (cl_uint i = 0; i < 1; i++)
    {
        // Store the binary just for the device requested.  In a scenario where
        // multiple devices were being used you would save all of the binaries out here.
        if (devices[i] == device)
        {
            FILE *fp = fopen(fileName, "wb");
            if(fp ==NULL){
              delete [] devices;
              delete [] programBinarySizes;
              for (cl_uint i = 0; i < 1; i++)
              {
                  delete [] programBinaries[i];
              }
              delete [] programBinaries;
              return false;
            }
            printf("Opened file\n");
            fwrite(programBinaries[i], 1, programBinarySizes[i], fp);
            printf("wrote file\n");
            fclose(fp);
            printf("close file\n");
            break;
        }
    }

    // Cleanup
    delete [] devices;
    delete [] programBinarySizes;
    for (cl_uint i = 0; i < 1; i++)
    {
        delete [] programBinaries[i];
    }
    delete [] programBinaries;
    return true;
}

///
//  Attempt to create the program object from a cached binary.  Note that
//  on first run this will fail because the binary has not yet been created.
//
cl_program CreateProgramFromBinary(cl_context context, cl_device_id device, const char* fileName)
{
    FILE *fp = fopen(fileName, "rb");
    if (fp == NULL)
    {
        return NULL;
    }

    // Determine the size of the binary
    size_t binarySize;
    fseek(fp, 0, SEEK_END);
    binarySize = ftell(fp);
    rewind(fp);

    unsigned char *programBinary = new unsigned char[binarySize];
    fread(programBinary, 1, binarySize, fp);
    fclose(fp);

    cl_int errNum = 0;
    cl_program program;
    cl_int binaryStatus;

    program = clCreateProgramWithBinary(context,
                                        1,
                                        &device,
                                        &binarySize,
                                        (const unsigned char**)&programBinary,
                                        &binaryStatus,
                                        &errNum);
    delete [] programBinary;
    if (errNum != CL_SUCCESS)
    {
        std::cerr << "Error loading program binary." << std::endl;
        return NULL;
    }

    if (binaryStatus != CL_SUCCESS)
    {
        std::cerr << "Invalid binary for device" << std::endl;
        return NULL;
    }

    errNum = clBuildProgram(program, 1, &device, NULL, NULL, NULL);
    if (errNum != CL_SUCCESS)
    {
        printf("build errNum:%d\n", errNum);
        // Determine the reason for the error
        char buildLog[16384];
        clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG,
                              sizeof(buildLog), buildLog, NULL);

        std::cerr << "Error in program: " << std::endl;
        std::cerr << buildLog << std::endl;
        clReleaseProgram(program);
        return NULL;
    }

    return program;
}

///
//  Cleanup any created OpenCL resources
//
void Cleanup(cl_context context, cl_command_queue commandQueue,
             cl_program program, cl_kernel kernel, cl_mem memObjects[3])
{
    for (int i = 0; i < 3; i++)
    {
        if (memObjects[i] != 0)
            clReleaseMemObject(memObjects[i]);
    }
    if (commandQueue != 0)
        clReleaseCommandQueue(commandQueue);

    if (kernel != 0)
        clReleaseKernel(kernel);

    if (program != 0)
        clReleaseProgram(program);

    if (context != 0)
        clReleaseContext(context);

}

int main(int argc, char **argv)
{
  printf("enter demo main\n");
  fflush(stdout);
  putenv("POCL_VERBOSE=1");
  putenv("POCL_DEVICES=basic");
  putenv("POCL_LEAVE_TEMP_DIRS=1");
  putenv("POCL_LEAVE_KERNEL_COMPILER_TEMP_FILES=1");
  putenv("POCL_TEMP_DIR=pocl");
  putenv("POCL_CACHE_DIR=pocl");
  putenv("POCL_WORK_GROUP_METHOD=spmd");
  if(argc >= 2){
    printf("argv[1]:%s:\n",argv[1]);
    if(!strcmp(argv[1], "h"))
      putenv("POCL_WORK_GROUP_METHOD=spmd");
    if(!strcmp(argv[1], "c"))
      putenv("POCL_CROSS_COMPILE=1");
  }
  if(argc >= 3){
    printf("argv[2]:%s:\n",argv[2]);
    if(!strcmp(argv[2], "h"))
      putenv("POCL_WORK_GROUP_METHOD=spmd");
    if(!strcmp(argv[2], "c"))
      putenv("POCL_CROSS_COMPILE=1");
  }

  //putenv("LD_LIBRARY_PATH=/scratch/colins/build/linux/fs/lib");
  //putenv("LTDL_LIBRARY_PATH=/scratch/colins/build/linux/fs/lib");
  //lt_dlsetsearchpath("/scratch/colins/build/linux/fs/lib");
  //printf("SEARCH_PATH:%s\n",lt_dlgetsearchpath());
	cl_platform_id platforms[100];
	cl_uint platforms_n = 0;
	CL_CHECK(clGetPlatformIDs(100, platforms, &platforms_n));

	printf("=== %d OpenCL platform(s) found: ===\n", platforms_n);
	for (int i=0; i<platforms_n; i++)
	{
		char buffer[10240];
		printf("  -- %d --\n", i);
		CL_CHECK(clGetPlatformInfo(platforms[i], CL_PLATFORM_PROFILE, 10240, buffer, NULL));
		printf("  PROFILE = %s\n", buffer);
		CL_CHECK(clGetPlatformInfo(platforms[i], CL_PLATFORM_VERSION, 10240, buffer, NULL));
		printf("  VERSION = %s\n", buffer);
		CL_CHECK(clGetPlatformInfo(platforms[i], CL_PLATFORM_NAME, 10240, buffer, NULL));
		printf("  NAME = %s\n", buffer);
		CL_CHECK(clGetPlatformInfo(platforms[i], CL_PLATFORM_VENDOR, 10240, buffer, NULL));
		printf("  VENDOR = %s\n", buffer);
		CL_CHECK(clGetPlatformInfo(platforms[i], CL_PLATFORM_EXTENSIONS, 10240, buffer, NULL));
		printf("  EXTENSIONS = %s\n", buffer);
	}

	if (platforms_n == 0)
		return 1;

	cl_device_id devices[100];
	cl_uint devices_n = 0;
	// CL_CHECK(clGetDeviceIDs(NULL, CL_DEVICE_TYPE_ALL, 100, devices, &devices_n));
	CL_CHECK(clGetDeviceIDs(platforms[0], CL_DEVICE_TYPE_GPU, 100, devices, &devices_n));

	printf("=== %d OpenCL device(s) found on platform:\n", platforms_n);
	for (int i=0; i<devices_n; i++)
	{
		char buffer[10240];
		cl_uint buf_uint;
		cl_ulong buf_ulong;
		printf("  -- %d --\n", i);
		CL_CHECK(clGetDeviceInfo(devices[i], CL_DEVICE_NAME, sizeof(buffer), buffer, NULL));
		printf("  DEVICE_NAME = %s\n", buffer);
		CL_CHECK(clGetDeviceInfo(devices[i], CL_DEVICE_VENDOR, sizeof(buffer), buffer, NULL));
		printf("  DEVICE_VENDOR = %s\n", buffer);
		CL_CHECK(clGetDeviceInfo(devices[i], CL_DEVICE_VERSION, sizeof(buffer), buffer, NULL));
		printf("  DEVICE_VERSION = %s\n", buffer);
		CL_CHECK(clGetDeviceInfo(devices[i], CL_DRIVER_VERSION, sizeof(buffer), buffer, NULL));
		printf("  DRIVER_VERSION = %s\n", buffer);
		CL_CHECK(clGetDeviceInfo(devices[i], CL_DEVICE_MAX_COMPUTE_UNITS, sizeof(buf_uint), &buf_uint, NULL));
		printf("  DEVICE_MAX_COMPUTE_UNITS = %u\n", (unsigned int)buf_uint);
		CL_CHECK(clGetDeviceInfo(devices[i], CL_DEVICE_MAX_CLOCK_FREQUENCY, sizeof(buf_uint), &buf_uint, NULL));
		printf("  DEVICE_MAX_CLOCK_FREQUENCY = %u\n", (unsigned int)buf_uint);
		CL_CHECK(clGetDeviceInfo(devices[i], CL_DEVICE_GLOBAL_MEM_SIZE, sizeof(buf_ulong), &buf_ulong, NULL));
		printf("  DEVICE_GLOBAL_MEM_SIZE = %llu\n", (unsigned long long)buf_ulong);
	}

	if (devices_n == 0)
		return 1;

	cl_context context;
	context = CL_CHECK_ERR(clCreateContext(NULL, 1, devices, &pfn_notify, NULL, &_err));

	cl_command_queue queue;
  queue = CL_CHECK_ERR(clCreateCommandQueue(context, devices[0], CL_QUEUE_PROFILING_ENABLE, &_err));

	cl_kernel kernel = 0;
  cl_mem memObjects[2] = {0,0};


  // Create OpenCL program - first attempt to load cached binary.
  //  If that is not available, then create the program from source
  //  and store the binary for future use.
  std::cout << "Attempting to create program from binary..." << std::endl;
  cl_program program = CreateProgramFromBinary(context, devices[0], "kernel.cl.bin");
  if (program == NULL)
  {
      std::cout << "Binary not loaded, create from source..." << std::endl;
      program = CreateProgram(context, devices[0], "kernel.cl");
      if (program == NULL)
      {
          Cleanup(context, queue, program, kernel, memObjects);
          return 1;
      }

      std::cout << "Save program binary for future run..." << std::endl;
      if (SaveProgramBinary(program, devices[0], "kernel.cl.bin") == false)
      {
          std::cerr << "Failed to write program binary" << std::endl;
          Cleanup(context, queue, program, kernel, memObjects);
          return 1;
      }
  }
  else
  {
      std::cout << "Read program from binary." << std::endl;
  }

  printf("attempting to create input buffer\n");
  fflush(stdout);
	cl_mem input_buffer;
	input_buffer = CL_CHECK_ERR(clCreateBuffer(context, CL_MEM_READ_ONLY, sizeof(double)*NUM_DATA*NUM_DATA, NULL, &_err));
	cl_mem mask;
	mask = CL_CHECK_ERR(clCreateBuffer(context, CL_MEM_READ_ONLY, sizeof(short)*NUM_DATA*NUM_DATA, NULL, &_err));

  printf("attempting to create output buffer\n");
  fflush(stdout);
	cl_mem output_buffer;
	output_buffer = CL_CHECK_ERR(clCreateBuffer(context, CL_MEM_WRITE_ONLY, sizeof(double)*NUM_DATA*NUM_DATA, NULL, &_err));

  memObjects[0] = input_buffer;
  memObjects[1] = output_buffer;

  long long ldc = NUM_DATA;

  double m0 = 1.0;
  double m1 = 1.0;
  double m2 = 1.0;
  double m3 = 1.0;
  double m4 = 1.0;
  double m5 = 1.0;
  double m6 = 1.0;
  double m7 = 1.0;
  double m8 = 1.0;

  printf("attempting to create kernel\n");
  fflush(stdout);
	kernel = CL_CHECK_ERR(clCreateKernel(program, "mask_dfilter", &_err));
  printf("setting up kernel args cl_mem:%lx \n",input_buffer);
  fflush(stdout);
	CL_CHECK(clSetKernelArg(kernel, 0, sizeof(input_buffer), &input_buffer));
	CL_CHECK(clSetKernelArg(kernel, 1, sizeof(output_buffer), &output_buffer));
	CL_CHECK(clSetKernelArg(kernel, 2, sizeof(ldc), (&ldc)));
	CL_CHECK(clSetKernelArg(kernel, 3, sizeof(mask), (&mask)));
	CL_CHECK(clSetKernelArg(kernel, 4, sizeof(m0), (&m0)));
	CL_CHECK(clSetKernelArg(kernel, 5, sizeof(m1), (&m1)));
	CL_CHECK(clSetKernelArg(kernel, 6, sizeof(m2), (&m2)));
	CL_CHECK(clSetKernelArg(kernel, 7, sizeof(m3), (&m3)));
	CL_CHECK(clSetKernelArg(kernel, 8, sizeof(m4), (&m4)));
	CL_CHECK(clSetKernelArg(kernel, 9, sizeof(m5), (&m5)));
	CL_CHECK(clSetKernelArg(kernel, 10, sizeof(m6), (&m6)));
	CL_CHECK(clSetKernelArg(kernel, 11, sizeof(m7), (&m7)));
	CL_CHECK(clSetKernelArg(kernel, 12, sizeof(m8), (&m8)));

  printf("attempting to enqueue write buffer\n");
  fflush(stdout);
	for (int i=0; i<NUM_DATA*NUM_DATA; i++) {
    double in = ((double)rand()/(double)(RAND_MAX)) * 100.0;;
		CL_CHECK(clEnqueueWriteBuffer(queue, input_buffer, CL_TRUE, i*sizeof(double), 8, &in, 0, NULL, NULL));
    short m = i%2;
		CL_CHECK(clEnqueueWriteBuffer(queue, mask, CL_TRUE, i*sizeof(short), 2, &m, 0, NULL, NULL));
	}

	cl_event kernel_completion;
	size_t global_offset[2] = { 1, 1};
	size_t global_work_size[2] = { NUM_DATA - 2, NUM_DATA - 2};//avoid the edges
  const size_t local_work_size[2] = { 64, 1 };
  printf("attempting to enqueue kernel\n");
  fflush(stdout);
	CL_CHECK(clEnqueueNDRangeKernel(queue, kernel, 2, global_offset, global_work_size, local_work_size, 0, NULL, &kernel_completion));
  printf("Enqueue'd kerenel\n");
  fflush(stdout);
  cl_ulong time_start, time_end;
  CL_CHECK(clWaitForEvents(1, &kernel_completion));
  CL_CHECK(clGetEventProfilingInfo(kernel_completion, CL_PROFILING_COMMAND_START, sizeof(time_start), &time_start, NULL));
  CL_CHECK(clGetEventProfilingInfo(kernel_completion, CL_PROFILING_COMMAND_END, sizeof(time_end), &time_end, NULL));
  double elapsed = time_end - time_start;
  printf("time(ns):%lg\n",elapsed);
	CL_CHECK(clReleaseEvent(kernel_completion));

	printf("Result:");
	for (int i=0; i<NUM_DATA*NUM_DATA; i++) {
		double data;
		CL_CHECK(clEnqueueReadBuffer(queue, output_buffer, CL_TRUE, i*sizeof(double), 8, &data, 0, NULL, NULL));
		//printf(" %lg", data);
	}
	printf("\n");

	CL_CHECK(clReleaseMemObject(memObjects[0]));
	CL_CHECK(clReleaseMemObject(memObjects[1]));

	CL_CHECK(clReleaseKernel(kernel));
	CL_CHECK(clReleaseProgram(program));
	CL_CHECK(clReleaseContext(context));

	return 0;
}

