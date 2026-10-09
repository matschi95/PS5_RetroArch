/* The download sources (src/remote/), on the host, with a backend of this file's own (the
 * type "fake", in place of src/remote/backends.cpp): which game is which, two sources, the
 * kept lists and queue, downloads going on with a spoilt end, a server that cannot go on, a
 * cancel, a failure, the cleaning of .remote-downloads/, and the playlists, stubs and covers
 * the frontends read. argv[1] is a scratch folder.
 */
#include "../src/ps5_library.h"
#include "../src/remote/files.h"
#include "../src/remote/library.h"
#include "../src/remote/remote.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <thread>
#include <vector>

namespace remote = ps5::remote;
namespace files = ps5::remote::files;
using remote::Json;

namespace
{
/* A server of the fake backend: its games, their files' bytes and what it was asked. */
struct Server
{
    std::vector<remote::SourceGame> games;
    std::map<std::string, std::string> files;  /* file id -> bytes */
    std::map<std::string, std::string> covers; /* cover name -> picture */
    bool ignores_range = false;
    std::string list_error;        /* the list fails with this */
    std::string file_error;        /* a file fails with this */
    std::atomic<bool> hold{false}; /* a transfer waits after its first piece until stopped */
    std::atomic<int> lists{0};
    std::mutex lock;
    std::vector<uint64_t> offsets; /* each fetch's */
};
std::map<std::string, Server *> servers; /* by address */

class FakeSource final : public remote::Source
{
  public:
    FakeSource(std::string url, Server &server) : url_(std::move(url)), server_(server)
    {
    }
    std::string address() const override
    {
        return url_;
    }
    bool list(std::vector<remote::SourceGame> *games, std::string *error, const remote::Stopped &,
              unsigned) override
    {
        server_.lists++;
        if (!server_.list_error.empty())
        {
            *error = server_.list_error;
            return false;
        }
        *games = server_.games;
        return true;
    }
    bool cover(const remote::SourceGame &game, std::string *picture,
               const remote::Stopped &) override
    {
        const auto found = server_.covers.find(game.cover);
        if (found == server_.covers.end())
            return false;
        *picture = found->second;
        return true;
    }
    bool fetch(const remote::SourceGame &, const remote::SourceFile &file, uint64_t offset,
               remote::Receiver &receiver, std::string *error) override
    {
        {
            std::lock_guard<std::mutex> guard(server_.lock);
            server_.offsets.push_back(offset);
        }
        if (!server_.file_error.empty())
        {
            *error = server_.file_error;
            return false;
        }
        const std::string &bytes = server_.files.at(file.id);
        if (server_.ignores_range)
            offset = 0;
        if (!receiver.begin(server_.ignores_range))
            return false;
        for (size_t at = offset; at < bytes.size(); at += 1 << 20)
        {
            if (receiver.stopped() ||
                !receiver.take(bytes.data() + at, std::min<size_t>(1 << 20, bytes.size() - at)))
                return false;
            while (server_.hold && !receiver.stopped())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return !receiver.stopped();
    }

  private:
    std::string url_;
    Server &server_;
};
} // namespace

/* The test's backends: "fake", at a server of this file's. */
std::unique_ptr<remote::Source> ps5::remote::make_source(const std::string &type,
                                                         const Json &settings, std::string *error)
{
    const std::string url = settings["url"].str();
    if (type != "fake" || !servers.count(url))
    {
        *error = "Unknown source type \"" + type + "\"";
        return nullptr;
    }
    return std::unique_ptr<remote::Source>(new FakeSource(url, *servers[url]));
}

namespace
{
std::string root;
remote::Paths paths;

void write(const std::string &path, const std::string &text)
{
    assert(files::make_folders(files::parent(path)));
    assert(files::write(path, text));
}

std::string read(const std::string &path)
{
    std::string text;
    files::read(path, &text);
    return text;
}

Json read_json(const std::string &path)
{
    Json json;
    const bool parsed = Json::parse(read(path), &json);
    assert(parsed);
    return json;
}

void core(const std::string &name, const std::string &database, const std::string &extensions)
{
    write(paths.cores + "/" + name + "_libretro.so", "");
    write(paths.info + "/" + name + "_libretro.info",
          "display_name = \"" + name + "\"\ncorename = \"" + name + "\"\ndatabase = \"" + database +
              "\"\nsupported_extensions = \"" + extensions + "\"\n");
}

/* Bytes that tell their places apart. */
std::string data(size_t size, unsigned seed)
{
    std::string bytes(size, '\0');
    for (size_t i = 0; i < size; i++)
        bytes[i] = static_cast<char>((i * 131u + seed * 7u + (i >> 12)) & 0xff);
    return bytes;
}

remote::SourceGame game(const std::string &id, const std::string &name, const std::string &system,
                        const std::string &folder,
                        std::vector<std::pair<std::string, std::string>> parts, /* name, id */
                        Server &server, const std::string &crc = "", bool identified = true)
{
    remote::SourceGame g;
    g.id = id;
    g.name = name;
    g.systems = {system};
    g.folder = folder;
    g.identified = identified;
    for (const auto &part : parts)
        g.files.push_back({part.second, part.first, server.files[part.second].size(),
                           remote::FileKind::game, crc, "", ""});
    return g;
}

/* Waits until a condition holds, at most a few seconds. */
template <typename Condition> void until(Condition condition)
{
    for (int i = 0; i < 5000 && !condition(); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(condition());
}

bool listed(size_t sources)
{
    const remote::Status status = remote::current();
    size_t done = 0;
    for (const auto &source : status.sources)
        done += !source.refreshing && (source.online || !source.error.empty());
    return done == sources;
}

std::vector<remote::Placed> placed;
std::mutex placed_lock;

/* A writer as the console's FTP server is one: none answering, or one whose drive is full. */
class TestWriter final : public remote::Writer
{
  public:
    explicit TestWriter(bool answers) : answers_(answers)
    {
    }
    bool open(const std::string &path, uint64_t offset, std::string *error) override
    {
        if (!answers_)
        {
            *error = "Downloads need an FTP server running on the console; none answers";
            return false;
        }
        file_ = std::fopen(path.c_str(), files::size(path) >= 0 ? "r+b" : "wb");
        return file_ && ftruncate(fileno(file_), off_t(offset)) == 0 &&
               std::fseek(file_, long(offset), SEEK_SET) == 0;
    }
    bool write(const void *data, size_t size, std::string *error) override
    {
        if (written_ + size > (2u << 20))
        {
            *error = "The console's FTP server could not write the file: No space left on device";
            return false;
        }
        written_ += size;
        return std::fwrite(data, 1, size, file_) == size;
    }
    bool finish(std::string *) override
    {
        const bool closed = std::fclose(file_) == 0;
        file_ = nullptr;
        return closed;
    }
    ~TestWriter() override
    {
        if (file_)
            std::fclose(file_);
    }

  private:
    bool answers_;
    std::FILE *file_ = nullptr;
    uint64_t written_ = 0;
};
} // namespace

static void identity()
{
    using remote::normal_name;
    assert(normal_name("The Legend of Zelda: A Link to the Past (USA) [!]") ==
           "thelegendofzeldaalinktothepast");
    assert(normal_name("Pokémon: Let's Go! (Europe)") == "pokemonletsgo");
    assert(normal_name("Æon Œuvre Łódź") == "aeonoeuvrelodz");
    assert(normal_name("ドラゴンクエスト V") == "ドラゴンクエストv");
    assert(normal_name("Mario™ ® 64") == "mario64");
    assert(remote::plain_text("Pokémon Café ドラ") == "Pokemon Cafe ??");
    assert(remote::normal_crc("B19ED489|crc") == "b19ed489" &&
           remote::normal_crc("0xB19ED489") == "b19ed489");
    assert(remote::normal_crc("DETECT").empty() && remote::normal_crc("00000000|crc").empty());

    remote::Game a, b;
    a.platform = b.platform = "snes";
    a.name = b.name = "Super Mario World";
    a.normal_name = b.normal_name = normal_name(a.name);
    a.file = "Super Mario World (USA).sfc";
    b.file = "smw.sfc";
    assert(!remote::same_game(a, b)); /* names known from the files only, files differ */
    a.identified = b.identified = true;
    assert(remote::same_game(a, b)); /* names from metadata */
    a.ids["igdb"] = "1070";
    b.ids["igdb"] = "1071";
    assert(!remote::same_game(a, b)); /* an id at the same provider decides */
    b.ids["igdb"] = "1070";
    b.name = "Something Else";
    b.normal_name = normal_name(b.name);
    assert(remote::same_game(a, b));
    a.crc32 = "b19ed489";
    b.crc32 = "11111111";
    assert(!remote::same_game(a, b)); /* checksums decide before ids */
    b.crc32 = "b19ed489";
    b.platform = "nes";
    assert(!remote::same_game(a, b)); /* never across platforms */
    assert(remote::title_key(a) == "crc:b19ed489");

    assert(remote::same_as_local(a, "", "", "/c/SNES/Super Mario World (USA).sfc")); /* its file */
    assert(remote::same_as_local(a, "B19ED489|crc", "", "/c/x.sfc"));
    assert(!remote::same_as_local(a, "11111111|crc", normal_name("Super Mario World"),
                                  "/c/x.sfc")); /* another dump */
    a.crc32.clear();
    assert(remote::same_as_local(a, "", normal_name("Super Mario World (Europe)"), "/c/x.sfc"));
    a.identified = false;
    assert(!remote::same_as_local(a, "", normal_name("Super Mario World (Europe)"), "/c/x.sfc"));

    assert(remote::launch_file({"b.chd", "a.chd", "big.bin"}, {1, 1, 9}) == "a.chd");
    assert(remote::launch_file({"small.bin", "big.iso"}, {1, 9}) == "big.iso");
    assert(remote::launch_file({"x.cue", "x.m3u"}, {1, 1}) == "x.m3u");

    Server server;
    server.files["1"] = "x";
    remote::Game made;
    remote::SourceGame from = game("7", "Crash", "Sony - PlayStation", "Crash Bandicoot (USA)",
                                   {{"../../evil/Crash.cue", "1"}}, server);
    assert(remote::as_game("s", from, &made) && made.platform == "psx" &&
           made.parts.front().name == "evil/Crash.cue" && made.file == "evil/Crash.cue");
    from.systems = {"No Such Console"};
    assert(!remote::as_game("s", from, &made)); /* a platform the console does not know */
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    root = argv[1];
    paths = remote::Paths::at(root);
    identity();

    /* The title: cores for SNES and PlayStation, and the remote core. */
    core("snes9x", "Nintendo - Super Nintendo Entertainment System", "smc|sfc");
    core("mednafen_psx_hw", "Sony - PlayStation", "cue|chd|m3u");
    core("remote", "", "remote");
    const std::string remote_core = paths.cores + "/" PS5_LIBRARY_FETCH_CORE;
    /* A game here, in the player's own SNES playlist (and its folder, SNES), with a field of
     * RetroArch's this port does not read. */
    const std::string zelda_here = paths.content + "/SNES/Zelda (USA).sfc";
    write(zelda_here, "zelda");
    const std::string snes_playlist =
        paths.playlists + "/Nintendo - Super Nintendo Entertainment System.lpl";
    write(snes_playlist,
          "{\n  \"version\": \"1.5\",\n  \"scan_content_dir\": \"/app0/content/SNES\",\n"
          "  \"items\": [\n    {\n      \"path\": \"" +
              zelda_here + "\",\n      \"label\": \"Zelda\",\n      \"core_path\": \"" +
              paths.cores + "/snes9x_libretro.so\",\n      \"entry_slot\": 3\n    }\n  ]\n}\n");

    /* Two servers. */
    Server home, office;
    const std::string smw = data(3000, 1), chrono = data(9u << 20, 2), ff7_bin = data(5000, 3);
    home.files = {{"11", smw},
                  {"21", "zelda"},
                  {"31", "FILE \"FF7.bin\" BINARY\n"},
                  {"32", ff7_bin},
                  {"41", "n64"}};
    home.covers = {{"/smw.png", "\x89PNG smw"}};
    home.games = {
        game("1", "Super Mario World", "snes", "Super Mario World (USA)",
             {{"Super Mario World (USA).sfc", "11"}}, home),
        game("2", "The Legend of Zelda", "snes", "Zelda (USA)", {{"Zelda (USA).sfc", "21"}}, home),
        game("3", "Final Fantasy VII", "psx", "Final Fantasy VII",
             {{"FF7.cue", "31"}, {"FF7.bin", "32"}}, home),
        game("4", "Mario 64", "n64", "Mario 64", {{"Mario 64.z64", "41"}}, home),
    };
    home.games[0].ids["igdb"] = "1070";
    home.games[0].cover = "/smw.png";
    office.files = {{"700", smw}, {"710", chrono}};
    office.covers = {{"/ct.jpg", "\xff\xd8 chrono"}};
    office.games = {
        game("70", "Super Mario World", "snes", "SMW", {{"SMW.sfc", "700"}}, office),
        game("71", "Chrono Trigger", "snes", "Chrono Trigger (USA)",
             {{"Chrono Trigger (USA).sfc", "710"}}, office),
    };
    office.games[0].ids["igdb"] = "1070";
    office.games[1].cover = "/ct.jpg";
    servers["http://home"] = &home;
    servers["http://office"] = &office;

    /* Nothing set up: nothing written, nothing listed. */
    remote::sync(paths);
    assert(files::size(paths.config + "/written.json") < 0);

    write(paths.config + "/sources.json",
          "{\"sources\":[{\"type\":\"fake\",\"name\":\"Home\",\"url\":\"http://home\"},"
          "{\"type\":\"fake\",\"name\":\"Office\",\"url\":\"http://office\"},"
          "{\"type\":\"ftp\",\"name\":\"Home\"}]}");

    /* As the title starts: new sources listed at once. */
    remote::list_new(paths, 5);
    assert(home.lists == 1 && office.lists == 1);
    remote::list_new(paths, 5); /* listed already: not at the next start */
    assert(home.lists == 1);
    remote::Status status = remote::current();
    assert(status.configured && status.sources.size() == 3);
    assert(status.sources[0].key == "home" && status.sources[2].key == "home%20%282%29" &&
           status.sources[2].name == "Home (2)");
    assert(status.sources[2].error == "Unknown source type \"ftp\"");
    assert(status.sources[0].games == 4); /* the N64 game is listed; the playlists leave it out */

    /* The titles: a game once however many sources have it (SMW by its IGDB id). */
    std::vector<remote::Title> titles = remote::titles();
    assert(titles.size() == 5);
    assert(titles[0].games.size() == 2 && titles[0].games[1].source == "office");

    /* The playlists: none for what is here, none of a platform without a core. */
    remote::sync(paths);
    const std::string snes_remote =
        paths.playlists + "/Nintendo - Super Nintendo Entertainment System (Remote).lpl";
    const std::string psx_remote = paths.playlists + "/Sony - PlayStation (Remote).lpl";
    Json playlist = read_json(snes_remote);
    assert(playlist["items"].items.size() == 2);
    assert(playlist["items"].items[0]["label"].str() == "Chrono Trigger [Remote]" &&
           playlist["items"].items[1]["label"].str() == "Super Mario World [Remote]");
    const Json &smw_entry = playlist["items"].items[1];
    assert(smw_entry["core_path"].str() == remote_core &&
           smw_entry["db_name"].str() == "Nintendo - Super Nintendo Entertainment System.lpl");
    const std::string smw_stub = paths.content + "/SNES/.remote/Super Mario World.remote";
    assert(smw_entry["path"].str() == smw_stub);
    remote::Stub stub;
    assert(remote::read_stub(smw_stub, &stub) && stub.platform == "snes" &&
           stub.core == paths.cores + "/snes9x_libretro.so" && stub.games.size() == 2 &&
           stub.games[0].first == "home" && stub.games[1].first == "office");
    assert(read_json(psx_remote)["items"].items.size() == 1);
    assert(files::size(paths.playlists + "/Remote.lpl") > 0);
    remote::Stub screen;
    assert(remote::read_stub(paths.config + "/screens/Downloads.remote", &screen) &&
           screen.screen == "downloads");
    {
        /* The library frontends read: the stubs in their systems, whose core stays the one
         * that runs the games, and the remote screens as their own. */
        struct ps5_library library;
        assert(ps5_library_load(&library, paths.playlists.c_str(), paths.info.c_str(),
                                paths.cores.c_str()) == 0);
        bool snes = false;
        for (size_t s = 0; s < library.system_count; s++)
            if (std::string(library.systems[s].id) == "snes")
            {
                snes = true;
                assert(library.systems[s].game_count == 3);
                assert(std::string(library.systems[s].core) == paths.cores + "/snes9x_libretro.so");
                assert(std::string(library.systems[s].folder) == paths.content + "/SNES");
            }
        assert(snes);
        ps5_library_free(&library);
    }

    /* Lists and covers in the background, while the menu is up. */
    remote::start(paths,
                  [](const remote::Placed &p)
                  {
                      std::lock_guard<std::mutex> guard(placed_lock);
                      placed.push_back(p);
                      remote::note_placed(paths, p);
                  });
    until([] { return listed(3); });
    until(
        []
        {
            remote::Game g;
            return remote::find("office", "71", &g) && !g.cover.empty();
        });
    remote::Game chrono_game;
    assert(remote::find("office", "71", &chrono_game) &&
           chrono_game.cover.substr(chrono_game.cover.size() - 4) == ".jpg" &&
           read(chrono_game.cover) == "\xff\xd8 chrono");

    /* A download: from the source asked for, into the system's folder, the playlist's at the
     * next start. */
    assert(remote::enqueue("home", "1", true));
    assert(!remote::enqueue("office", "70", false)); /* the same game is coming already */
    until([] { return remote::downloads().empty(); });
    const std::string smw_folder = remote::placed_folder("home", "1");
    assert(smw_folder == paths.content + "/SNES/Super Mario World (USA)");
    assert(read(smw_folder + "/Super Mario World (USA).sfc") == smw);
    assert(!files::is_folder(paths.downloads));
    assert(placed.size() == 1 && placed[0].launch == smw_folder + "/Super Mario World (USA).sfc");

    /* A game in a folder: its files as on the source, the .cue run; the system has no folder
     * of its own yet. */
    assert(remote::enqueue("home", "3", false));
    until([] { return remote::downloads().empty(); });
    assert(remote::placed_folder("home", "3") == paths.content + "/psx/Final Fantasy VII");
    assert(read(paths.content + "/psx/Final Fantasy VII/FF7.bin") == ff7_bin);

    /* At the next start: into the player's own playlist, the rest of it kept, and listed on
     * the sources no more; the cover with it. */
    remote::sync(paths);
    const std::string own = read(snes_playlist);
    assert(own.find("\"scan_content_dir\": \"/app0/content/SNES\"") != std::string::npos &&
           own.find("\"entry_slot\": 3") != std::string::npos);
    playlist = read_json(snes_playlist);
    assert(playlist["items"].items.size() == 2 &&
           playlist["items"].items[1]["label"].str() == "Super Mario World" &&
           playlist["items"].items[1]["core_path"].str() == paths.cores + "/snes9x_libretro.so");
    assert(files::size(paths.media + "/snes/covers/Super Mario World (USA).png") > 0);
    assert(read_json(snes_remote)["items"].items.size() == 1 && files::size(smw_stub) < 0);
    playlist = read_json(paths.playlists + "/Sony - PlayStation.lpl");
    assert(playlist["items"].items.size() == 1 &&
           playlist["items"].items[0]["path"].str() ==
               paths.content + "/psx/Final Fantasy VII/FF7.cue");
    assert(files::size(psx_remote) < 0);
    /* The remaining title's cover, under its stub's name. */
    assert(read(paths.media + "/snes/covers/Chrono Trigger.jpg") == "\xff\xd8 chrono");

    /* An interrupted download goes on where it was, the last 4 MiB fetched again. */
    const std::string staged = paths.downloads + "/office/71/Chrono Trigger (USA).sfc";
    std::string spoilt = chrono.substr(0, 7u << 20);
    for (size_t i = (6u << 20); i < spoilt.size(); i++)
        spoilt[i] = 'x'; /* the end was never written */
    write(staged, spoilt);
    assert(remote::enqueue("office", "71", false));
    until([] { return remote::downloads().empty(); });
    assert(office.offsets.back() == (3u << 20));
    std::string chrono_folder = remote::placed_folder("office", "71");
    assert(read(chrono_folder + "/Chrono Trigger (USA).sfc") == chrono);
    files::remove_tree(chrono_folder);

    /* ...also from a server that cannot go on: from the start. A folder of the game's name
     * that is there is the player's: left alone. */
    write(paths.content + "/SNES/Chrono Trigger (USA)/mine.txt", "mine");
    write(staged, spoilt);
    office.ignores_range = true;
    assert(remote::enqueue("office", "71", false));
    until([] { return remote::downloads().empty(); });
    office.ignores_range = false;
    chrono_folder = remote::placed_folder("office", "71");
    assert(chrono_folder == paths.content + "/SNES/Chrono Trigger (USA) (2)");
    assert(read(chrono_folder + "/Chrono Trigger (USA).sfc") == chrono);
    assert(files::names(paths.content + "/SNES/Chrono Trigger (USA)").size() == 1);
    files::remove_tree(chrono_folder);
    files::remove_tree(paths.config + "/placed");

    /* A failed download stays, with why; a cancelled one is removed. */
    office.file_error = "The server answered with status 500";
    assert(remote::enqueue("office", "71", false));
    until(
        []
        {
            const auto list = remote::downloads();
            return !list.empty() && list[0].state == remote::State::failed;
        });
    assert(remote::downloads()[0].error == "The server answered with status 500");
    office.file_error.clear();
    office.hold = true;
    assert(remote::enqueue("office", "71", false)); /* tried again */
    until(
        []
        {
            const auto list = remote::downloads();
            return !list.empty() && list[0].state == remote::State::downloading &&
                   list[0].done >= (1u << 20);
        });
    assert(remote::cancel("office", "71"));
    office.hold = false;
    until([] { return remote::downloads().empty() && !files::is_folder(paths.downloads); });
    assert(remote::placed_folder("office", "71").empty());

    /* Written by the console's FTP server: none answering fails it, saying so; a full drive
     * fails it with what the server said, what was written kept to go on from. */
    {
        remote::Paths ftp = paths;
        ftp.writer = [] { return std::unique_ptr<remote::Writer>(new TestWriter(false)); };
        remote::start(ftp, {});
        assert(remote::enqueue("office", "71", false));
        until(
            []
            {
                const auto list = remote::downloads();
                return !list.empty() && list[0].state == remote::State::failed;
            });
        assert(remote::downloads()[0].error ==
               "Downloads need an FTP server running on the console; none answers");
        ftp.writer = [] { return std::unique_ptr<remote::Writer>(new TestWriter(true)); };
        remote::start(ftp, {});
        assert(remote::enqueue("office", "71", false));
        until(
            []
            {
                const auto list = remote::downloads();
                return !list.empty() && list[0].state == remote::State::failed;
            });
        assert(remote::downloads()[0].error ==
               "The console's FTP server could not write the file: No space left on device");
        assert(files::size(staged) == (2u << 20));
        assert(remote::cancel("office", "71"));
        until([] { return !files::is_folder(paths.downloads); });
        remote::start(paths, {});
    }

    /* Stopped while a game runs: what it has stays, and it goes on afterwards. */
    office.hold = true;
    assert(remote::enqueue("office", "71", false));
    until([] { return !remote::downloads().empty() && remote::downloads()[0].done > 0; });
    remote::stop();
    assert(remote::downloads()[0].state == remote::State::queued && files::size(staged) > 0);
    office.hold = false;
    /* The kept queue. */
    assert(read_json(paths.config + "/queue.json").items.size() == 1);
    remote::start(paths, [](const remote::Placed &p) { remote::note_placed(paths, p); });
    until([] { return remote::downloads().empty(); });
    assert(read(remote::placed_folder("office", "71") + "/Chrono Trigger (USA).sfc") == chrono);

    /* Leftovers of a game no longer queued go when the menu is up again. */
    write(paths.downloads + "/home/99/x.bin", "left");
    remote::start(paths, {});
    assert(!files::is_folder(paths.downloads + "/home/99"));

    /* Deleted from the console: out of its playlist at the next start, on its sources again. */
    remote::sync(paths);
    assert(read_json(snes_playlist)["items"].items.size() == 3);
    const std::string deleted = remote::placed_folder("office", "71");
    files::remove_tree(deleted);
    remote::note_removed(paths, "snes", deleted + "/Chrono Trigger (USA).sfc");
    remote::sync(paths);
    assert(read_json(snes_playlist)["items"].items.size() == 2);
    assert(read_json(snes_remote)["items"].items.size() == 1);

    /* A source taken out: its games go, and what was written for it. */
    write(paths.config + "/sources.json",
          "{\"marker\":\" (server)\",\"sources\":[{\"type\":\"fake\",\"name\":\"Home\",\"url\":"
          "\"http://home\"}]}");
    remote::sync(paths);
    assert(files::size(snes_remote) < 0);
    /* None set up: everything written is taken back. */
    std::remove((paths.config + "/sources.json").c_str());
    remote::sync(paths);
    assert(read_json(paths.config + "/written.json")["files"].items.empty());
    assert(files::size(paths.playlists + "/Remote.lpl") < 0);

    std::puts(
        "remote: identity, lists, titles, playlists and stubs, covers, downloads with "
        "resume, a server that cannot go on, failure, cancel, stop, leftovers, deleting PASS");
    return 0;
}
