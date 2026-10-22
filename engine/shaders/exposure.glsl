#ifndef EXPOSURE_GLSL
#define EXPOSURE_GLSL
const uint HISTOGRAM_BINS = 256u;
// Faixa de luminância medida (log2): do escuro de uma noite sem lua ao sol forte.
const float MIN_LOG_LUM = -14.0;
const float MAX_LOG_LUM = 10.0;
#endif
