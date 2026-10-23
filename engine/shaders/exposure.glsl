#ifndef EXPOSURE_GLSL
#define EXPOSURE_GLSL
const uint HISTOGRAM_BINS = 256u;
// Faixa medida em log2(luminância absoluta, nits): de uma noite sem lua (~0,0001)
// até neve ao sol (~30.000) com folga.
const float MIN_LOG_LUM = -14.0;
const float MAX_LOG_LUM = 18.0;
#endif
