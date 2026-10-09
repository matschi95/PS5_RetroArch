/* The remote core's screens (src/remote/ui/), on the host, with sample services: two sources,
 * Home and Office, a game on both, a download queue. Buttons are pressed and what the screens
 * did is checked; with a second argument, each screen's frame is written there as a picture
 * (tools/remote-preview.sh makes PNGs of them).
 */
#include "../src/remote/ui/remote_ui.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

namespace remote = ps5::remote;
namespace ui = ps5::remote::ui;

namespace
{
class SampleServices final : public ui::Services
{
  public:
    SampleServices()
    {
        status_.configured = true;
        status_.sources = {{"home", "Home", "http://192.168.1.20:3000", false, true, "", 214},
                           {"office", "Office", "http://nas.office:8080", false, false,
                            "The server did not answer (network or DNS).", 0}};
        remote::Game smw = make("home", "1", "Super Mario World", 524288);
        remote::Game smw_office = make("office", "70", "Super Mario World", 524288);
        remote::Game ff7 = make("home", "3", "Final Fantasy VII", 2147483648ull);
        remote::Game chrono = make("home", "71", "Chrono Trigger", 4194304);
        remote::Game zelda = make("home", "2", "The Legend of Zelda: A Link to the Past", 1048576);
        titles_ = {{"igdb:1070", {smw, smw_office}},
                   {"crc:2", {ff7}},
                   {"crc:3", {chrono}},
                   {"crc:4", {zelda}}};
    }
    static remote::Game make(const char *source, const char *id, const char *name, uint64_t size)
    {
        remote::Game game;
        game.source = source;
        game.id = id;
        game.name = name;
        game.platform = "snes";
        game.folder = name;
        game.file = std::string(name) + ".sfc";
        game.size = size;
        return game;
    }
    remote::Status status() override
    {
        return status_;
    }
    std::vector<remote::Title> titles() override
    {
        return titles_;
    }
    std::vector<remote::Download> downloads() override
    {
        return downloads_;
    }
    bool enqueue(const std::string &source, const std::string &id, bool first) override
    {
        enqueued.push_back(source + "/" + id + (first ? " first" : ""));
        for (auto &download : downloads_)
            if (download.source == source && download.id == id)
            {
                download.state = remote::State::queued;
                download.error.clear();
                return true;
            }
        remote::Download download;
        download.source = source;
        download.id = id;
        download.total = 524288;
        downloads_.insert(first ? downloads_.begin() : downloads_.end(), download);
        return true;
    }
    bool cancel(const std::string &source, const std::string &id) override
    {
        cancelled.push_back(source + "/" + id);
        for (size_t i = 0; i < downloads_.size(); i++)
            if (downloads_[i].source == source && downloads_[i].id == id)
            {
                downloads_.erase(downloads_.begin() + long(i));
                return true;
            }
        return false;
    }
    void refresh() override
    {
        refreshed++;
        status_.sources[0].refreshing = true;
    }
    std::string placed(const std::string &source, const std::string &id) override
    {
        return source == "home" && id == placed_id ? "/app0/content/SNES/Chrono Trigger" : "";
    }
    bool remove(const remote::Game &game, const std::string &, std::string *) override
    {
        removed.push_back(game.id);
        placed_id.clear();
        return true;
    }
    std::string play(const remote::Game &, const std::string &launch,
                     const std::string &core) override
    {
        played.push_back(launch + " with " + core);
        return "LoadExec refused";
    }
    double now() override
    {
        return clock;
    }

    remote::Status status_;
    std::vector<remote::Title> titles_;
    std::vector<remote::Download> downloads_;
    std::vector<std::string> enqueued, cancelled, removed, played;
    std::string placed_id = "71";
    int refreshed = 0;
    double clock = 100;
};

std::vector<uint32_t> pixels(ui::frame_width *ui::frame_height);
std::string pictures;

/* Draws a screen; with a folder for pictures, writes the frame there (raw XRGB8888). */
void shot(ui::Screen &screen, const char *name)
{
    ui::Canvas canvas(pixels.data());
    screen.draw(canvas);
    if (pictures.empty())
        return;
    std::FILE *file = std::fopen((pictures + "/" + name + ".rgb").c_str(), "wb");
    assert(file);
    std::fwrite(pixels.data(), sizeof(uint32_t), pixels.size(), file);
    std::fclose(file);
}

/* Whether a row band of the frame holds anything but the background and the panel: drawn. */
bool drawn(int top, int bottom)
{
    for (int y = top; y < bottom; y++)
        for (unsigned x = 0; x < ui::frame_width; x++)
        {
            const uint32_t p = pixels[size_t(y) * ui::frame_width + x];
            if (p != ui::color::background && p != ui::color::panel)
                return true;
        }
    return false;
}

remote::Stub stub_of(std::vector<std::pair<std::string, std::string>> games)
{
    remote::Stub stub;
    stub.platform = "snes";
    stub.core = "/app0/cores/snes9x_libretro.so";
    stub.name = "Super Mario World";
    stub.games = std::move(games);
    return stub;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc > 1)
        pictures = argv[1];

    /* Two sources have the game: which one first; then it downloads, first in the queue. */
    {
        SampleServices services;
        auto screen = ui::open(stub_of({{"home", "1"}, {"office", "70"}}), services);
        shot(*screen, "10-source");
        assert(services.enqueued.empty());
        screen->input(ui::down);
        screen->input(ui::cross);
        assert(services.enqueued.size() == 1 && services.enqueued[0] == "office/70 first");
        shot(*screen, "11-waiting");
        services.downloads_[0].state = remote::State::downloading;
        services.downloads_[0].done = 200000;
        services.downloads_[0].rate = 3500000;
        shot(*screen, "12-downloading");
        assert(drawn(230, 250)); /* the bar */
        /* Circle: it goes on in the background. */
        screen->input(ui::circle);
        assert(screen->finished() && services.cancelled.empty());
    }
    /* Failed from one of two sources: opened again, the source is chosen anew; the other
     * one's download takes its place. */
    {
        SampleServices services;
        services.enqueue("office", "70", false);
        services.downloads_[0].state = remote::State::failed;
        services.enqueued.clear();
        auto screen = ui::open(stub_of({{"home", "1"}, {"office", "70"}}), services);
        assert(services.enqueued.empty()); /* asks first */
        screen->input(ui::cross);
        assert(services.enqueued.size() == 1 && services.enqueued[0] == "home/1 first");
        assert(services.cancelled.size() == 1 && services.cancelled[0] == "office/70");
    }
    /* Square cancels. */
    {
        SampleServices services;
        auto screen = ui::open(stub_of({{"home", "3"}}), services);
        assert(services.enqueued.size() == 1 && services.enqueued[0] == "home/3 first");
        screen->input(ui::square);
        assert(screen->finished() && services.cancelled.size() == 1);
    }
    /* A failed download: Cross tries again. */
    {
        SampleServices services;
        auto screen = ui::open(stub_of({{"home", "3"}}), services);
        services.downloads_[0].state = remote::State::failed;
        services.downloads_[0].done = 1u << 20;
        services.downloads_[0].error = "The server answered with status 500";
        shot(*screen, "13-failed");
        screen->input(ui::cross);
        assert(services.enqueued.size() == 2 && !screen->finished());
    }
    /* Downloaded already: it starts once "Starting the game..." is up (not while it is
     * drawn); a start that fails says so. */
    {
        SampleServices services;
        auto screen = ui::open(stub_of({{"home", "71"}}), services);
        screen->input(0);
        assert(services.played.empty());
        shot(*screen, "14-starting");
        assert(services.played.empty());
        screen->input(0);
        assert(services.played.size() == 1 &&
               services.played[0] == "/app0/content/SNES/Chrono Trigger/Chrono Trigger.sfc with "
                                     "/app0/cores/snes9x_libretro.so");
        shot(*screen, "15-not-started");
        screen->input(0);
        assert(services.played.size() == 1); /* once */
    }
    /* A stub of a game the sources no longer have. */
    {
        SampleServices services;
        auto screen = ui::open(stub_of({{"home", "99"}}), services);
        shot(*screen, "16-gone");
        screen->input(ui::circle);
        assert(screen->finished());
    }
    /* Downloads: the sources, the queue, the games downloaded. */
    {
        SampleServices services;
        services.enqueue("home", "3", false);
        services.downloads_[0].state = remote::State::downloading;
        services.downloads_[0].done = 1073741824ull;
        services.downloads_[0].total = 2147483648ull;
        services.downloads_[0].rate = 12582912;
        services.enqueue("home", "2", false);
        services.downloads_[1].state = remote::State::failed;
        services.downloads_[1].error = "Not enough free space";
        remote::Stub stub;
        stub.screen = "downloads";
        auto screen = ui::open(stub, services);
        shot(*screen, "20-downloads");
        screen->input(ui::cross); /* Home: read again */
        assert(services.refreshed == 1);
        shot(*screen, "21-reading");
        services.status_.sources[0].refreshing = false;
        shot(*screen, "22-read");
        /* The failed one: tried again; then cancelled. */
        screen->input(ui::down);
        screen->input(ui::down);
        screen->input(ui::down);
        screen->input(ui::cross);
        assert(services.enqueued.back() == "home/2");
        screen->input(ui::square);
        assert(services.cancelled.back() == "home/2");
        /* The game downloaded (its row moved up into focus): Square twice deletes it. */
        screen->input(ui::square);
        assert(services.removed.empty());
        shot(*screen, "23-delete");
        screen->input(0); /* frames without a press keep it asked */
        screen->input(ui::square);
        assert(services.removed.size() == 1 && services.removed[0] == "71");
        screen->input(ui::circle);
        assert(screen->finished());
    }
    /* No source set up. */
    {
        SampleServices services;
        services.status_ = remote::Status();
        remote::Stub stub;
        stub.screen = "downloads";
        auto screen = ui::open(stub, services);
        shot(*screen, "24-no-source");
    }
    std::puts("remote ui: the source dialog, the download dialog (waiting, downloading, failed, "
              "starting), a game gone, Downloads (sources, queue, deleting) PASS");
    return 0;
}
