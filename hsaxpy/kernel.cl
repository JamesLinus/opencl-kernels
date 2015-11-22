__kernel void hsaxpy(__global half *src, __global float *dst, float factor)
{
  long i = get_global_id(0);
  dst[i] += src[i] * (factor);
}
