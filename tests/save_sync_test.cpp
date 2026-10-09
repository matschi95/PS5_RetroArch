/* The save sync (src/remote/save_sync.h, save_config.h), on the host, with a store of the
 * test's own: which side changed, conflicts and the player's choice, save states the store or
 * the console has alone, a store that keeps no hash, backups, a game the store does not have;
 * save-sync.json; RomM's times and versions (src/remote/romm/romm_saves.h, romm_client.h).
 */
#include "../src/remote/backends.h"
#include "../src/remote/files.h"
#include "../src/remote/romm/romm_client.h"
#include "../src/remote/romm/romm_saves.h"
#include "../src/remote/save_jobs.h"
#include "../src/remote/save_config.h"
#include "../src/remote/save_sync.h"

#include <cassert>
#include <cstdio>
#include <map>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace remote = ps5::remote;
namespace files = ps5::remote::files;
using ps5_scraper::Json;

namespace
{
struct Copy
{
    std::string data;
    int version = 0;
};

/* The server: its files by kind, emulator and name; uploads and downloads counted. */
struct Server
{
    std::map<std::string, Copy> copies;
    bool states_hashed = false; /* RomM keeps no hash of a state */
    bool refuse_newer = false;  /* an upload without overwrite is refused (409) */
    bool down = false;          /* it does not answer */
    int uploads = 0, downloads = 0, versions = 0;
    std::string last_user = "alex";

    static std::string key(remote::SaveKind kind, const std::string &emulator,
                           const std::string &name)
    {
        return std::string(kind == remote::SaveKind::save ? "save/" : "state/") + emulator + "/" +
               name;
    }
} server;

std::string md5_of(const std::string &data)
{
    const std::string path = "/tmp/ps5-save-sync-md5";
    files::write(path, data);
    std::string hash;
    remote::md5_file(path, &hash);
    std::remove(path.c_str());
    return hash;
}

class FakeStore final : public remote::SaveStore
{
  public:
    std::string address() const override
    {
        return "http://fake";
    }
    bool prepare(const remote::Stopped &, std::string *error) override
    {
        if (server.down)
            *error = "The server did not answer";
        return !server.down;
    }
    std::string user() const override
    {
        return server.last_user;
    }
    bool games(std::vector<remote::SourceGame> *games, const remote::Stopped &,
               std::string *) override
    {
        remote::SourceGame game;
        game.id = "42";
        game.name = "Chrono Trigger";
        game.systems = {"snes"};
        game.folder = "Chrono Trigger (USA)";
        game.identified = true;
        game.files.push_back(
            {"420", "Chrono Trigger (USA).sfc", 100, remote::FileKind::game, "deadbeef", "", ""});
        games->assign(1, game);
        return true;
    }
    bool names(const remote::SourceGame &, remote::SaveKind kind, const std::string &emulator,
               std::vector<std::string> *names, std::string *) override
    {
        const std::string prefix = Server::key(kind, emulator, "");
        for (const auto &copy : server.copies)
            if (copy.first.rfind(prefix, 0) == 0)
                names->push_back(copy.first.substr(prefix.size()));
        return true;
    }
    bool compare(const remote::SourceGame &, const remote::LocalSave &local, remote::SavePlan *plan,
                 std::string *) override
    {
        *plan = remote::SavePlan{};
        const auto found = server.copies.find(Server::key(local.kind, local.emulator, local.name));
        if (found == server.copies.end())
        {
            plan->action = local.present ? remote::SaveAction::upload : remote::SaveAction::none;
            return true;
        }
        plan->remote = found->first;
        plan->version = "v" + std::to_string(found->second.version);
        if (local.kind == remote::SaveKind::save || server.states_hashed)
            plan->hash = md5_of(found->second.data);
        plan->action = !local.present ? remote::SaveAction::download
                       : !plan->hash.empty() && plan->hash == local.hash
                           ? remote::SaveAction::none
                           : remote::SaveAction::conflict;
        return true;
    }
    bool upload(const remote::SourceGame &, const remote::LocalSave &local, const std::string &path,
                bool overwrite, bool *newer, std::string *version, std::string *error) override
    {
        if (server.refuse_newer && !overwrite)
        {
            *newer = true;
            *error = "newer";
            return false;
        }
        Copy &copy = server.copies[Server::key(local.kind, local.emulator, local.name)];
        assert(files::read(path, &copy.data));
        copy.version = ++server.versions;
        *version = "v" + std::to_string(copy.version);
        server.uploads++;
        return true;
    }
    bool download(const remote::SourceGame &, const remote::LocalSave &,
                  const remote::SavePlan &plan, const std::string &path, std::string *) override
    {
        server.downloads++;
        return files::write(path, server.copies[plan.remote].data);
    }
    void finish(bool) override
    {
    }
};

std::string root;
remote::SyncGame game;
remote::SyncPlaces places;
remote::SyncChoice choice = remote::SyncChoice::neither;
int asked = 0;

remote::SyncResult sync_now()
{
    return remote::sync_save_data("fake", Json::record(), game, places,
                                  [](const remote::SavePlan &, const remote::LocalSave &)
                                  {
                                      asked++;
                                      return choice;
                                  },
                                  {});
}

void write(const std::string &path, const std::string &data)
{
    assert(files::make_folders(files::parent(path)) && files::write(path, data));
}

std::string read(const std::string &path)
{
    std::string text;
    files::read(path, &text);
    return text;
}

void put(remote::SaveKind kind, const std::string &name, const std::string &data)
{
    Copy &copy = server.copies[Server::key(kind, "snes9x", name)];
    copy.data = data;
    copy.version = ++server.versions;
}
} // namespace

/* The test's backends (in place of src/remote/backends.cpp): a store, no download source. */
std::unique_ptr<remote::Source> ps5::remote::make_source(const std::string &, const Json &,
                                                         std::string *error)
{
    *error = "none";
    return nullptr;
}

/* No firmware either (in place of src/remote/backends.cpp). */
std::unique_ptr<remote::FirmwareSource>
ps5::remote::make_firmware_source(const std::string &, const Json &, std::string *)
{
    return nullptr;
}

/* The test's store (in place of src/remote/backends.cpp). */
std::unique_ptr<remote::SaveStore> ps5::remote::make_save_store(const std::string &type,
                                                                const Json &, const std::string &,
                                                                std::string *error)
{
    if (type != "fake")
    {
        *error = "Unknown server type \"" + type + "\" in save-sync.json";
        return nullptr;
    }
    return std::unique_ptr<remote::SaveStore>(new FakeStore);
}

int main(int argc, char **argv)
{
    assert(argc > 1);
    root = argv[1];

    /* The parts. */
    assert(remote::emulator_of("/app0/cores/snes9x_libretro.so") == "snes9x" &&
           remote::emulator_of("mgba_libretro.so") == "mgba");
    assert(remote::is_state_of("Game.state", "Game.state") &&
           remote::is_state_of("Game.state12", "Game.state") &&
           remote::is_state_of("Game.state.auto", "Game.state") &&
           !remote::is_state_of("Game.state.png", "Game.state") &&
           !remote::is_state_of("Game 2.state", "Game.state"));
    assert(md5_of("abc") == "900150983cd24fb0d6963f7d28e17f72");
    assert(remote::romm::parse_time("2026-10-08T12:34:56.123456+00:00") == 1791462896 &&
           remote::romm::parse_time("2026-10-08T14:34:56+02:00") == 1791462896 &&
           remote::romm::parse_time("2026-10-08T12:34:56Z") == 1791462896 &&
           remote::romm::parse_time("2026-10-08T12:34:56") == 1791462896 &&
           remote::romm::parse_time("") == 0);
    assert(remote::romm::format_time(1791462896) == "2026-10-08T12:34:56Z");
    assert(remote::romm::new_enough("5.3.0") && remote::romm::new_enough("5.3.1") &&
           remote::romm::new_enough("5.4.0-alpha.1") && remote::romm::new_enough("development") &&
           !remote::romm::new_enough("5.2.0") && !remote::romm::new_enough("4.9.0"));

    /* save-sync.json: made with an empty server; the player's fields and order stay; a file
     * that is no JSON is left alone; paired, the sign-in typed in before goes. */
    const std::string config = root + "/config/remote/save-sync.json";
    assert(remote::prepare_save_config(config));
    remote::SaveConfig read_back = remote::read_save_config(config);
    assert(read_back.readable && read_back.automatic && read_back.states && read_back.type.empty());
    write(config, "{\"type\":\"romm\",\"mine\":1,\"username\":\"a\",\"password\":\"b\","
                  "\"auto\":false}");
    assert(remote::prepare_save_config(config));
    read_back = remote::read_save_config(config);
    assert(read_back.type == "romm" && !read_back.automatic && read_back.settings.has("mine") &&
           read_back.settings.order[0] == "type" && !read_back.settings.has("token"));
    Json paired = Json::record();
    paired.set("type", Json::of("romm"));
    paired.set("url", Json::of("http://nas:3000"));
    paired.set("token", Json::of("rmm_paired"));
    assert(remote::set_save_server(config, paired));
    read_back = remote::read_save_config(config);
    assert(read_back.settings["token"].str() == "rmm_paired" &&
           !read_back.settings.has("username") && !read_back.settings.has("password") &&
           read_back.settings.has("mine"));
    assert(remote::set_save_server(config, Json::record()));
    assert(remote::read_save_config(config).type.empty());
    write(config, "not json");
    assert(!remote::prepare_save_config(config) && read(config) == "not json");
    assert(!remote::read_save_config(config).readable);

    /* Stores by type. */
    std::string error;
    assert(!remote::make_save_store("nope", Json::record(), root, &error) &&
           error == "Unknown server type \"nope\" in save-sync.json");

    /* The game as RetroArch loads it. */
    game.platform = "snes";
    game.name = "Chrono Trigger";
    game.content = root + "/content/SNES/Chrono Trigger (USA).sfc";
    game.crc32 = "DEADBEEF|crc";
    game.emulator = "snes9x";
    game.save = root + "/saves/snes9x/Chrono Trigger (USA).srm";
    game.state = root + "/states/snes9x/Chrono Trigger (USA).state";
    places.store = root + "/config/remote/saves";
    places.work = root + "/config/remote/saves/work";
    places.backups = root + "/config/remote/saves/backups";

    /* The console's save, none on the server: up it goes. Nothing changed: nothing goes. */
    write(game.save, "save 1");
    remote::SyncResult result = sync_now();
    assert(result.outcome == remote::SyncOutcome::uploaded && result.user == "alex");
    assert(server.copies["save/snes9x/Chrono Trigger (USA).srm"].data == "save 1");
    assert(sync_now().outcome == remote::SyncOutcome::same && server.uploads == 1);

    /* Played here: the console's goes up, over the copy it had. */
    write(game.save, "save 2");
    server.refuse_newer = true; /* only an upload without overwrite is refused */
    assert(sync_now().outcome == remote::SyncOutcome::uploaded && server.uploads == 2);
    server.refuse_newer = false;

    /* Played elsewhere: the server's comes, the console's to the backups. */
    put(remote::SaveKind::save, "Chrono Trigger (USA).srm", "save 3");
    assert(sync_now().outcome == remote::SyncOutcome::downloaded && read(game.save) == "save 3");
    const std::string backups = places.backups + "/snes9x/Chrono Trigger (USA).srm";
    assert(files::names(backups).size() == 1 &&
           read(backups + "/" + files::names(backups)[0]) == "save 2");
    assert(sync_now().outcome == remote::SyncOutcome::same && asked == 0);

    /* Both changed: the player chooses. Left as it is, nothing goes and it is asked again. */
    write(game.save, "save 4 here");
    put(remote::SaveKind::save, "Chrono Trigger (USA).srm", "save 4 there");
    assert(sync_now().outcome == remote::SyncOutcome::kept && asked == 1);
    assert(sync_now().outcome == remote::SyncOutcome::kept && asked == 2);
    choice = remote::SyncChoice::console;
    assert(sync_now().outcome == remote::SyncOutcome::uploaded &&
           server.copies["save/snes9x/Chrono Trigger (USA).srm"].data == "save 4 here");
    write(game.save, "save 5 here");
    put(remote::SaveKind::save, "Chrono Trigger (USA).srm", "save 5 there");
    choice = remote::SyncChoice::server;
    assert(sync_now().outcome == remote::SyncOutcome::downloaded &&
           read(game.save) == "save 5 there");
    assert(asked == 4);

    /* No record of it (another server): the same contents are in step, different ones are a
     * conflict, not replaced unasked. */
    files::remove_tree(places.store + "/synced.json");
    assert(sync_now().outcome == remote::SyncOutcome::same && asked == 4);
    files::remove_tree(places.store + "/synced.json");
    write(game.save, "save 6 here");
    choice = remote::SyncChoice::neither;
    assert(sync_now().outcome == remote::SyncOutcome::kept && asked == 5 &&
           read(game.save) == "save 6 here");

    /* Backups: the last three of the file. */
    for (int n = 0; n < 4; n++)
    {
        files::remove_tree(places.store + "/synced.json");
        write(game.save, "local " + std::to_string(n));
        put(remote::SaveKind::save, "Chrono Trigger (USA).srm", "remote " + std::to_string(n));
        choice = remote::SyncChoice::server;
        assert(sync_now().outcome == remote::SyncOutcome::downloaded);
    }
    assert(files::names(backups).size() == 3);

    /* Save states: one the console has alone goes up, one the server has alone comes; one of
     * each without a hash on the server and without a record is fetched once to compare. */
    write(game.state + "1", "state one");
    put(remote::SaveKind::state, "Chrono Trigger (USA).state2", "state two");
    put(remote::SaveKind::state, "Chrono Trigger (USA).state.auto", "auto");
    put(remote::SaveKind::state, "Other Game.state", "not this game's");
    write(game.state + ".auto", "auto");
    write(game.state + ".png", "a thumbnail");
    const int downloads = server.downloads;
    result = sync_now();
    assert(result.outcome == remote::SyncOutcome::downloaded);
    assert(server.copies["state/snes9x/Chrono Trigger (USA).state1"].data == "state one");
    assert(read(game.state + "2") == "state two" && read(game.state + ".auto") == "auto");
    assert(server.downloads == downloads + 2); /* the auto state to compare, the second slot */
    assert(!server.copies.count("state/snes9x/Chrono Trigger (USA).state.png"));
    assert(files::size(files::parent(game.state) + "/Other Game.state") < 0);
    assert(sync_now().outcome == remote::SyncOutcome::same && server.downloads == downloads + 2);
    /* ...not at all when the settings say so. */
    places.states = false;
    write(game.state + "1", "state one changed");
    result = sync_now();
    assert(result.outcome == remote::SyncOutcome::same && result.files.size() == 1);
    places.states = true;

    /* A game the server does not have. */
    game.crc32 = "11111111";
    game.name = "Secret of Mana";
    game.content = root + "/content/SNES/Secret of Mana.sfc";
    assert(sync_now().outcome == remote::SyncOutcome::no_game);
    /* ...nor of another platform. */
    game.crc32 = "deadbeef";
    game.platform = "nes";
    assert(sync_now().outcome == remote::SyncOutcome::no_game);

    /* When it runs (save_jobs.h): before a game from RetroArch's menu, without asking; not
     * for a game the remote core synced just now; after a game, waiting in pending.json until
     * it worked; not at all with "auto" off. */
    {
        game.platform = "snes";
        game.name = "Chrono Trigger";
        game.content = root + "/content/SNES/Chrono Trigger (USA).sfc";
        game.crc32 = "deadbeef";
        const std::string folder = root + "/jobs/config/remote";
        write(folder + "/save-sync.json", "{\"type\":\"fake\",\"states\":false}");
        remote::SaveJobs jobs(folder);
        std::vector<std::string> notices;
        jobs.on_notice([&](const std::string &text) { notices.push_back(text); });
        assert(jobs.wanted());
        remote::SyncGame known;
        assert(!jobs.known(game.content, &known));
        put(remote::SaveKind::save, "Chrono Trigger (USA).srm", "from the server");
        files::remove_tree(game.save);
        remote::SyncResult before = jobs.sync_before(game, 10);
        assert(before.outcome == remote::SyncOutcome::downloaded &&
               read(game.save) == "from the server");
        assert(!notices.empty() && notices.back() == "Chrono Trigger: save data from the server");
        assert(jobs.known(game.content, &known) && known.save == game.save &&
               known.emulator == "snes9x");
        /* Both changed: left as it is, unasked, and said. */
        write(game.save, "played here");
        put(remote::SaveKind::save, "Chrono Trigger (USA).srm", "played there");
        before = jobs.sync_before(game, 10);
        assert(before.outcome == remote::SyncOutcome::kept && read(game.save) == "played here");
        assert(notices.back().find("left as it is") != std::string::npos);
        /* Synced by the remote core, asking: the hook leaves it alone, once. */
        const int uploads = server.uploads;
        jobs.synced_launch(game.content);
        assert(jobs.sync_before(game, 10).outcome == remote::SyncOutcome::same &&
               files::size(folder + "/save-sync/launched") < 0);
        /* After the game: up on the thread; with the server not answering, it waits for later. */
        server.down = true;
        jobs.played(game);
        jobs.wait();
        assert(jobs.pending().size() == 1 && jobs.view().stage == remote::SyncView::failed);
        jobs.end();
        server.down = false;
        jobs.resume_pending();
        jobs.wait();
        /* Both changed: after a game nobody is asked; left as it is, done with. */
        assert(jobs.view().outcome == remote::SyncOutcome::kept && jobs.pending().empty() &&
               read(game.save) == "played here");
        /* A conflict on the thread, the player asked. */
        write(game.save, "played here again");
        std::thread chooser(
            [&]
            {
                while (jobs.view().stage != remote::SyncView::conflict)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                assert(jobs.view().file == "Chrono Trigger (USA).srm");
                jobs.choose(remote::SyncChoice::console);
            });
        assert(jobs.start({game}, true));
        chooser.join();
        jobs.wait();
        assert(jobs.view().outcome == remote::SyncOutcome::uploaded && server.uploads > uploads &&
               server.copies["save/snes9x/Chrono Trigger (USA).srm"].data == "played here again");
        /* "auto" off: nothing at all. */
        write(folder + "/save-sync.json", "{\"type\":\"fake\",\"states\":false,\"auto\":false}");
        assert(!jobs.wanted());
        write(game.save, "off");
        assert(jobs.sync_before(game, 10).outcome == remote::SyncOutcome::same);
        jobs.played(game);
        assert(jobs.pending().empty());
        /* pending.json as it is written. */
        const remote::SyncGame back = remote::SaveJobs::from_json(remote::SaveJobs::to_json(game));
        assert(back.content == game.content && back.state == game.state &&
               back.crc32 == game.crc32 && back.platform == game.platform);
    }

    std::puts("save sync: the parts, save-sync.json, uploads, downloads, conflicts and choices, no "
              "record, backups, save states, games the server does not have, before and after a "
              "game PASS");
    return 0;
}
