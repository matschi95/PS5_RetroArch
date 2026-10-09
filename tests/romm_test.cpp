/* The RomM backend of the download sources (src/remote/romm/), on the host, against
 * tools/romm-mock-server.py: the address, the pages and what of them is a game, the sign-in,
 * the platforms asked for, a cover, a file from its start, from a byte on, from a server that
 * cannot go on and past its end; then the download sources with it, a game in a folder
 * downloaded whole. argv: the mock server's address, a scratch folder.
 */
#include "../src/remote/files.h"
#include "../src/remote/remote.h"
#include "../src/remote/backends.h"
#include "../src/remote/romm/romm_client.h"
#include "../src/remote/romm/romm_source.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace remote = ps5::remote;
namespace romm = ps5::remote::romm;
namespace files = ps5::remote::files;
using remote::Json;

namespace
{
std::string pattern(unsigned id, size_t size)
{
    std::string bytes(size, '\0');
    for (size_t i = 0; i < size; i++)
        bytes[i] = char((i * 7 + id) & 0xff);
    return bytes;
}

class Collect final : public remote::Receiver
{
  public:
    bool begin(bool from_start_) override
    {
        begun++;
        from_start = from_start_;
        return true;
    }
    bool take(const void *data, size_t size) override
    {
        bytes.append(static_cast<const char *>(data), size);
        return true;
    }
    bool stopped() override
    {
        return false;
    }
    std::string bytes;
    bool from_start = false;
    int begun = 0;
};

std::unique_ptr<remote::Source> source(const std::string &entry)
{
    Json settings;
    const bool parsed = Json::parse(entry, &settings);
    assert(parsed);
    std::string error;
    auto made = romm::make_source(settings, &error);
    assert(made && error.empty());
    return made;
}

const remote::SourceGame *find(const std::vector<remote::SourceGame> &games, const std::string &id)
{
    for (const auto &game : games)
        if (game.id == id)
            return &game;
    return nullptr;
}
} // namespace

int main(int argc, char **argv)
{
    assert(argc == 3);
    const std::string url = argv[1], root = argv[2];
    const remote::Stopped never = [] { return false; };

    assert(romm::normal_url(" nas:3000/ ") == "http://nas:3000" &&
           romm::normal_url("HTTPS://x/") == "https://x");
    assert(romm::normal_url("ftp://x").empty() && romm::normal_url("").empty());
    {
        /* The backends by what they can do; a RomM entry read for another file names it. */
        assert(remote::find_backend("romm") && remote::find_backend("romm")->source &&
               !remote::find_backend("ftp"));
        std::string error;
        assert(!remote::make_source("ftp", Json::record(), &error) &&
               error == "Unknown source type \"ftp\"");
        assert(!remote::make_source("", Json::record(), &error) &&
               error == "A source in sources.json has no \"type\"");
        assert(!romm::make_source(Json::parse("{\"type\":\"romm\"}"), &error) &&
               error == "No server address (\"url\") in sources.json");
        assert(!romm::Client::make(Json::record(), "save-sync.json", &error) &&
               error == "No server address (\"url\") in save-sync.json");
        std::vector<remote::SourceGame> games;
        assert(!romm::parse_page("{\"items\":{}}", &games) && !romm::parse_page("[]", &games));
        /* Firmware, each with its platform's names; one gone from the server's drive is not. */
        assert(remote::find_backend("romm")->firmware);
        std::vector<remote::FirmwareFile> firmware;
        assert(romm::parse_firmware(
            "[{\"id\":5,\"platform_id\":2,\"file_name\":\"scph5501.bin\",\"file_size_bytes\":"
            "524288,\"crc_hash\":\"47b2b88b\",\"md5_hash\":\"490f666e1afb15b7362b406ed1cea246\","
            "\"sha1_hash\":\"b05def971d8ec59f346f2d9ac21fb742e3eb6917\",\"is_verified\":true,"
            "\"missing_from_fs\":false},"
            "{\"id\":6,\"platform_id\":2,\"file_name\":\"gone.bin\",\"missing_from_fs\":true}]",
            "[{\"id\":2,\"slug\":\"ps\",\"fs_slug\":\"psx\",\"name\":\"PlayStation\"}]",
            &firmware));
        assert(firmware.size() == 1 && firmware[0].id == "5" && firmware[0].size == 524288 &&
               firmware[0].crc32 == "47b2b88b" && firmware[0].verified &&
               firmware[0].systems == std::vector<std::string>({"psx", "ps", "PlayStation"}));
        assert(!romm::parse_firmware("{}", "[]", &firmware));
    }

    /* A server older than the backends take: refused before anything else, saying so. */
    {
        auto old = source("{\"url\":\"" + url + "/old\",\"token\":\"rmm_test\"}");
        std::vector<remote::SourceGame> none;
        std::string why, version, needed;
        assert(!old->list(&none, &why, never) && old->too_old(&version, &needed));
        assert(version == "5.2.0" && needed == romm::minimum_version &&
               why == std::string("RomM 5.2.0 is too old: it needs RomM ") + romm::minimum_version +
                          " or newer");
        assert(romm::new_enough("5.3.0") && !romm::new_enough("5.2.9"));
    }

    /* The list: in pages, signed in by token or password, what is the game kept. */
    std::vector<remote::SourceGame> games;
    std::string error;
    auto token = source("{\"url\":\"" + url + "\",\"token\":\"rmm_test\"}");
    assert(token->address() == url);
    assert(token->list(&games, &error, never) && games.size() == 6);
    const remote::SourceGame *alpha = find(games, "10");
    assert(alpha && alpha->identified && alpha->ids.at("screenscraper") == "1000" &&
           !alpha->ids.count("igdb") && alpha->systems.front() == "snes" &&
           alpha->folder == "Alpha Quest (USA)" &&
           alpha->cover == "/assets/romm/resources/roms/2/10/cover/small.png?ts=1");
    const remote::SourceGame *beta = find(games, "11");
    assert(beta && beta->files.size() == 2 && beta->files[0].name == "Beta Racer.cue" &&
           beta->files[1].name == "Beta Racer (Track 1).bin" && beta->files[1].size == 5000000);
    const remote::SourceGame *delta = find(games, "13");
    assert(delta && delta->name == "Delta (Europe)" && !delta->identified);
    assert(find(games, "15")->files.empty()); /* a patch only */
    auto password =
        source("{\"url\":\"" + url + "\",\"username\":\"player\",\"password\":\"secret\"}");
    assert(password->list(&games, &error, never) && games.size() == 6);
    auto wrong = source("{\"url\":\"" + url + "\",\"token\":\"rmm_other\"}");
    assert(!wrong->list(&games, &error, never) &&
           error == "RomM did not accept the token or password in sources.json");
    auto psx = source("{\"url\":\"" + url + "\",\"token\":\"rmm_test\",\"platforms\":[\"PSX\"]}");
    assert(psx->list(&games, &error, never) && games.size() == 1 && games[0].id == "11");
    auto none = source("{\"url\":\"" + url + "\",\"token\":\"rmm_test\",\"platforms\":[\"n64\"]}");
    assert(!none->list(&games, &error, never) &&
           error == "The server has none of the platforms sources.json names");
    auto away = source("{\"url\":\"http://127.0.0.1:1\",\"token\":\"rmm_test\"}");
    assert(!away->list(&games, &error, never) && !error.empty());

    /* A cover. */
    std::string picture;
    assert(token->list(&games, &error, never));
    alpha = find(games, "10");
    assert(token->cover(*alpha, &picture, never) && picture == "\x89PNG alpha");

    /* A file: from its start, from a byte on, from a server that cannot go on, past its end. */
    {
        Collect all;
        assert(token->fetch(*alpha, alpha->files[0], 0, all, &error) && all.begun == 1);
        assert(all.bytes == pattern(100, 6000000));
        Collect part;
        assert(token->fetch(*alpha, alpha->files[0], 5000000, part, &error) && !part.from_start);
        assert(part.bytes == pattern(100, 6000000).substr(5000000));
        beta = find(games, "11");
        Collect whole;
        assert(token->fetch(*beta, beta->files[1], 1000, whole, &error) && whole.from_start);
        assert(whole.bytes == pattern(111, 5000000));
        Collect past;
        assert(token->fetch(*alpha, alpha->files[0], 7000000, past, &error) && past.from_start);
        assert(past.bytes == pattern(100, 6000000));
        Collect refused;
        assert(!wrong->fetch(*alpha, alpha->files[0], 0, refused, &error) &&
               error == "RomM did not accept the token or password in sources.json");
    }

    /* The download sources with it: two copies of a game on one server are two titles, a game
     * of no platform the title knows and one without a file of the game are not listed, a game in
     * a folder comes whole. */
    const remote::Paths paths = remote::Paths::at(root);
    files::make_folders(paths.config);
    files::write(paths.config + "/sources.json",
                 "{\"sources\":[{\"type\":\"romm\",\"name\":\"Home\",\"url\":\"" + url +
                     "\",\"token\":\"rmm_test\"}]}");
    remote::list_new(paths, 5);
    const std::vector<remote::Title> titles = remote::titles();
    assert(titles.size() == 4);
    assert(titles[0].key == "screenscraper:1000" && titles[3].key == "screenscraper:1000#2");
    remote::start(paths, {});
    assert(remote::enqueue("home", "11", true));
    for (int i = 0; i < 5000 && !remote::downloads().empty(); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    assert(remote::downloads().empty());
    const std::string folder = remote::placed_folder("home", "11");
    assert(folder == root + "/content/psx/Beta Racer");
    std::string track;
    assert(files::read(folder + "/Beta Racer (Track 1).bin", &track) &&
           track == pattern(111, 5000000));
    assert(files::size(folder + "/Beta Racer.cue") == 120 && !files::is_folder(folder + "/manual"));

    std::puts("romm: addresses, pages and categories, sign-ins, platforms, a cover, files from "
              "their start, a byte on, a server that cannot go on, past their end; a game "
              "downloaded through the download sources, firmware PASS");
    return 0;
}
