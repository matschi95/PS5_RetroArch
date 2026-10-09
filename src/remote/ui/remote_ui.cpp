/* PS5 RetroArch - the remote core's screens: games on download sources
 * (src/remote/ui/remote_ui.h says what they do).
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "remote_ui.h"

#include "qrcodegen/qrcodegen.h"

#include <algorithm>
#include <cstdio>

namespace ps5::remote::ui
{
namespace
{
/* How long a message stands. */
constexpr double message_seconds = 4.0;
/* The panel every screen draws on. */
constexpr int panel_x = 80, panel_y = 40, panel_width = frame_width - 160,
              panel_height = frame_height - 80;
constexpr int inner_x = panel_x + 40, inner_width = panel_width - 80;

std::string percent(uint64_t done, uint64_t total)
{
    char out[16];
    std::snprintf(out, sizeof out, "%d%%",
                  total ? int(std::min<uint64_t>(done * 100 / total, 100)) : 0);
    return out;
}

std::string size_text(uint64_t bytes)
{
    char out[32];
    if (bytes >= (1ull << 30))
        std::snprintf(out, sizeof out, "%.1f GB", double(bytes) / 1073741824.0);
    else
        std::snprintf(out, sizeof out, "%.1f MB", double(bytes) / 1048576.0);
    return out;
}

/* "1.2 MB of 2.0 GB, 3.4 MB/s, about 9 min left": how far a download is, and its speed
 * once it is known. */
std::string progress_line(const Download &download)
{
    std::string line = size_text(download.done) + " of " + size_text(download.total);
    if (download.rate == 0)
        return line;
    char speed[32];
    std::snprintf(speed, sizeof speed, "%.1f MB/s", double(download.rate) / 1048576.0);
    line += ", " + std::string(speed);
    if (download.total > download.done)
    {
        const uint64_t whole =
            (download.total - download.done + download.rate - 1) / download.rate; /* rounded up */
        line += whole < 90 ? ", about " + std::to_string(whole) + " s left"
                           : ", about " + std::to_string((whole + 59) / 60) + " min left";
    }
    return line;
}

std::string source_name(Services &services, const std::string &key)
{
    for (const auto &source : services.status().sources)
        if (source.key == key)
            return source.name;
    return key;
}

/* A message at the panel's foot, for a few seconds. */
struct Message
{
    std::string text;
    bool warning = false;
    double until = 0;
    void say(Services &services, const std::string &line, bool bad = false)
    {
        text = line;
        warning = bad;
        until = services.now() + message_seconds;
    }
    void draw(Canvas &c, Services &services) const
    {
        if (!text.empty() && services.now() < until)
            c.text(inner_x, panel_y + panel_height - 70, text,
                   warning ? color::warning : color::good, 2, inner_width);
    }
};

void panel(Canvas &c, const std::string &title, const std::string &subtitle)
{
    c.fill(0, 0, int(c.width()), int(c.height()), color::background);
    c.fill(panel_x, panel_y, panel_width, panel_height, color::panel);
    c.text(inner_x, panel_y + 30, title, color::title, 3, inner_width);
    if (!subtitle.empty())
        c.block(inner_x, panel_y + 72, subtitle, color::meta, inner_width);
}

void hints(Canvas &c, const std::string &line)
{
    c.text(inner_x, panel_y + panel_height - 36, line, color::meta, 2, inner_width);
}

/* ---------------------------------------------------------------- which source */
class SourceDialog
{
  public:
    explicit SourceDialog(std::vector<Game> games) : games_(std::move(games))
    {
    }
    /* The game chosen, once Cross chose it; nullptr while asking. back: Circle went back. */
    const Game *input(uint32_t pressed, bool *back)
    {
        if (pressed & up)
            focus_ = (focus_ + games_.size() - 1) % games_.size();
        if (pressed & down)
            focus_ = (focus_ + 1) % games_.size();
        if (pressed & circle)
            *back = true;
        return pressed & cross ? &games_[focus_] : nullptr;
    }
    void draw(Canvas &c, Services &services) const
    {
        panel(c, "Download from", "Several sources have this game. Which one should it come from?");
        int y = panel_y + 136;
        for (size_t i = 0; i < games_.size(); i++)
        {
            if (i == focus_)
                c.fill(inner_x - 12, y - 8, inner_width + 24, 56, color::focus);
            c.text(inner_x, y, source_name(services, games_[i].source), color::title, 2,
                   inner_width);
            c.text(inner_x, y + 24, games_[i].name + "  -  " + size_text(games_[i].size),
                   color::meta, 2, inner_width);
            y += 64;
        }
        hints(c, "Cross: download from it    Circle: back");
    }

  private:
    std::vector<Game> games_;
    size_t focus_ = 0;
};

/* ---------------------------------------------------------------- the download dialog */
class DownloadScreen final : public Screen
{
  public:
    DownloadScreen(const Stub &stub, Services &services) : services_(services), stub_(stub)
    {
        /* The title the stub stands for, as the sources have it now. */
        for (Title &title : services.titles())
            for (const Game &game : title.games)
                for (const auto &wanted : stub.games)
                    if (game.source == wanted.first && game.id == wanted.second)
                        title_ = title;
        if (title_.games.empty())
        {
            message_.say(services, "The sources no longer have this game.", true);
            return;
        }
        /* Downloaded already (a frontend that has not read its list again since): it starts. */
        for (const Game &game : title_.games)
            if (const std::string launch = services.placed(game.source, game.id); !launch.empty())
            {
                chosen_ = game;
                launch_ = launch;
                return;
            }
        /* Already coming: from the source it comes from, now first; one whose download failed
         * may come from another source this time. */
        for (const Download &download : services.downloads())
            for (const Game &game : title_.games)
                if (download.source == game.source && download.id == game.id &&
                    (download.state != State::failed || title_.games.size() == 1))
                {
                    choose(game);
                    return;
                }
        if (title_.games.size() > 1)
            sources_.reset(new SourceDialog(title_.games));
        else
            choose(title_.games.front());
    }

    void input(uint32_t pressed) override
    {
        if (sources_)
        {
            bool back = false;
            if (const Game *game = sources_->input(pressed, &back))
            {
                const Game chosen = *game;
                sources_.reset();
                choose(chosen);
            }
            else if (back)
                finished_ = true;
            return;
        }
        if (pressed & circle)
            finished_ = true; /* the download goes on in the background */
        if (!chosen_.id.empty() && (pressed & square) && launch_.empty())
        {
            const bool cancelled = services_.cancel(chosen_.source, chosen_.id);
            message_.say(services_,
                         cancelled ? "Download cancelled." : "Could not cancel the download.",
                         !cancelled);
            finished_ = cancelled;
        }
        if (!chosen_.id.empty() && (pressed & cross) && failed_)
            choose(chosen_); /* tried again */
        /* Once "Starting the game..." was drawn: it starts in this RetroArch, with its
         * platform's core, and the remote core goes (its system's list has it then). */
        if (ready_ && !started_)
        {
            started_ = true;
            services_.play(launch_, stub_.core);
        }
    }

    void draw(Canvas &c) override
    {
        if (sources_)
        {
            sources_->draw(c, services_);
            return;
        }
        const std::string name = !title_.games.empty() ? title_.games.front().name : stub_.name;
        panel(c, name, chosen_.id.empty() ? "" : "From " + source_name(services_, chosen_.source));
        std::string state;
        double share = 0;
        failed_ = false;
        if (chosen_.id.empty())
            state = "";
        else if (launch_.empty())
            launch_ = services_.placed(chosen_.source, chosen_.id);
        if (!launch_.empty())
        {
            share = 1;
            state = "Starting the game...";
            detail_.clear();
            ready_ = true;
        }
        else if (!chosen_.id.empty())
        {
            const std::vector<Download> list = services_.downloads();
            const auto at =
                std::find_if(list.begin(), list.end(), [&](const Download &d)
                             { return d.source == chosen_.source && d.id == chosen_.id; });
            if (at == list.end())
                state = "Preparing";
            else if (at->state == State::failed)
            {
                failed_ = true;
                state = "The download stopped";
                share = at->total ? double(at->done) / double(at->total) : 0;
                detail_ = at->error;
            }
            else if (at->state == State::downloading)
            {
                share = at->total ? double(at->done) / double(at->total) : 0;
                state = "Downloading " + percent(at->done, at->total);
                detail_ = progress_line(*at);
            }
            else if (at->state == State::verifying)
            {
                share = at->total ? double(at->done) / double(at->total) : 0;
                state = "Checking the game files " + percent(at->done, at->total);
            }
            else
            {
                /* Queued behind another: which one. */
                std::string other;
                for (const Download &d : list)
                    if (d.state == State::downloading || d.state == State::verifying)
                        for (const Title &title : services_.titles())
                            for (const Game &game : title.games)
                                if (game.source == d.source && game.id == d.id)
                                    other = game.name;
                state = other.empty() ? "Waiting to download" : "Waiting for " + other;
            }
        }
        draw_progress(c, state, share, detail_);
        message_.draw(c, services_);
        if (failed_)
            hints(c, "Cross: try again    Square: cancel    Circle: back");
        else if (launch_.empty() && !chosen_.id.empty())
            hints(c, "Circle: keep downloading in the background    Square: cancel");
        else
            hints(c, "Circle: back");
    }

    bool finished() const override
    {
        return finished_;
    }

  private:
    void choose(const Game &game)
    {
        /* Another source's failed download of it goes: this one comes instead. */
        for (const Download &download : services_.downloads())
            for (const Game &other : title_.games)
                if (download.source == other.source && download.id == other.id &&
                    other.source != game.source && download.state == State::failed)
                    services_.cancel(other.source, other.id);
        chosen_ = game;
        failed_ = false;
        detail_.clear();
        if (!services_.enqueue(game.source, game.id, true))
            message_.say(services_,
                         "The source no longer has this game, or it comes from another one "
                         "already.",
                         true);
    }

    void draw_progress(Canvas &c, const std::string &state, double share, const std::string &detail)
    {
        c.text(inner_x, panel_y + 150, state, failed_ ? color::warning : color::text, 2,
               inner_width);
        c.bar(inner_x, panel_y + 190, inner_width, 18, share);
        if (!detail.empty())
            c.block(inner_x, panel_y + 226, detail, color::meta, inner_width);
        if (!chosen_.id.empty() && launch_.empty() && !failed_)
            c.text(inner_x, panel_y + 320, "The game starts when the download is done.",
                   color::meta);
    }

    Services &services_;
    Stub stub_;
    Title title_;
    std::unique_ptr<SourceDialog> sources_;
    Game chosen_;
    std::string launch_, detail_; /* launch_: its file on the console, once it is there */
    bool ready_ = false, started_ = false, failed_ = false, finished_ = false;
    Message message_;
};

/* ---------------------------------------------------------------- Downloads */
class DownloadsScreen final : public Screen
{
  public:
    explicit DownloadsScreen(Services &services) : services_(services)
    {
    }

    void input(uint32_t pressed) override
    {
        const std::vector<Row> rows = this->rows();
        if (pressed & circle)
            finished_ = true;
        if (rows.empty())
            return;
        const size_t before = focus_;
        if (pressed & up)
            focus_ = (focus_ + rows.size() - 1) % rows.size();
        if (pressed & down)
            focus_ = (focus_ + 1) % rows.size();
        focus_ = std::min(focus_, rows.size() - 1);
        const Row &row = rows[focus_];
        /* A first Square asks; it stands until another button or another row. */
        if ((pressed & ~uint32_t(square)) || focus_ != before || row.kind != Row::placed_row)
            armed_.clear();
        if (pressed & cross)
        {
            if (row.kind == Row::source_row)
            {
                services_.refresh();
                reading_ = true;
                message_.say(services_, "Reading the game lists...");
            }
            else if (row.kind == Row::download_row && row.download.state == State::failed)
            {
                services_.enqueue(row.download.source, row.download.id, false);
                message_.say(services_, "Trying again.");
            }
        }
        if (pressed & square)
        {
            if (row.kind == Row::download_row)
            {
                const bool cancelled = services_.cancel(row.download.source, row.download.id);
                message_.say(services_,
                             cancelled ? "Download cancelled." : "Could not cancel the download.",
                             !cancelled);
            }
            else if (row.kind == Row::placed_row && armed_ != row.launch)
            {
                armed_ = row.launch;
                message_.say(services_, "Square again deletes " + row.game.name +
                                            " from the console. Its saves stay.");
            }
            else if (row.kind == Row::placed_row)
            {
                std::string error;
                armed_.clear();
                if (services_.remove(row.game, row.launch, &error))
                    message_.say(services_, "Deleted. It is listed on its sources again once "
                                            "you leave Downloads.");
                else
                    message_.say(services_, "Could not delete it: " + error, true);
            }
        }
    }

    void draw(Canvas &c) override
    {
        const Status status = services_.status();
        panel(c, "Downloads", "Games on your network, written by the console's FTP server.");
        if (!status.configured)
        {
            int y =
                c.block(inner_x, panel_y + 120,
                        status.error.empty() ? "No download source is set up. Save a file named "
                                               "config/remote/sources.json (over FTP) that lists "
                                               "your sources, as this one does:"
                                             : "sources.json cannot be read: " + status.error,
                        color::text, inner_width);
            if (status.error.empty())
                for (const char *line :
                     {"{ \"sources\": [", "    { \"type\": \"<type>\", \"name\": \"Home\", ... }",
                      "] }"})
                {
                    c.text(inner_x + 24, y + 8, line, color::accent);
                    y += Canvas::line(2);
                }
            hints(c, "Circle: back");
            return;
        }
        /* "Reading the game lists..." stands while they are read; what came of them, as any
         * other message, for a few seconds. */
        if (reading_ && std::none_of(status.sources.begin(), status.sources.end(),
                                     [](const SourceStatus &s) { return s.refreshing; }))
        {
            reading_ = false;
            size_t games = 0, failed = 0;
            for (const auto &source : status.sources)
            {
                games += source.games;
                failed += !source.online;
            }
            if (failed)
                message_.say(services_, std::to_string(failed) + " game list(s) could not be read.",
                             true);
            else
                message_.say(services_, "The game lists are read: " + std::to_string(games) +
                                            (games == 1 ? " game." : " games."));
        }
        const std::vector<Row> rows = this->rows();
        focus_ = rows.empty() ? 0 : std::min(focus_, rows.size() - 1);
        /* The rows around the one in focus, as many as fit above the message and hints. */
        constexpr size_t shown = 4;
        const size_t first = focus_ >= shown ? focus_ - shown + 1 : 0;
        int y = panel_y + 116;
        Row::Kind section = Row::Kind(-1);
        for (size_t i = first; i < rows.size() && i < first + shown; i++)
        {
            const Row &row = rows[i];
            if (row.kind != section)
            {
                section = row.kind;
                c.text(inner_x, y,
                       section == Row::source_row     ? "Sources"
                       : section == Row::download_row ? "Queue"
                                                      : "Downloaded",
                       color::accent);
                y += 24;
            }
            if (i == focus_)
                c.fill(inner_x - 12, y - 6, inner_width + 24, 50, color::focus);
            c.text(inner_x, y, row.title, color::title, 2, inner_width - 300);
            c.text(inner_x + inner_width - 288, y, row.state,
                   row.warning ? color::warning : color::text, 2, 288);
            c.text(inner_x, y + 22, row.line, color::meta, 2, inner_width);
            y += 52;
        }
        if (std::none_of(rows.begin(), rows.end(),
                         [](const Row &r) { return r.kind != Row::source_row; }))
            c.block(inner_x, y + 10,
                    "No downloads. Choosing a game in a (Remote) playlist downloads it.",
                    color::meta, inner_width);
        message_.draw(c, services_);
        if (!rows.empty())
        {
            const Row &row = rows[focus_];
            hints(c, row.kind == Row::source_row
                         ? "Cross: read the game lists again    Circle: back"
                     : row.kind == Row::download_row
                         ? (row.download.state == State::failed
                                ? "Cross: try again    Square: cancel    Circle: back"
                                : "Square: cancel    Circle: back")
                         : "Square twice: delete from the console    Circle: back");
        }
    }

    bool finished() const override
    {
        return finished_;
    }

  private:
    struct Row
    {
        enum Kind
        {
            source_row,
            download_row,
            placed_row
        } kind;
        std::string title, line, state;
        bool warning = false;
        Download download;
        Game game;
        std::string launch; /* placed_row: its file on the console */
    };

    std::vector<Row> rows()
    {
        std::vector<Row> list;
        const Status status = services_.status();
        for (const auto &source : status.sources)
        {
            Row row{Row::source_row, source.name, source.address, "", false, {}, {}, ""};
            const bool trouble = !source.error.empty() && !source.online;
            row.state = source.refreshing ? "Reading..."
                        : source.online   ? std::to_string(source.games) +
                                              (source.games == 1 ? " game" : " games")
                        : source.too_old ? "Too old"
                        : trouble        ? "Not reachable"
                                         : "Not connected yet";
            if (trouble)
            {
                row.warning = true;
                row.line = source.error;
            }
            list.push_back(row);
        }
        /* The titles and what of them is on the console: read again only when a list, a
         * cover or a game on the console changed (the screen asks every frame). */
        if (status.generation != generation_)
        {
            generation_ = status.generation;
            titles_ = services_.titles();
            placed_.clear();
            for (const Title &title : titles_)
                for (const Game &game : title.games)
                    if (const std::string launch = services_.placed(game.source, game.id);
                        !launch.empty())
                        placed_.emplace_back(game, launch);
        }
        const std::vector<Title> &titles = titles_;
        const auto name_of = [&](const std::string &source, const std::string &id)
        {
            for (const Title &title : titles)
                for (const Game &game : title.games)
                    if (game.source == source && game.id == id)
                        return game.name;
            return id;
        };
        for (const Download &download : services_.downloads())
        {
            Row row{Row::download_row,
                    name_of(download.source, download.id),
                    "",
                    "",
                    false,
                    download,
                    {},
                    ""};
            const std::string from = source_name(services_, download.source);
            switch (download.state)
            {
            case State::queued:
                row.state = "Queued";
                row.line = "Queued, from " + from;
                break;
            case State::downloading:
                row.state = percent(download.done, download.total);
                row.line = progress_line(download) + ", from " + from;
                break;
            case State::verifying:
                row.state = "Checking " + percent(download.done, download.total);
                row.line = "From " + from;
                break;
            case State::failed:
                row.state = "Download failed";
                row.warning = true;
                row.line = download.error;
                break;
            }
            list.push_back(row);
        }
        for (const auto &placed : placed_)
            list.push_back({Row::placed_row,
                            placed.first.name,
                            placed.second,
                            "From " + source_name(services_, placed.first.source),
                            false,
                            {},
                            placed.first,
                            placed.second});
        return list;
    }

    Services &services_;
    size_t focus_ = 0;
    bool reading_ = false, finished_ = false;
    std::string armed_; /* the game a first Square asked to delete */
    uint64_t generation_ = ~uint64_t(0);
    std::vector<Title> titles_;
    std::vector<std::pair<Game, std::string>> placed_; /* the games on the console, their files */
    Message message_;
};

/* A stub that is no game of a source any more. */
class GoneScreen final : public Screen
{
  public:
    explicit GoneScreen(std::string name) : name_(std::move(name))
    {
    }
    void input(uint32_t pressed) override
    {
        finished_ = (pressed & (circle | cross)) != 0;
    }
    void draw(Canvas &c) override
    {
        panel(c, name_.empty() ? "Download sources" : name_, "");
        c.block(inner_x, panel_y + 120,
                "This entry is not known any more. Its playlist is written again at the "
                "next start.",
                color::text, inner_width);
        hints(c, "Circle: back");
    }
    bool finished() const override
    {
        return finished_;
    }

  private:
    std::string name_;
    bool finished_ = false;
};
/* ---------------------------------------------------------------- Save sync */
/* A code as it is easier to read: in two halves. */
std::string spaced(const std::string &code)
{
    return code.size() == 8 ? code.substr(0, 4) + " " + code.substr(4) : code;
}

/* A QR code of `text` in a white square at x, y of `size` pixels (its quiet zone in it). */
void draw_qr(Canvas &c, int x, int y, int size, const std::string &text)
{
    std::vector<uint8_t> scratch(qrcodegen_BUFFER_LEN_MAX), code(qrcodegen_BUFFER_LEN_MAX);
    c.fill(x, y, size, size, 0xffffff);
    if (!qrcodegen_encodeText(text.c_str(), scratch.data(), code.data(), qrcodegen_Ecc_LOW,
                              qrcodegen_VERSION_MIN, 10, qrcodegen_Mask_AUTO, true))
        return;
    const int modules = qrcodegen_getSize(code.data());
    const int scale = size / (modules + 8);
    const int start = (size - modules * scale) / 2;
    for (int row = 0; row < modules; row++)
        for (int column = 0; column < modules; column++)
            if (qrcodegen_getModule(code.data(), column, row))
                c.fill(x + start + column * scale, y + start + row * scale, scale, scale, 0x000000);
}

/* Pairing with a server: the QR code of its approval page at the left, what to do at the
 * right, until the player approved it on the phone. */
class PairingDialog
{
  public:
    PairingDialog(Services &services, PairServer server)
        : services_(services), server_(std::move(server))
    {
        services_.pair_start(server_);
    }
    /* False once it is closed. */
    bool input(uint32_t pressed)
    {
        const PairingRun::View view = services_.pairing();
        const bool over =
            view.stage != PairingRun::View::starting && view.stage != PairingRun::View::waiting;
        if (pressed & circle)
        {
            services_.pair_cancel();
            return false;
        }
        if ((pressed & cross) && over)
        {
            if (view.stage == PairingRun::View::approved)
                return false;
            services_.pair_start(server_); /* tried again */
        }
        return true;
    }
    void draw(Canvas &c) const
    {
        const PairingRun::View view = services_.pairing();
        panel(c, "Pair with " + server_.name, server_.url);
        int y = panel_y + 136;
        switch (view.stage)
        {
        case PairingRun::View::waiting:
        {
            draw_qr(c, inner_x, y - 16, 220, view.address);
            const int x = inner_x + 250, width = inner_width - 250;
            int at = c.block(x, y,
                             "Scan the code with your phone, or open the address, sign in as "
                             "yourself and approve:",
                             color::text, width);
            c.text(x, at + 10, spaced(view.code), color::title, 4, width);
            at = c.block(x, at + 70, view.address, color::meta, width);
            const int left = std::max(0, int(view.expires - services_.now()));
            char ends[48];
            std::snprintf(ends, sizeof ends, "The code works for %d:%02d.", left / 60, left % 60);
            c.text(x, at + 14, ends, color::meta, 2, width);
            hints(c, "Circle: cancel");
            return;
        }
        case PairingRun::View::approved:
            c.text(inner_x, y, "Paired", color::good);
            c.block(inner_x, y + 34,
                    "The save data syncs as " + (view.user.empty() ? "you" : view.user) +
                        " now: before and after each game.",
                    color::text, inner_width);
            hints(c, "Cross: done");
            return;
        case PairingRun::View::denied:
        case PairingRun::View::expired:
        case PairingRun::View::failed:
            c.text(inner_x, y,
                   view.stage == PairingRun::View::denied    ? "The pairing was refused"
                   : view.stage == PairingRun::View::expired ? "The code ran out"
                                                             : "The pairing did not work",
                   color::warning);
            if (!view.error.empty())
                c.block(inner_x, y + 34, view.error, color::meta, inner_width);
            hints(c, "Cross: try again    Circle: back");
            return;
        default:
            c.text(inner_x, y, "Asking the server for a code...", color::text);
            hints(c, "Circle: cancel");
            return;
        }
    }

  private:
    Services &services_;
    PairServer server_;
};

/* Save sync: the server the save data is kept on, pairing with one, unlinking it. */
class SaveSyncScreen final : public Screen
{
  public:
    explicit SaveSyncScreen(Services &services) : services_(services)
    {
        setup_ = services_.save_setup();
    }
    void input(uint32_t pressed) override
    {
        if (pairing_)
        {
            if (!pairing_->input(pressed))
            {
                pairing_.reset();
                setup_ = services_.save_setup();
            }
            return;
        }
        const size_t rows = setup_.servers.size();
        if (rows > 0 && (pressed & up))
            focus_ = (focus_ + rows - 1) % rows;
        if (rows > 0 && (pressed & down))
            focus_ = (focus_ + 1) % rows;
        if (pressed & ~uint32_t(square))
            armed_ = false;
        if (pressed & circle)
            finished_ = true;
        else if ((pressed & cross) && rows > 0)
            pairing_.reset(new PairingDialog(services_, setup_.servers[focus_]));
        else if ((pressed & square) && !setup_.type.empty())
        {
            /* A first Square asks; a second unlinks. */
            if (!armed_)
                armed_ = true;
            else
            {
                armed_ = false;
                std::string error;
                const bool done = services_.unlink(&error);
                message_.say(services_,
                             done ? "Unlinked: the save data stays on the console only."
                                  : "Could not unlink: " + error,
                             !done);
                setup_ = services_.save_setup();
            }
        }
    }
    void draw(Canvas &c) override
    {
        if (pairing_)
        {
            pairing_->draw(c);
            return;
        }
        panel(c, "Save sync",
              "Your saves and save states on a server, synced before and after each game.");
        int y = panel_y + 140;
        if (!setup_.readable)
            y = c.block(inner_x, y, setup_.error + ": fix it over FTP.", color::warning,
                        inner_width);
        else if (setup_.type.empty())
            y = c.block(inner_x, y,
                        "Not set up: pair with a server below, or fill in "
                        "config/remote/save-sync.json over FTP.",
                        color::text, inner_width);
        else
        {
            c.text(inner_x, y, "Server: " + setup_.url, color::title, 2, inner_width);
            if (!setup_.user.empty())
                c.text(inner_x, y + 26, "Signed in as " + setup_.user, color::text, 2, inner_width);
            std::string line = setup_.automatic ? "Before and after each game" : "Turned off";
            if (setup_.automatic)
                line += setup_.states ? ", with the save states" : ", without the save states";
            c.text(inner_x, y + 52, line, color::meta, 2, inner_width);
            if (setup_.waiting > 0)
                c.text(inner_x, y + 78,
                       std::to_string(setup_.waiting) + " game(s) waiting to go up", color::meta, 2,
                       inner_width);
            y += 110;
        }
        y += 12;
        if (setup_.servers.empty())
            c.block(inner_x, y,
                    "Pairing needs a RomM server among the download sources (sources.json).",
                    color::meta, inner_width);
        for (size_t i = 0; i < setup_.servers.size(); i++)
        {
            if (i == focus_)
                c.fill(inner_x - 12, y - 6, inner_width + 24, 32, color::focus);
            c.text(inner_x, y,
                   "Pair with " + setup_.servers[i].name + " (" + setup_.servers[i].url + ")",
                   color::title, 2, inner_width);
            y += 36;
        }
        if (armed_)
            c.text(inner_x, panel_y + panel_height - 70,
                   "Square again unlinks the server; the save data stays.", color::warning, 2,
                   inner_width);
        else
            message_.draw(c, services_);
        std::string keys = setup_.servers.empty() ? "" : "Cross: pair    ";
        if (!setup_.type.empty())
            keys += "Square twice: unlink    ";
        hints(c, keys + "Circle: back");
    }
    bool finished() const override
    {
        return finished_;
    }

  private:
    Services &services_;
    SaveSetup setup_;
    std::unique_ptr<PairingDialog> pairing_;
    size_t focus_ = 0;
    bool armed_ = false, finished_ = false;
    Message message_;
};
} // namespace

std::unique_ptr<Screen> open(const Stub &stub, Services &services)
{
    if (stub.screen == "downloads")
        return std::unique_ptr<Screen>(new DownloadsScreen(services));
    if (stub.screen == "savesync")
        return std::unique_ptr<Screen>(new SaveSyncScreen(services));
    if (stub.games.empty())
        return std::unique_ptr<Screen>(new GoneScreen(stub.name));
    return std::unique_ptr<Screen>(new DownloadScreen(stub, services));
}
} // namespace ps5::remote::ui
