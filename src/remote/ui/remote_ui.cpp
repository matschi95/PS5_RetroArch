/* PS5 RetroArch - the remote core's screens: games on download sources
 * (src/remote/ui/remote_ui.h says what they do).
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "remote_ui.h"

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
        /* Downloaded already (the playlists take it at the next start): it starts. */
        for (const Game &game : title_.games)
            if (const std::string folder = services.placed(game.source, game.id); !folder.empty())
            {
                chosen_ = game;
                folder_ = folder;
                return;
            }
        /* Already coming: from the source it comes from, now first. */
        for (const Download &download : services.downloads())
            for (const Game &game : title_.games)
                if (download.source == game.source && download.id == game.id)
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
        if (!chosen_.id.empty() && (pressed & square) && folder_.empty())
        {
            const bool cancelled = services_.cancel(chosen_.source, chosen_.id);
            message_.say(services_,
                         cancelled ? "Download cancelled." : "Could not cancel the download.",
                         !cancelled);
            finished_ = cancelled;
        }
        if (!chosen_.id.empty() && (pressed & cross) && failed_)
            choose(chosen_); /* tried again */
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
        else if (folder_.empty())
            folder_ = services_.placed(chosen_.source, chosen_.id);
        if (!folder_.empty())
        {
            share = 1;
            state = play_error_.empty() ? "Starting the game..." : "The game could not be started";
            if (!played_)
            {
                played_ = true;
                draw_progress(c, state, share, "");
                play_error_ = services_.play(chosen_, folder_ + "/" + chosen_.file, stub_.core);
                if (play_error_.empty())
                    play_error_ = "game mode did not start";
            }
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
                detail_ = size_text(at->done) + " of " + size_text(at->total);
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
        draw_progress(c, state, share,
                      failed_ ? detail_ : (folder_.empty() ? detail_ : play_error_));
        message_.draw(c, services_);
        if (failed_)
            hints(c, "Cross: try again    Square: cancel    Circle: back");
        else if (folder_.empty() && !chosen_.id.empty())
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
        chosen_ = game;
        failed_ = false;
        detail_.clear();
        if (!services_.enqueue(game.source, game.id, true))
            message_.say(services_, "The source no longer has this game.", true);
    }

    void draw_progress(Canvas &c, const std::string &state, double share, const std::string &detail)
    {
        c.text(inner_x, panel_y + 150, state, failed_ ? color::warning : color::text, 2,
               inner_width);
        c.bar(inner_x, panel_y + 190, inner_width, 18, share);
        if (!detail.empty())
            c.block(inner_x, panel_y + 226, detail, color::meta, inner_width);
        if (!chosen_.id.empty() && folder_.empty() && !failed_)
            c.text(inner_x, panel_y + 320, "The game starts when the download is done.",
                   color::meta);
    }

    Services &services_;
    Stub stub_;
    Title title_;
    std::unique_ptr<SourceDialog> sources_;
    Game chosen_;
    std::string folder_, detail_, play_error_;
    bool played_ = false, failed_ = false, finished_ = false;
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
        if (pressed & up)
            focus_ = (focus_ + rows.size() - 1) % rows.size();
        if (pressed & down)
            focus_ = (focus_ + 1) % rows.size();
        focus_ = std::min(focus_, rows.size() - 1);
        const Row &row = rows[focus_];
        if (!(pressed & square) || row.kind != Row::placed_row)
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
            else if (row.kind == Row::placed_row && armed_ != row.folder)
            {
                armed_ = row.folder;
                message_.say(services_, "Square again deletes " + row.game.name +
                                            " from the console. Its saves stay.");
            }
            else if (row.kind == Row::placed_row)
            {
                std::string error;
                armed_.clear();
                if (services_.remove(row.game, row.folder, &error))
                    message_.say(services_, "Deleted. It is listed on its sources again at the "
                                            "next start.");
                else
                    message_.say(services_, "Could not delete it: " + error, true);
            }
        }
    }

    void draw(Canvas &c) override
    {
        const Status status = services_.status();
        panel(c, "Downloads", "Games on your network, downloaded when you play them.");
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
        std::string folder;
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
                        : trouble ? "Not reachable"
                                  : "Not connected yet";
            if (trouble)
            {
                row.warning = true;
                row.line = source.error;
            }
            list.push_back(row);
        }
        const std::vector<Title> titles = services_.titles();
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
                row.line = size_text(download.done) + " of " + size_text(download.total) +
                           ", from " + from;
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
        for (const Title &title : titles)
            for (const Game &game : title.games)
                if (const std::string folder = services_.placed(game.source, game.id);
                    !folder.empty())
                    list.push_back({Row::placed_row,
                                    game.name,
                                    folder,
                                    "From " + source_name(services_, game.source),
                                    false,
                                    {},
                                    game,
                                    folder});
        return list;
    }

    Services &services_;
    size_t focus_ = 0;
    bool reading_ = false, finished_ = false;
    std::string armed_; /* the game a first Square asked to delete */
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
} // namespace

std::unique_ptr<Screen> open(const Stub &stub, Services &services)
{
    if (stub.screen == "downloads")
        return std::unique_ptr<Screen>(new DownloadsScreen(services));
    if (stub.games.empty())
        return std::unique_ptr<Screen>(new GoneScreen(stub.name));
    return std::unique_ptr<Screen>(new DownloadScreen(stub, services));
}
} // namespace ps5::remote::ui
