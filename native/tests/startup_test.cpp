/* SPDX-License-Identifier: BSD-3-Clause */
#include "ar_topology_startup.h"

#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <unistd.h>

struct mixer {
    std::string name;
};

static std::map<unsigned, mixer> cards;
static int open_mixers;

extern "C" struct mixer *mixer_open(unsigned card)
{
    auto found = cards.find(card);
    if (found == cards.end())
        return nullptr;

    open_mixers++;

    return &found->second;
}

extern "C" const char *mixer_get_name(struct mixer *mixer)
{
    return mixer->name.c_str();
}

extern "C" void mixer_close(struct mixer *)
{
    open_mixers--;
}

static std::string directory;

static std::string file(const std::string &name, const std::string &data)
{
    std::string path = directory + "/" + name;
    std::ofstream(path, std::ios::binary) << data;

    return path;
}

static void card_lookup()
{
    unsigned card = 99;

    cards = {{0, {"pineapple-mtp-snd-card"}}, {3, {"SM8650-CERRO"}}};
    assert(!ar_topology::find_card("SM8650-CERRO", card));
    assert(card == 3);
    assert(!open_mixers);

    assert(ar_topology::find_card("SM8650-CERRO-2", card) == -ENODEV);
    assert(ar_topology::find_card("", card) == -EINVAL);
    assert(card == 3);

    cards[5] = {"SM8650-CERRO"};
    assert(ar_topology::find_card("SM8650-CERRO", card) == -EINVAL);
    assert(card == 3);
    assert(!open_mixers);
}

static void bindings_loading()
{
    std::unique_ptr<ar_topology::Bindings> bindings;

    assert(!ar_topology::load_bindings(file("bindings.json",
        R"({"version": 1, "frontends": [], "references": [],
            "selections": [], "hardware_gates": []})"), bindings));
    assert(bindings);

    for (const char *data : {R"({"version": 2, "frontends": [],
                                 "references": [], "selections": [],
                                 "hardware_gates": []})",
                             "{\"version\": 1,", "[1]"}) {
        std::unique_ptr<ar_topology::Bindings> rejected;
        assert(ar_topology::load_bindings(file("invalid.json", data),
                                          rejected) == -EINVAL);
        assert(!rejected);
    }

    std::unique_ptr<ar_topology::Bindings> missing;
    assert(ar_topology::load_bindings(directory + "/missing.json", missing) ==
           -ENOENT);
    assert(!missing);
}

int main()
{
    char path[] = "/tmp/ar-topology-startup-XXXXXX";
    assert(mkdtemp(path));
    directory = path;

    card_lookup();
    bindings_loading();

    assert(!system(("rm -rf " + directory).c_str()));

    return 0;
}
