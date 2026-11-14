/* SPDX-License-Identifier: BSD-3-Clause */
#include "compress.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <system_error>
#include <poll.h>
#include <sound/asound.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace gsl_native
{

CompressStream::CompressStream(int fd, const snd_compr_params &params,
                               bool capture)
    : fd(fd), params(params), capture(capture)
{
    epoll = epoll_create1(EPOLL_CLOEXEC);
    if (epoll < 0)
        throw std::system_error(errno, std::generic_category());

    wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);

    epoll_event event{};
    event.events = EPOLLIN;
    event.data.fd = wake;

    int status = wake < 0 || epoll_ctl(epoll, EPOLL_CTL_ADD, wake, &event) ?
        -errno : watch(true);
    if (status) {
        if (wake >= 0)
            ::close(wake);

        ::close(epoll);

        throw std::system_error(-status, std::generic_category());
    }
}

CompressStream::~CompressStream()
{
    halt();
    ::close(wake);
    ::close(epoll);
}

/*
 * Polling a draining compress stream moves it to SETUP and ends the drain
 * early, so the descriptor leaves the epoll set while a drain is in flight.
 */
int CompressStream::watch(bool enable)
{
    if (capture || watched == enable)
        return 0;

    epoll_event event{};
    event.events = EPOLLOUT | EPOLLET;
    event.data.fd = fd;

    if (epoll_ctl(epoll, enable ? EPOLL_CTL_ADD : EPOLL_CTL_DEL, fd, &event))
        return -errno;

    watched = enable;

    return 0;
}

int CompressStream::halt()
{
    int status = ::ioctl(fd, SNDRV_COMPRESS_STOP) ? -errno : 0;
    if (status == -EPERM)
        status = 0;

    if (drainer.joinable())
        drainer.join();

    uint64_t count;
    while (::read(wake, &count, sizeof(count)) > 0) {
    }

    draining = false;
    running = false;
    owed.clear();
    events.clear();
    written = consumed = 0;

    snd_compr_tstamp position{};
    if (!::ioctl(fd, SNDRV_COMPRESS_TSTAMP, &position))
        last = position.copied_total;

    int watching = watch(true);

    return status ? status : watching;
}

void CompressStream::kick()
{
    if (!started || running || draining)
        return;

    if (!::ioctl(fd, SNDRV_COMPRESS_START))
        running = true;
}

void CompressStream::release(uint64_t limit)
{
    while (!owed.empty() && owed.front() <= limit) {
        owed.pop_front();
        events.push_back(Event::write_done);
    }
}

void CompressStream::sample()
{
    snd_compr_tstamp position{};
    if (::ioctl(fd, SNDRV_COMPRESS_TSTAMP, &position))
        return;

    consumed = std::min(written,
                        consumed + uint32_t(position.copied_total - last));
    last = position.copied_total;
    release(consumed);
}

void CompressStream::harvest()
{
    epoll_event ready[2];

    epoll_wait(epoll, ready, 2, 0);

    if (draining) {
        {
            std::lock_guard<std::mutex> lock(mutex);

            if (!drained)
                return;
        }

        drainer.join();

        uint64_t count;
        while (::read(wake, &count, sizeof(count)) > 0) {
        }

        draining = false;
        running = partial;
        consumed = written;
        release(written);
        events.push_back(Event::eos);

        snd_compr_tstamp position{};
        if (!::ioctl(fd, SNDRV_COMPRESS_TSTAMP, &position))
            last = position.copied_total;

        watch(true);

        return;
    }

    sample();
}

int CompressStream::configure(const snd_compr_params &value)
{
    if (!std::memcmp(&value, &params, sizeof(value)))
        return 0;

    if (!started)
        return -EBUSY;

    snd_compr_params copy = value;
    if (::ioctl(fd, SNDRV_COMPRESS_SET_PARAMS, &copy))
        return -errno;

    params = value;

    return 0;
}

int CompressStream::start()
{
    if (capture) {
        if (!running && ::ioctl(fd, SNDRV_COMPRESS_START))
            return -errno;

        started = running = true;

        return 0;
    }

    started = true;
    kick();

    return 0;
}

int CompressStream::stop()
{
    started = false;

    return halt();
}

int CompressStream::flush()
{
    return capture ? -EOPNOTSUPP : halt();
}

int CompressStream::read(void *data, size_t size, size_t &count)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    bool failed = false;

    while (running) {
        ssize_t result = ::read(fd, data, size);
        if (result > 0) {
            count = result;

            return 0;
        }

        if (result < 0 && errno != EAGAIN)
            return -errno;

        if (failed)
            return -EBADFD;

        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0)
            return -ETIMEDOUT;

        pollfd wait[] = {{fd, POLLIN, 0}, {wake, POLLIN, 0}};

        int ready = poll(wait, 2, left);
        if (ready < 0 && errno != EINTR)
            return -errno;

        if (ready <= 0)
            continue;

        if (wait[1].revents)
            return -EBADFD;

        failed = wait[0].revents & (POLLERR | POLLHUP | POLLNVAL);
    }

    return -EBADFD;
}

int CompressStream::write(const void *data, size_t size, size_t &accepted)
{
    ssize_t count = ::write(fd, data, size);
    if (count < 0)
        return -errno;

    accepted = count;

    uint64_t end = written + accepted;
    size_t fragment = params.buffer.fragment_size;

    while (written < end) {
        written = std::min<uint64_t>(written + fragment, end);
        owed.push_back(written);
    }

    kick();

    return 0;
}

int CompressStream::eos()
{
    if (capture)
        return -EOPNOTSUPP;

    if (draining)
        return 0;

    bool next = !::ioctl(fd, SNDRV_COMPRESS_NEXT_TRACK);

    int status = watch(false);
    if (status)
        return status;

    drained = false;
    partial = false;

    try {
        drainer = std::thread([this, next] {
            int result = ::ioctl(fd, next ? SNDRV_COMPRESS_PARTIAL_DRAIN :
                                            SNDRV_COMPRESS_DRAIN);

            {
                std::lock_guard<std::mutex> lock(mutex);

                partial = next && !result;
                drained = true;
            }

            uint64_t count = 1;
            ssize_t ignored = ::write(wake, &count, sizeof(count));
            (void)ignored;
        });
    } catch (const std::system_error &) {
        watch(true);

        return -EAGAIN;
    }

    draining = true;

    return 0;
}

int CompressStream::next_event(Event &id)
{
    harvest();

    if (events.empty())
        return -EAGAIN;

    id = events.front();
    events.pop_front();

    return 0;
}

int CompressStream::silence(bool trailing, uint32_t frames)
{
    if (capture)
        return -EOPNOTSUPP;

    snd_compr_metadata metadata{};

    metadata.key = trailing ? SNDRV_COMPRESS_ENCODER_PADDING :
                              SNDRV_COMPRESS_ENCODER_DELAY;
    metadata.value[0] = frames;

    return ::ioctl(fd, SNDRV_COMPRESS_SET_METADATA, &metadata) ? -errno : 0;
}

}
