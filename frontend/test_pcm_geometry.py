#!/usr/bin/env python3
"""Compile the real device-open function against a PCM driver negotiation fake."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parent
source = (root / 'frontend.c').read_text()
start = source.index('static bool audio_open_device_period(')
end = source.index('\nstatic bool audio_open(unsigned rate)', start)
preamble = r'''
#include "audio_policy.h"
#include <assert.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#define PCM_FORMAT_S16_LE 0
#define PCM_OUT 1
#define PCM_NORESTART 2
#define AUDIO_OPEN_RETRY_MS 100
#define AUDIO_OPEN_TIMEOUT_MS 2000
#define STRETCH_Q16_ONE 65536
struct pcm_config {
 unsigned channels, rate, period_size, period_count, format;
 unsigned long start_threshold, stop_threshold, avail_min;
};
struct pcm { struct pcm_config config; unsigned buffer; } fake;
static struct pcm *pcm;
static unsigned device_rate = 48000, audio_active_period, audio_short_underrun_base;
static struct { unsigned underruns; } audio_xr;
static unsigned stretch, stretch_ratio_q16;
static double stretch_next_log;
static unsigned opens, closes, mode;
static struct pcm *pcm_open(unsigned a,unsigned b,unsigned c,const struct pcm_config *cfg) {
 (void)a;(void)b;(void)c; ++opens;
 fake.config = *cfg;
 fake.buffer = cfg->period_size * cfg->period_count; /* tinyalsa cached REQUEST */
 if (mode == 1 || (mode == 2 && opens == 1)) fake.config.period_size = 1024;
 if (mode == 2 && opens == 2) fake.config.period_size = 2048;
 if (mode == 3) fake.config.period_size = 0;
 if (mode == 4) fake.config.period_count = UINT_MAX;
 if (!cfg->avail_min) fake.config.avail_min = fake.config.period_size;
 return &fake;
}
static int pcm_is_ready(struct pcm *p) { return p != NULL; }
static const struct pcm_config *pcm_get_config(struct pcm *p) { return &p->config; }
static unsigned pcm_get_buffer_size(struct pcm *p) { return p->buffer; }
static void pcm_close(struct pcm *p) { assert(p); ++closes; }
static const char *pcm_get_error(struct pcm *p) { (void)p; return "fake failure"; }
static void runner_log(const char *fmt, ...) { (void)fmt; }
static void stretch_init(unsigned *s,unsigned capacity,unsigned rate) { (void)rate; *s=capacity; }
static void usleep(unsigned n) { (void)n; }
'''
tests = r'''
static void reset(unsigned m) { opens=closes=0; mode=m; pcm=NULL; }
int main(void) {
 for (unsigned request=256;request<=512;request*=2) {
  reset(1); assert(audio_open_device_period(request,false));
  assert(opens==2 && closes==1 && audio_active_period==1024 && stretch==4096);
  assert(fake.config.start_threshold==4096 && fake.config.stop_threshold==4096);
#ifdef FRONTEND_TEST_PCM_AVAIL_MIN_FRAMES
  assert(fake.config.avail_min==FRONTEND_TEST_PCM_AVAIL_MIN_FRAMES);
#else
  assert(fake.config.avail_min==1024);
#endif
 }
 reset(0); assert(audio_open_device_period(1024,false)); assert(opens==1 && closes==0);
 reset(2); assert(!audio_open_device_period(512,true)); assert(opens==2 && closes==2 && !pcm);
 reset(3); assert(!audio_open_device_period(512,true)); assert(opens==1 && closes==1 && !pcm);
 reset(4); assert(!audio_open_device_period(512,true)); assert(opens==1 && closes==1 && !pcm);
 uint32_t capacity=0;
 assert(!audio_policy_capacity(UINT32_MAX,4,&capacity));
 assert(!audio_policy_capacity(8192,4,&capacity));
 assert(!audio_policy_capacity(1024,0,&capacity));
 assert(!audio_policy_capacity(1024,4,NULL));
 puts("real PCM open negotiation tests: ok");
}
'''
with tempfile.TemporaryDirectory(prefix='compas-pcm-') as temporary:
    test = Path(temporary) / 'test.c'
    test.write_text(preamble + source[start:end] + tests)
    for flags in ([], ['-DFRONTEND_TEST_PCM_AVAIL_MIN_FRAMES=256']):
        binary = Path(temporary) / 'test'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                        *flags, '-I' + str(root), str(test), str(root / 'audio_policy.c'),
                        '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
