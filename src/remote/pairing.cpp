/* PS5 RetroArch - a pairing on a thread of its own (src/remote/pairing.h).
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "pairing.h"

#include "backends.h"
#include "files.h"
#include "save_config.h"

#include <chrono>
#include <utility>

namespace ps5::remote
{
std::vector<PairServer> pair_servers(const std::string &file)
{
    std::vector<PairServer> servers;
    std::string content;
    Json document;
    if (!files::read(file, &content) || !Json::parse(content, &document))
        return servers;
    for (const Json &entry : document["sources"].items)
    {
        const std::string type = entry["type"].str();
        const Backend *backend = find_backend(type);
        if (backend == nullptr || backend->pairing == nullptr || entry["url"].str().empty())
            continue;
        PairServer server;
        server.name = !entry["name"].str().empty() ? entry["name"].str() : entry["url"].str();
        server.url = entry["url"].str();
        server.type = type;
        server.settings = entry;
        servers.push_back(std::move(server));
    }
    return servers;
}

PairingRun::PairingRun(std::function<double()> now, std::string config, std::string folder)
    : now_(std::move(now)), config_(std::move(config)), folder_(std::move(folder))
{
}

PairingRun::~PairingRun()
{
    cancel();
}

void PairingRun::join()
{
    if (thread_.joinable())
        thread_.join();
}

void PairingRun::cancel()
{
    stop_ = true;
    join();
    std::lock_guard<std::mutex> guard(lock_);
    if (view_.stage == View::starting || view_.stage == View::waiting)
        view_ = View();
}

PairingRun::View PairingRun::view()
{
    std::lock_guard<std::mutex> guard(lock_);
    return view_;
}

void PairingRun::start(const std::string &type, const Json &settings)
{
    cancel();
    const Backend *backend = find_backend(type);
    {
        std::lock_guard<std::mutex> guard(lock_);
        view_ = View();
        if (backend == nullptr || backend->pairing == nullptr)
        {
            view_.stage = View::failed;
            view_.error = "This server cannot be paired with: fill in save-sync.json instead";
            return;
        }
        view_.stage = View::starting;
    }
    stop_ = false;
    const Pairing pairing = *backend->pairing;
    thread_ = std::thread(
        [this, pairing, settings]
        {
            const Stopped stopped = [this] { return stop_.load(); };
            const auto set = [this](const View &view)
            {
                std::lock_guard<std::mutex> guard(lock_);
                view_ = view;
            };
            View view;
            PairingStart request;
            std::string error;
            if (!pairing.start(settings, folder_, &request, &error, stopped))
            {
                if (stop_)
                    return;
                view.stage = View::failed;
                view.error = error;
                set(view);
                return;
            }
            view.stage = View::waiting;
            view.code = request.user_code;
            view.address = request.address;
            view.expires = now_() + request.expires_in;
            set(view);
            int interval = request.interval;
            const auto ends =
                std::chrono::steady_clock::now() + std::chrono::seconds(request.expires_in);
            while (!stop_)
            {
                /* The interval in small steps, so that cancelling does not wait for it. */
                const auto next = std::chrono::steady_clock::now() + std::chrono::seconds(interval);
                while (!stop_ && std::chrono::steady_clock::now() < next)
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                if (stop_)
                    return;
                request.interval = interval;
                const PairingResult result = pairing.poll(settings, folder_, request, stopped);
                if (stop_)
                    return;
                if (result.state == PairingState::pending)
                {
                    interval = result.interval > 0 ? result.interval : interval;
                    if (std::chrono::steady_clock::now() < ends)
                        continue;
                    view.stage = View::expired;
                }
                else if (result.state == PairingState::approved)
                {
                    Json fields = result.entry;
                    fields.set("__server_user", Json::of(result.user));
                    if (set_save_server(config_, fields))
                    {
                        view.stage = View::approved;
                        view.user = result.user;
                    }
                    else
                    {
                        view.stage = View::failed;
                        view.error = "save-sync.json could not be written";
                    }
                }
                else
                {
                    view.stage = result.state == PairingState::denied    ? View::denied
                                 : result.state == PairingState::expired ? View::expired
                                                                         : View::failed;
                    view.error = result.error;
                }
                set(view);
                return;
            }
        });
}
} // namespace ps5::remote
