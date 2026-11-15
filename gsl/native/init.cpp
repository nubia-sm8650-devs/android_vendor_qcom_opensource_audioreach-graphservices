/* SPDX-License-Identifier: BSD-3-Clause */
#define LOG_TAG "gsl-native"

#include "gsl_native.h"

#include <ar_topology_log.h>

#include <ar_topology_startup.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <acdb.h>
#include <ar_osal_error.h>
#include <fcntl.h>
#include <gsl_intf.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <tinyalsa/asoundlib.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

namespace
{

class CardWatch {
public:
    ~CardWatch()
    {
        if (worker.joinable()) {
            uint64_t count = 1;
            ssize_t status;

            do {
                status = ::write(stop, &count, sizeof(count));
            } while (status < 0 && errno == EINTR);

            worker.join();
        }

        if (card >= 0)
            ::close(card);

        if (stop >= 0)
            ::close(stop);
    }

    int start(unsigned index, gsl_native::CardLost lost)
    {
        char path[32];

        snprintf(path, sizeof(path), "/dev/snd/controlC%u", index);

        card = ::open(path, O_RDONLY | O_CLOEXEC);
        if (card < 0)
            return -errno;

        stop = eventfd(0, EFD_CLOEXEC);
        if (stop < 0)
            return -errno;

        worker = std::thread([this, lost] {
            pollfd fds[2] = {{card, 0, 0}, {stop, POLLIN, 0}};

            for (;;) {
                if (::poll(fds, 2, -1) < 0) {
                    if (errno == EINTR)
                        continue;

                    return;
                }

                if (fds[1].revents)
                    return;

                if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                    lost();

                    return;
                }
            }
        });

        return 0;
    }

private:
    int card = -1;
    int stop = -1;
    std::thread worker;
};

struct Startup {
    CardWatch watch;
    struct mixer *mixer = nullptr;
    bool acdb = false;
    bool started = false;

    ~Startup()
    {
        if (started)
            gsl_native::stop();

        if (mixer)
            mixer_close(mixer);

        if (acdb)
            acdb_deinit();
    }
};

std::unique_ptr<Startup> startup;

void exit_on_card_loss()
{
    ALOGE("the sound card is gone, exiting");
    _exit(EXIT_FAILURE);
}

std::string card_name()
{
#ifdef __ANDROID__
    char value[PROP_VALUE_MAX] = {};

    __system_property_get(ar_topology::card_property, value);

    return value;
#else
    const char *value = getenv("AR_TOPOLOGY_CARD");

    return value ? value : "";
#endif
}

}

namespace gsl_native
{

int init(const gsl_init_data &data, const std::string &name,
         const std::string &path, std::chrono::milliseconds wait, CardLost lost)
{
    if (startup)
        return -EALREADY;

    try {
        std::unique_ptr<Startup> pending(new Startup);
        std::unique_ptr<ar_topology::Bindings> bindings;

        int status = ar_topology::load_bindings(path, bindings);
        if (status)
            return LOG_ERRNO(status, "bindings %s: %s", path.c_str(),
                             strerror(-status));

        unsigned card;
        auto deadline = std::chrono::steady_clock::now() + wait;

        ALOGI("looking for sound card '%s'", name.c_str());

        for (;;) {
            status = ar_topology::find_card(name, card);
            if (status != -ENODEV ||
                std::chrono::steady_clock::now() >= deadline)
                break;

            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        if (status)
            return LOG_ERRNO(status, "sound card '%s': %s", name.c_str(),
                             strerror(-status));

        ALOGI("sound card '%s' is card %u", name.c_str(), card);

        if (acdb_init(reinterpret_cast<AcdbDataFiles *>(data.acdb_files),
                      reinterpret_cast<AcdbFile *>(data.acdb_delta_file)))
            return LOG_ERRNO(-EIO, "the ACDB files do not load");

        pending->acdb = true;

        pending->mixer = mixer_open(card);
        if (!pending->mixer)
            return LOG_ERRNO(-ENODEV, "the mixer of card %u does not open",
                             card);

        status = start(card, pending->mixer, std::move(bindings));
        if (status)
            return status;

        pending->started = true;

        if (lost) {
            status = pending->watch.start(card, lost);
            if (status)
                return LOG_ERRNO(status, "card %u cannot be watched: %s", card,
                                 strerror(-status));
        }

        startup = std::move(pending);

        return 0;
    } catch (const std::bad_alloc &) {
        return -ENOMEM;
    } catch (const std::invalid_argument &) {
        return -EINVAL;
    } catch (const std::system_error &error) {
        return -error.code().value();
    }
}

void deinit()
{
    startup.reset();
}

}

extern "C" int32_t gsl_init(struct gsl_init_data *init_data)
{
    if (!init_data)
        return AR_EBADPARAM;

    int status = gsl_native::init(*init_data, card_name(),
                                  ar_topology::bindings_path,
                                  gsl_native::card_wait, exit_on_card_loss);

    switch (status) {
    case 0:
        return AR_EOK;
    case -ENOMEM:
        return AR_ENOMEMORY;
    case -EINVAL:
        return AR_EBADPARAM;
    case -ENODEV:
    case -ENOENT:
        return AR_ENOTEXIST;
    case -EALREADY:
        return AR_EALREADY;
    default:
        return AR_EFAILED;
    }
}

extern "C" void gsl_deinit(void)
{
    gsl_native::deinit();
}

extern "C" int32_t gsl_register_global_event_cb(gsl_global_cb_func_ptr, void *)
{
    return AR_EOK;
}
