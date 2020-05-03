__kernel void pixelate(__global unsigned char *pixels, __global unsigned char *out,
                   __global int *pixelSize, __constant int *rows,
                   __constant int *cols) {
  int idx = get_global_id(0);
  int currentRow = ((idx / 4) / (*cols)) * (*pixelSize);
  int currentCol = ((idx / 4) % (*cols)) * (*pixelSize);
  //printf("%i\n",*pixelSize);
  int colorOffset = idx % 4;
  float acc = 0;
  if (colorOffset != 3) {
    for(int j = 0; j < *pixelSize;j++)
    {
      int y = currentRow+j;
      if(j < 0)
        j = 0;
      if(j >= *rows)
        j = *rows -1;

      for(int i = 0; i < *pixelSize;i++)
      {
        int x = currentCol + i;
        if(x < 0)
          x = 0;
        if(x >= *cols)
          x = *cols -1;

        acc = acc + (float)pixels[(y*(*cols) + x)*4+colorOffset];
      }
    }
    acc /= *pixelSize * *pixelSize;
        if(idx == 0)
    if (acc > 255)
      acc = 255;
    for(int j = 0; j < *pixelSize;j++)
    {
      int y = currentRow+j;
      if(y < 0)
        y = 0;
      if(y >= *rows)
        y = *rows -1;

      for(int i = 0; i < *pixelSize;i++)
      {
        int x = currentCol + i;
        if(x < 0)
          x = 0;
        if(x >= *cols)
          x = *cols -1;

        out[(y*(*cols) + x)*4+colorOffset] = (unsigned char)acc;
      }
    }
  }
}