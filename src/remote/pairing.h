/* PS5 RetroArch - pairing: the save sync's server signed in without typing (as RFC 8628's
 * device flow).
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The console asks the server for a code and shows it with an address (as a QR code); the
 * player opens that on a phone, signs in to the server as themself and approves; the console,
 * asking now and then, gets a sign-in of its own, which goes into save-sync.json. A backend
 * that can do it has a Pairing in its line of backends.cpp (backends.h); PairingRun runs one.
 */
#pragma once

#include "source.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ps5::remote
{
struct PairingStart
{
    std::string device_code; /* the console's name for the request (asked about by poll) */
    std::string user_code;   /* what the player sees, to tell it is this console's request */
    std::string address;     /* where the player approves it (the QR code), the code in it */
    int expires_in = 0;      /* seconds the request lasts */
    int interval = 5;        /* seconds between polls */
};

enum class PairingState : uint8_t
{
    pending,  /* not yet approved: ask again after `interval` seconds */
    approved, /* `entry` has the sign-in */
    denied,   /* the player said no */
    expired,  /* nobody approved it in time */
    failed,   /* see error */
};

struct PairingResult
{
    PairingState state = PairingState::failed;
    std::string error; /* failed: what went wrong */
    Json entry;        /* approved: what save-sync.json gets ("type", "url", "token"...) */
    std::string user;  /* approved: who it signed in as, when the server says */
    int interval = 0;  /* pending: the seconds to wait now (more when it asked too often) */
};

struct Pairing
{
    /* A request for a code from the server an entry names (its "url"). folder: the store's
     * own (backends.h), for what the backend keeps about the console. False with *error.
     * stopped: true once it is cancelled; what it sends stops then. */
    bool (*start)(const Json &settings, const std::string &folder, PairingStart *start,
                  std::string *error, const Stopped &stopped);
    /* Whether the request was approved yet. */
    PairingResult (*poll)(const Json &settings, const std::string &folder,
                          const PairingStart &start, const Stopped &stopped);
};

/* A server the save sync can be paired with: a download source's whose backend can pair. */
struct PairServer
{
    std::string name; /* the source's, as the menu shows it */
    std::string url;
    std::string type;
    Json settings; /* its entry in sources.json */
};
/* Those of sources.json (`file`), in its order. */
std::vector<PairServer> pair_servers(const std::string &file);

/* A pairing on a thread of its own, for a screen: the code once the server gave one, then
 * asked until approved, denied or expired, or cancelled. Approved, save-sync.json gets the
 * sign-in (save_config.h: set_save_server). */
class PairingRun
{
  public:
    struct View
    {
        enum Stage : uint8_t
        {
            idle,
            starting, /* asking the server for a code */
            waiting,  /* the code shown, the player to approve it */
            approved,
            denied,
            expired,
            failed,
        } stage = idle;
        std::string code, address;
        double expires = 0; /* when the code ends, as `now` counts */
        std::string user;   /* approved: who it signed in as */
        std::string error;
    };

    /* now: seconds, as the screen counts them; config: save-sync.json; folder: the store's. */
    PairingRun(std::function<double()> now, std::string config, std::string folder);
    ~PairingRun();
    PairingRun(const PairingRun &) = delete;
    PairingRun &operator=(const PairingRun &) = delete;

    /* Pairs with the server `settings` names (a download source's entry: its "url"), by its
     * backend of `type`. */
    void start(const std::string &type, const Json &settings);
    View view();
    void cancel();

  private:
    void join();

    std::function<double()> now_;
    const std::string config_, folder_;
    std::mutex lock_;
    View view_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};
} // namespace ps5::remote
