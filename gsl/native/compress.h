/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef GSL_NATIVE_COMPRESS_H
#define GSL_NATIVE_COMPRESS_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <sound/compress_offload.h>

namespace gsl_native
{

class CompressStream {
public:
    enum class Event { write_done, eos };

    CompressStream(int fd, const snd_compr_params &params, bool capture);
    ~CompressStream();
    CompressStream(const CompressStream &) = delete;
    CompressStream &operator=(const CompressStream &) = delete;

    int event_fd() const
    {
        return epoll;
    }

    bool capturing() const
    {
        return capture;
    }

    int configure(const snd_compr_params &params);
    int start();
    int stop();
    int flush();
    int write(const void *data, size_t size, size_t &accepted);
    int read(void *data, size_t size, size_t &count);
    int eos();
    int next_event(Event &id);
    int silence(bool trailing, uint32_t frames);

private:
    int fd;
    snd_compr_params params;
    bool capture;
    int epoll = -1;
    int wake = -1;
    bool watched = false;
    bool started = false;
    bool running = false;
    bool draining = false;
    uint32_t last = 0;
    uint64_t written = 0;
    uint64_t consumed = 0;
    std::deque<uint64_t> owed;
    std::deque<Event> events;
    std::thread drainer;
    std::mutex mutex;
    bool drained = false;
    bool partial = false;

    int watch(bool enable);
    int halt();
    void kick();
    void sample();
    void harvest();
    void release(uint64_t limit);
};

}

#endif
