/* SPDX-License-Identifier: BSD-3-Clause */
#include "ar_topology_startup.h"

#include <cerrno>
#include <fstream>
#include <new>
#include <sstream>
#include <stdexcept>
#include <json/json.h>
#include <tinyalsa/asoundlib.h>

namespace ar_topology
{
namespace
{

constexpr unsigned max_cards = 32;

}

int find_card(const std::string &name, unsigned &card)
{
    if (name.empty())
        return -EINVAL;

    bool found = false;
    unsigned match = 0;

    for (unsigned index = 0; index < max_cards; index++) {
        struct mixer *mixer = mixer_open(index);
        if (!mixer)
            continue;

        const char *value = mixer_get_name(mixer);
        bool matches = value && name == value;
        mixer_close(mixer);

        if (!matches)
            continue;

        if (found)
            return -EINVAL;

        found = true;
        match = index;
    }

    if (!found)
        return -ENODEV;

    card = match;

    return 0;
}

int load_bindings(const std::string &path, std::unique_ptr<Bindings> &bindings)
{
    try {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return errno ? -errno : -EIO;

        std::ostringstream stream;
        stream << file.rdbuf();
        if (file.bad())
            return -EIO;

        std::string data = stream.str();
        Json::CharReaderBuilder builder;
        Json::CharReaderBuilder::strictMode(&builder.settings_);
        std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
        Json::Value input;
        std::string errors;

        if (!reader->parse(data.data(), data.data() + data.size(), &input,
                           &errors))
            return -EINVAL;

        bindings.reset(new Bindings(input));

        return 0;
    } catch (const std::invalid_argument &) {
        return -EINVAL;
    } catch (const Json::Exception &) {
        return -EINVAL;
    } catch (const std::bad_alloc &) {
        return -ENOMEM;
    }
}

}
