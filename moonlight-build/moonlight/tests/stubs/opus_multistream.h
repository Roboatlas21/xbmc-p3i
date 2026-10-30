#pragma once
#define OPUS_OK 0
typedef struct { int placeholder; } OpusMSDecoder;
OpusMSDecoder *opus_multistream_decoder_create(int,int,int,int,const unsigned char *,int *);
void opus_multistream_decoder_destroy(OpusMSDecoder *);
int opus_multistream_decode(OpusMSDecoder *,const void *,int,short *,int,int);
