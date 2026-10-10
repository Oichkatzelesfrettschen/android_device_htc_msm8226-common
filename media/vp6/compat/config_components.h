/*
 * Component switches for FFmpeg's ARM assembly; only the VP6 decoder and its
 * DSP helpers are built, so the H.264, RV and VC-1 chroma paths stay off.
 */
#ifndef A11_VP6_CONFIG_COMPONENTS_H
#define A11_VP6_CONFIG_COMPONENTS_H
#define CONFIG_H264_DECODER 0
#define CONFIG_RV40_DECODER 0
#define CONFIG_VC1_DECODER 0
#define CONFIG_WMV3_DECODER 0
#define CONFIG_VC1DSP 0
#endif
