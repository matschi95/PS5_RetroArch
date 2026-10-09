/* The download sources and the save sync against a real RomM (tools/check-romm.py starts one in
 * Docker, with a few fake games, and runs this as its player).
 *
 *   romm-live-test <url> <token> <token with roms.read only> <folder> <library> <old: 0|1>
 *
 * A RomM older than the backends take (old, romm::minimum_version) is refused by each of them,
 * saying so: the games, the firmware, the save sync and pairing. Otherwise: the games listed
 * with their files and checksums, one
 * downloaded whole and checked as it came, one stopped part way and gone on with; the save
 * and the save states of a game synced between two consoles (each a device of its own): up,
 * unchanged, down, a conflict and the player's choices, a state the server keeps no hash of; a
 * token without the scopes the save sync needs refused, naming them; the firmware in the
 * library's bios/snes listed and fetched for a core that names it; the WebUI's setup signed in
 * with the player's password, its token able to list the games, made again in its place.
 */
#include "../src/remote/backends.h"
#include "../src/remote/files.h"
#include "../src/remote/library.h"
#include "../src/remote/remote.h"
#include "../src/remote/pairing.h"
#include "../src/remote/romm/romm_client.h"
#include "../src/remote/romm/romm_client.h"
#include "../src/remote/romm/romm_saves.h"
#include "../src/remote/save_config.h"
#include "../src/remote/save_sync.h"
#include "../src/remote/servers.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <zlib.h>

namespace remote = ps5::remote;
namespace files = ps5::remote::files;
using ps5_scraper::Json;

namespace
{
int failures = 0;

void check(bool ok, const std::string &what)
{
    std::printf("romm-live: %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    std::fflush(stdout);
    failures += !ok;
}

std::string read(const std::string &path)
{
    std::string text;
    files::read(path, &text, 512u << 20);
    return text;
}

void write(const std::string &path, const std::string &data)
{
    assert(files::make_folders(files::parent(path)) && files::write(path, data));
}

std::string crc_of(const std::string &bytes)
{
    char out[16];
    std::snprintf(out, sizeof out, "%08lx",
                  ::crc32(0, reinterpret_cast<const Bytef *>(bytes.data()), uInt(bytes.size())));
    return out;
}

template <typename Condition> bool until(Condition condition, int seconds)
{
    for (int i = 0; i < seconds * 100 && !condition(); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return condition();
}

Json entry(const std::string &url, const std::string &token)
{
    Json settings = Json::record();
    settings.set("type", Json::of("romm"));
    settings.set("url", Json::of(url));
    if (!token.empty())
        settings.set("token", Json::of(token));
    else if (const char *user = std::getenv("ROMM_LIVE_USER"))
    {
        /* Without a token: the player's name and password. */
        const std::string pair = user;
        settings.set("username", Json::of(pair.substr(0, pair.find(':'))));
        settings.set("password", Json::of(pair.substr(pair.find(':') + 1)));
    }
    return settings;
}

/* A console: its own folder for the store (another device of the user) and its own files. */
struct Console
{
    remote::SyncGame game;
    remote::SyncPlaces places;
    remote::SyncChoice choice = remote::SyncChoice::neither;
    int asked = 0;
};

remote::SyncResult sync(const Json &settings, Console &console)
{
    return remote::sync_save_data("romm", settings, console.game, console.places,
                                  [&](const remote::SavePlan &, const remote::LocalSave &)
                                  {
                                      console.asked++;
                                      return console.choice;
                                  },
                                  {});
}

Console console_at(const std::string &folder, const std::string &library)
{
    Console console;
    console.game.platform = "snes";
    console.game.name = "Alpha Quest";
    console.game.content = folder + "/content/SNES/Alpha Quest (USA).sfc";
    console.game.crc32 = crc_of(read(library + "/roms/snes/Alpha Quest (USA).sfc"));
    console.game.emulator = "snes9x";
    console.game.save = folder + "/saves/snes9x/Alpha Quest (USA).srm";
    console.game.state = folder + "/states/snes9x/Alpha Quest (USA).state";
    console.places.store = folder + "/config/remote/saves";
    console.places.work = folder + "/config/remote/saves/work";
    console.places.backups = folder + "/config/remote/saves/backups";
    return console;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 7)
    {
        std::fprintf(stderr, "usage: romm-live-test url token bare-token folder library old\n");
        return 2;
    }
    const std::string url = argv[1], token = argv[2], bare = argv[3], folder = argv[4],
                      library = argv[5];
    const bool old = std::strcmp(argv[6], "1") == 0;
    const Json settings = entry(url, token);

    /* A server older than the backends take: each refuses it, saying so. */
    std::string error;
    std::unique_ptr<remote::Source> source = remote::make_source("romm", settings, &error);
    check(source != nullptr, "a source of the entry " + error);
    if (old)
    {
        const std::string too_old =
            std::string("is too old: it needs RomM ") + remote::romm::minimum_version + " or newer";
        std::vector<remote::SourceGame> games;
        std::string version, needed;
        const bool refused = source && !source->list(&games, &error, {}) &&
                             source->too_old(&version, &needed) && !version.empty();
        check(refused && error.find(too_old) != std::string::npos &&
                  needed == remote::romm::minimum_version,
              "the games refused: " + error);
        auto firmware = remote::make_firmware_source("romm", settings, &error);
        std::vector<remote::FirmwareFile> files_;
        error.clear();
        const bool firmware_refused = firmware && !firmware->list(&files_, &error, {});
        check(firmware_refused && error.find(too_old) != std::string::npos,
              "the firmware refused: " + error);
        Console first = console_at(folder + "/first", library);
        write(first.game.save, "save");
        const remote::SyncResult result = sync(settings, first);
        check(result.outcome == remote::SyncOutcome::failed && result.too_old &&
                  result.needed == remote::romm::minimum_version,
              "the save sync refused: " + result.message);
        remote::PairingStart start;
        error.clear();
        const bool pairing_refused =
            !remote::romm::pairing.start(settings, folder + "/pair", &start, &error, {});
        check(pairing_refused && error.find(too_old) != std::string::npos,
              "pairing refused: " + error);
        std::printf("romm-live: %s\n", failures == 0 ? "PASS" : "FAIL");
        return failures == 0 ? 0 : 1;
    }

    /* The games: all of them, with their files and checksums. */
    std::vector<remote::SourceGame> games;
    const bool listed = source && source->list(&games, &error, {});
    check(listed && games.size() == 3,
          "the games listed (" + std::to_string(games.size()) + ") " + error);
    const std::string alpha = read(library + "/roms/snes/Alpha Quest (USA).sfc");
    bool hashed = false;
    for (const auto &game : games)
        for (const auto &file : game.files)
            hashed = hashed || (file.name == "Alpha Quest (USA).sfc" &&
                                remote::normal_crc(file.crc32) == crc_of(alpha));
    check(hashed, "a file's CRC32 as RomM hashed it");

    /* The WebUI's setup: the player's name and password, once; the console's own token. */
    if (const char *pair = std::getenv("ROMM_LIVE_USER"))
    {
        const std::string user = pair;
        remote::ServerSetup setup;
        setup.name = "Live";
        setup.url = url;
        setup.user = user.substr(0, user.find(':'));
        setup.password = user.substr(user.find(':') + 1);
        setup.saves = true;
        const std::string config = folder + "/setup";
        std::string why;
        const bool saved = remote::save_server(config, setup, "", &why) &&
                           remote::save_server(config, setup, "Live", &why);
        std::string text_;
        Json sources;
        files::read(config + "/sources.json", &text_);
        Json::parse(text_, &sources);
        const Json made = sources["sources"].items.empty() ? Json() : sources["sources"].items[0];
        auto by_token = remote::make_source("romm", entry(url, made["token"].str()), &why);
        std::vector<remote::SourceGame> listed_games;
        const bool lists = by_token && by_token->list(&listed_games, &why, {});
        /* Made again: the console has one token, not two. */
        Json basic = Json::record();
        basic.set("url", Json::of(url));
        basic.set("username", Json::of(setup.user));
        basic.set("password", Json::of(setup.password));
        auto client = remote::romm::Client::make(basic, "the check", &why);
        remote::romm::Client::Answer answer;
        Json tokens;
        int ours = 0;
        if (client && client->send(nullptr, "/api/client-tokens", {}, &answer, &why) &&
            Json::parse(answer.body, &tokens))
            for (const Json &token : tokens.items)
                ours += token["name"].str().rfind("PS5 RetroArch ", 0) == 0;
        check(saved && made["__server_user"].str() == setup.user && lists &&
                  listed_games.size() == 3 && ours == 1,
              "the WebUI's setup signed in, its token lists the games, one token (" +
                  std::to_string(ours) + ") " + why);
    }

    /* Firmware: the BIOS in the library's bios/snes, for a core that names it (and one it
     * does not have); a token without the scope told so. */
    {
        std::shared_ptr<remote::FirmwareSource> firmware =
            remote::make_firmware_source("romm", settings, &error);
        std::vector<remote::FirmwareFile> files_;
        const bool listed_ = firmware && firmware->list(&files_, &error, {});
        const remote::FirmwareFile *bsx = nullptr;
        for (const auto &file : files_)
            if (file.name == "BS-X.bin")
                bsx = &file;
        check(listed_ && bsx && !bsx->crc32.empty() &&
                  std::find(bsx->systems.begin(), bsx->systems.end(), "snes") != bsx->systems.end(),
              "the firmware listed with its platform (" + std::to_string(files_.size()) + ") " +
                  error);
        remote::CoreFirmware core;
        core.core = "Snes9x";
        core.platforms = {"snes"};
        core.files = {{"BS-X.bin", true}, {"STBIOS.bin", false}};
        const remote::FirmwareOutcome outcome =
            remote::fetch_firmware(core, folder + "/system", {{"RomM", firmware, files_}}, {});
        check(outcome.fetched == std::vector<std::string>{"BS-X.bin"} &&
                  read(folder + "/system/BS-X.bin") == read(library + "/bios/snes/BS-X.bin") &&
                  outcome.missing == std::vector<std::string>{"STBIOS.bin"},
              "a core's firmware fetched: " + remote::firmware_notice(outcome, true));
        if (!bare.empty())
        {
            std::shared_ptr<remote::FirmwareSource> refused =
                remote::make_firmware_source("romm", entry(url, bare), &error);
            error.clear();
            const bool refused_ = refused && !refused->list(&files_, &error, {});
            check(refused_ && error.find("firmware.read") != std::string::npos,
                  "a token without the scope refused: " + error);
        }
    }

    Console first = console_at(folder + "/first", library);

    /* Downloads: one whole, checked as it came; one stopped part way and gone on with. */
    {
        remote::Paths paths = remote::Paths::at(folder + "/title");
        write(paths.cores + "/snes9x_libretro.so", "");
        write(paths.info + "/snes9x_libretro.info",
              "display_name = \"snes9x\"\ndatabase = \"Nintendo - Super Nintendo Entertainment "
              "System\"\nsupported_extensions = \"sfc|smc\"\n");
        Json sources = Json::record();
        Json entry_ = settings;
        entry_.set("name", Json::of("Home"));
        sources.set("sources", Json::list()).push(entry_);
        write(paths.config + "/sources.json", sources.write(true));
        remote::list_new(paths, 10);
        remote::start(paths, {});
        std::string alpha_id, big_id;
        for (const auto &title : remote::titles())
            for (const auto &game : title.games)
            {
                if (game.name.rfind("Alpha Quest", 0) == 0)
                    alpha_id = game.id;
                if (game.name.rfind("Delta Big", 0) == 0)
                    big_id = game.id;
            }
        check(!alpha_id.empty() && !big_id.empty(), "the games as titles");
        check(remote::enqueue("home", alpha_id, false) &&
                  until([] { return remote::downloads().empty(); }, 60),
              "a download");
        const std::string alpha_folder = remote::placed_folder("home", alpha_id);
        check(read(alpha_folder + "/Alpha Quest (USA).sfc") == alpha, "...whole");
        const std::string big = read(library + "/roms/snes/Delta Big (USA).sfc");
        remote::enqueue("home", big_id, false);
        until(
            []
            {
                const auto list = remote::downloads();
                return !list.empty() && list[0].done > (8u << 20);
            },
            60);
        remote::stop();
        check(!remote::downloads().empty() && remote::downloads()[0].done < big.size(),
              "a download stopped part way");
        remote::start(paths, {});
        check(until([] { return remote::downloads().empty(); }, 120) &&
                  read(remote::placed_folder("home", big_id) + "/Delta Big (USA).sfc") == big,
              "...gone on with and whole");
        remote::stop();
    }

    /* The save: up from the first console, unchanged, down to the second. */
    Console second = console_at(folder + "/second", library);
    write(first.game.save, "first save");
    remote::SyncResult result = sync(settings, first);
    check(result.outcome == remote::SyncOutcome::uploaded && result.user == "player",
          "the first console's save up: " + result.message);
    result = sync(settings, first);
    check(result.outcome == remote::SyncOutcome::same, "...unchanged: " + result.message);
    result = sync(settings, second);
    check(result.outcome == remote::SyncOutcome::downloaded &&
              read(second.game.save) == "first save",
          "...down to the second console: " + result.message);

    /* Played on the second: up; down on the first. */
    write(second.game.save, "second save");
    result = sync(settings, second);
    check(result.outcome == remote::SyncOutcome::uploaded, "the second's up: " + result.message);
    result = sync(settings, first);
    check(result.outcome == remote::SyncOutcome::downloaded &&
              read(first.game.save) == "second save",
          "...down to the first: " + result.message);

    /* Played on both: a conflict, left as it is, then the first console's chosen. */
    write(first.game.save, "first again");
    write(second.game.save, "second again");
    result = sync(settings, second);
    check(result.outcome == remote::SyncOutcome::uploaded, "the second's again: " + result.message);
    result = sync(settings, first);
    check(result.outcome == remote::SyncOutcome::kept && first.asked == 1 &&
              read(first.game.save) == "first again",
          "a conflict, left as it is: " + result.message);
    first.choice = remote::SyncChoice::console;
    result = sync(settings, first);
    check(result.outcome == remote::SyncOutcome::uploaded,
          "...the console's chosen: " + result.message);
    second.choice = remote::SyncChoice::server;
    result = sync(settings, second);
    check(result.outcome == remote::SyncOutcome::downloaded &&
              read(second.game.save) == "first again",
          "...down to the second: " + result.message);

    /* Save states: the first's slot 1 up, down to the second; one with the same contents on
     * both, which RomM keeps no hash of, fetched once and found the same. */
    write(first.game.state + "1", "state one");
    write(first.game.state + ".auto", "auto state");
    result = sync(settings, first);
    check(result.outcome == remote::SyncOutcome::uploaded,
          "the first's states up: " + result.message);
    write(second.game.state + ".auto", "auto state");
    result = sync(settings, second);
    check(result.outcome == remote::SyncOutcome::downloaded &&
              read(second.game.state + "1") == "state one" && second.asked == 0,
          "...down to the second, the same one left: " + result.message);
    write(second.game.state + "1", "state one, played on");
    result = sync(settings, second);
    check(result.outcome == remote::SyncOutcome::uploaded,
          "a state played on, up: " + result.message);
    result = sync(settings, first);
    check(result.outcome == remote::SyncOutcome::downloaded &&
              read(first.game.state + "1") == "state one, played on",
          "...down to the first: " + result.message);

    /* A token without the save sync's scopes: refused, naming one. */
    Console third = console_at(folder + "/third", library);
    write(third.game.save, "save");
    result = sync(entry(url, bare), third);
    check(result.outcome == remote::SyncOutcome::failed &&
              result.message.find("lacks the scope") != std::string::npos,
          "a token without the scopes: " + result.message);

    /* Pairing: the code approved on the server as the player (as the phone does), the console
     * gets a sign-in of its own in save-sync.json, and syncs with it as the same device. */
    {
        const std::string pair_folder = folder + "/paired";
        const std::string config = pair_folder + "/config/remote/save-sync.json";
        assert(remote::prepare_save_config(config));
        write(pair_folder + "/config/remote/sources.json",
              "{\"sources\":[{\"type\":\"romm\",\"name\":\"Home\",\"url\":\"" + url +
                  "\",\"token\":\"" + token + "\"}]}");
        const std::vector<remote::PairServer> servers =
            remote::pair_servers(pair_folder + "/config/remote/sources.json");
        check(servers.size() == 1 && servers[0].name == "Home", "a source to pair with");
        const auto clock = []
        {
            return std::chrono::duration<double>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        };
        remote::PairingRun run(clock, config, pair_folder + "/config/remote/save-sync");
        /* As the player on the phone: approve, or refuse. */
        const auto answer = [&](const std::string &code, bool approve)
        {
            const char *user = std::getenv("ROMM_LIVE_USER");
            const std::string pair = user ? user : "";
            Json player = Json::record();
            player.set("url", Json::of(url));
            player.set("username", Json::of(pair.substr(0, pair.find(':'))));
            player.set("password", Json::of(pair.substr(pair.find(':') + 1)));
            std::string error;
            auto client = remote::romm::Client::make(player, "check", &error);
            Json payload = Json::record();
            payload.set("user_code", Json::of(code));
            if (approve)
            {
                Json &scopes = payload.set("approved_scopes", Json::list());
                for (const char *scope :
                     {"platforms.read", "roms.read", "assets.read", "assets.write", "devices.read",
                      "devices.write", "me.read"})
                    scopes.push(Json::of(scope));
            }
            remote::romm::Client::Answer reply;
            return client &&
                   client->send("POST",
                                approve ? "/api/auth/device/approve" : "/api/auth/device/deny",
                                payload.write(), &reply, &error) &&
                   (reply.status == 200 || reply.status == 204);
        };
        run.start(servers[0].type, servers[0].settings);
        check(until([&] { return run.view().stage == remote::PairingRun::View::waiting; }, 30),
              "a code to approve: " + run.view().code + " " + run.view().error);
        const remote::PairingRun::View waiting = run.view();
        check(waiting.address.find(waiting.code) != std::string::npos &&
                  waiting.address.rfind(url, 0) == 0,
              "...with the address to approve it at: " + waiting.address);
        check(answer(waiting.code, false), "...refused on the server");
        check(until([&] { return run.view().stage == remote::PairingRun::View::denied; }, 30),
              "...and so on the console");
        run.start(servers[0].type, servers[0].settings);
        until([&] { return run.view().stage == remote::PairingRun::View::waiting; }, 30);
        check(answer(run.view().code, true), "a code approved on the server");
        check(until([&] { return run.view().stage == remote::PairingRun::View::approved; }, 30) &&
                  run.view().user == "player",
              "...paired as the player: " + run.view().error);
        const remote::SaveConfig paired = remote::read_save_config(config);
        check(paired.type == "romm" && paired.settings["token"].str().rfind("rmm_", 0) == 0 &&
                  paired.settings["__server_user"].str() == "player",
              "...its sign-in in save-sync.json");
        Console fourth = console_at(pair_folder, library);
        fourth.places.store = pair_folder + "/config/remote/save-sync";
        result = remote::sync_save_data("romm", paired.settings, fourth.game, fourth.places,
                                        nullptr, {});
        check(result.outcome == remote::SyncOutcome::downloaded &&
                  read(fourth.game.save) == "first again",
              "...and syncs with it: " + result.message);
    }

    std::printf("romm-live: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
