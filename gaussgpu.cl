__kernel void blur(__global unsigned char *pixels, __global unsigned char *out,
                   __global float *ckernel, __constant int *rows,
                   __constant int *cols, __constant int *cKernelDimension) {
  int idx = get_global_id(0);
  int currentRow = (idx / 4) / (*cols);
  int currentCol = (idx / 4) % (*cols);
  int colorOffset = idx % 4;
  float acc = 0;
  if (colorOffset != 3) {
    int i, j;
    for (j = 0; j < (*cKernelDimension); j++) {
      int y = currentRow + (j - (*cKernelDimension / 2));
      if (y < 0)
        y = 0;
      if(y >= *rows)
        y = *rows -1;

      for (i = 0; i < (*cKernelDimension); i++) {
        int x = currentCol + (i - (*cKernelDimension / 2));
        if (x < 0)
          x = 0;
        if (x > *cols)
          x = *cols;
        acc += (float)((float)(pixels[(y * (*cols) + x)*4 + colorOffset]) * ckernel[(j * (*cKernelDimension)) + i]);
      }
    }
    if (acc >= 255)
      acc = 255;
    out[idx] = (unsigned char)acc;
  } else {
    out[idx] = 255;
  }
}