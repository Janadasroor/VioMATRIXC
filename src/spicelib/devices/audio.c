#include "ngspice/ngspice.h"
#include "vsrc/vsrcdefs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Simple linked list for WAV data caching */
typedef struct sWavCache {
    VSRCwavData *data;
    struct sWavCache *next;
} WavCache;

static WavCache *s_cache = NULL;

static uint32_t read32le(unsigned char *p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

static uint16_t read16le(unsigned char *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

VSRCwavData *VSRCwavLoad(const char *filename) {
    WavCache *c = s_cache;
    while (c) {
        if (strcmp(c->data->filename, filename) == 0) return c->data;
        c = c->next;
    }

    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        fprintf(stderr, "Error: Could not open WAV file '%s'\n", filename);
        return NULL;
    }

    unsigned char header[44];
    if (fread(header, 1, 44, fp) < 44) {
        fprintf(stderr, "Error: WAV file '%s' too short\n", filename);
        fclose(fp);
        return NULL;
    }

    if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
        fprintf(stderr, "Error: '%s' is not a valid RIFF/WAVE file\n", filename);
        fclose(fp);
        return NULL;
    }

    /* Simple parser for fmt and data chunks */
    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t sampleRate = 0, dataSize = 0;
    
    fseek(fp, 12, SEEK_SET);
    unsigned char chunkHeader[8];
    while (fread(chunkHeader, 1, 8, fp) == 8) {
        uint32_t sz = read32le(chunkHeader + 4);
        if (memcmp(chunkHeader, "fmt ", 4) == 0) {
            unsigned char fmt[16];
            if (fread(fmt, 1, 16, fp) < 16) break;
            format = read16le(fmt);
            channels = read16le(fmt + 2);
            sampleRate = read32le(fmt + 4);
            bits = read16le(fmt + 14);
            if (sz > 16) fseek(fp, sz - 16, SEEK_CUR);
        } else if (memcmp(chunkHeader, "data", 4) == 0) {
            dataSize = sz;
            break; 
        } else {
            fseek(fp, sz, SEEK_CUR);
        }
        if (sz & 1) fseek(fp, 1, SEEK_CUR);
    }

    if (channels == 0 || sampleRate == 0 || bits == 0 || dataSize == 0) {
        fprintf(stderr, "Error: Invalid or unsupported WAV format in '%s'\n", filename);
        fclose(fp);
        return NULL;
    }

    int bytesPerSample = bits / 8;
    int numFrames = (int)(dataSize / (channels * bytesPerSample));
    
    VSRCwavData *dw = (VSRCwavData *)calloc(1, sizeof(VSRCwavData));
    dw->filename = strdup(filename);
    dw->sampleRate = (int)sampleRate;
    dw->channelCount = (int)channels;
    dw->numSamples = numFrames;
    dw->samples = (double **)calloc(channels, sizeof(double *));
    for (int i = 0; i < channels; i++) {
        dw->samples[i] = (double *)calloc((size_t)numFrames, sizeof(double));
    }

    unsigned char *buf = (unsigned char *)malloc((size_t)(channels * bytesPerSample));
    for (int f = 0; f < numFrames; f++) {
        if (fread(buf, 1, (size_t)(channels * bytesPerSample), fp) < (size_t)(channels * bytesPerSample)) break;
        for (int c = 0; c < channels; c++) {
            unsigned char *s = buf + c * bytesPerSample;
            double val = 0;
            if (format == 1) { /* PCM */
                if (bits == 8) val = (s[0] - 128.0) / 128.0;
                else if (bits == 16) val = (int16_t)read16le(s) / 32768.0;
                else if (bits == 24) {
                    int32_t v24 = (int32_t)(s[0] | (s[1] << 8) | (s[2] << 16));
                    if (v24 & 0x800000) v24 |= (int32_t)0xFF000000;
                    val = v24 / 8388608.0;
                }
                else if (bits == 32) val = (int32_t)read32le(s) / 2147483648.0;
            } else if (format == 3) { /* Float */
                if (bits == 32) {
                    float fv; memcpy(&fv, s, 4); val = (double)fv;
                } else if (bits == 64) {
                    double dv; memcpy(&dv, s, 8); val = dv;
                }
            }
            dw->samples[c][f] = val;
        }
    }
    free(buf);
    fclose(fp);

    /* Add to cache */
    WavCache *nc = (WavCache *)calloc(1, sizeof(WavCache));
    nc->data = dw;
    nc->next = s_cache;
    s_cache = nc;

    return dw;
}

double VSRCwavGetSample(VSRCwavData *data, int channel, double time) {
    if (!data || channel < 0 || channel >= data->channelCount || time < 0) return 0;

    double pos = time * data->sampleRate;
    int i = (int)floor(pos);
    if (i >= data->numSamples - 1) return (i < data->numSamples) ? data->samples[channel][data->numSamples-1] : 0;

    double frac = pos - i;
    return data->samples[channel][i] * (1.0 - frac) + data->samples[channel][i+1] * frac;
}
