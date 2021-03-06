
#pragma OPENCL EXTENSION cl_khr_fp64 : enable


__kernel void sdaxpy(__global float *src, __global double *dst, double factor)
{
  long i = get_global_id(0);
  dst[i] += src[i] * (factor);
}
