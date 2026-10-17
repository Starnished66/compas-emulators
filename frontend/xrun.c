#include "xrun.h"

#include <errno.h>

bool xrun_write_all(const xrun_device_t * dev, xrun_stats_t * stats, const int16_t * data, unsigned frames) {
    unsigned recoveries = 0;
    while (frames) {
        errno = 0;
        int n = dev->write(dev->ctx, data, frames);
        if (n > 0) {
            if ((unsigned) n > frames) n = (int) frames;
            data += (size_t) n * 2;
            frames -= (unsigned) n;
            continue;
        }
        int err = n < 0 ? errno : 0;
        if (n < 0 && (err == EPIPE || err == ESTRPIPE) && recoveries < XRUN_MAX_RECOVERIES) {
            if (err == EPIPE) stats->underruns++;
            else stats->suspends++;
            recoveries++;
            stats->prepares++;
            if (!dev->prepare(dev->ctx)) {
                stats->errors++;
                return false;
            }
            continue;
        }
        stats->errors++;
        return false;
    }
    return true;
}
