/* PS5 RetroArch - a RomM server set up from the WebUI (src/remote/romm/romm_setup.h).
 *
 * Copyright (C) 2026 Mario Reisinger
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "romm_setup.h"

#include "romm_client.h"

#include <memory>

namespace ps5::remote::romm
{
namespace
{
/* RomM's own words for a refusal ("detail"), else what its status means. */
std::string refusal(const Client::Answer &answer)
{
    Json json;
    if (Json::parse(answer.body, &json) && json["detail"].kind == Json::string &&
        !json["detail"].text.empty())
        return "RomM says: " + json["detail"].text;
    return "RomM answered with status " + std::to_string(answer.status);
}

bool probe_address(const std::string &url, ServerProbe *probe, std::string *error)
{
    Json settings = Json::record();
    settings.set("url", Json::of(url));
    std::unique_ptr<Client> client = Client::make(settings, "the address", error);
    if (!client)
    {
        *error = "Enter the server's address, such as http://192.168.1.20:3000";
        return false;
    }
    *probe = ServerProbe();
    probe->url = client->url();
    probe->needed = minimum_version;
    std::string why;
    if (client->check_version({}, &why, 10))
    {
        probe->version = client->version();
        return true;
    }
    if (!client->too_old().empty())
    {
        probe->version = client->too_old();
        probe->too_old = true;
        *error = why;
        return false;
    }
    *error = "No RomM answers at " + probe->url + " (" + why + ")";
    return false;
}

bool sign_in_account(const std::string &url, const std::string &name, const std::string &password,
                     const std::string &folder, Json *entry, std::string *user, std::string *error)
{
    Json settings = Json::record();
    settings.set("url", Json::of(url));
    settings.set("username", Json::of(name));
    settings.set("password", Json::of(password));
    std::unique_ptr<Client> client = Client::make(settings, "the sign-in", error);
    if (!client || !client->check_version({}, error, 15))
        return false;
    /* The console's token of an earlier setup goes: RomM keeps a few per user. */
    const std::string token_name = "PS5 RetroArch " + console_id(folder);
    Client::Answer answer;
    if (!client->send(nullptr, "/api/client-tokens", {}, &answer, error))
        return false;
    if (answer.status == 401 || answer.status == 403)
    {
        *error = "RomM did not accept the name or password";
        return false;
    }
    Json tokens;
    if (answer.status == 200 && Json::parse(answer.body, &tokens))
        for (const Json &token : tokens.items)
            if (token["name"].str() == token_name && !token["id"].str().empty())
            {
                Client::Answer gone;
                std::string why;
                (void)client->send("DELETE", "/api/client-tokens/" + token["id"].str(), {}, &gone,
                                   &why);
            }
    Json payload = Json::record();
    payload.set("name", Json::of(token_name));
    Json &scopes = payload.set("scopes", Json::list());
    for (const std::string &scope : console_scopes())
        scopes.push(Json::of(scope));
    if (!client->send("POST", "/api/client-tokens", payload.write(), &answer, error))
        return false;
    Json made;
    Json::parse(answer.body, &made);
    if ((answer.status != 200 && answer.status != 201) || made["raw_token"].str().empty())
    {
        *error = answer.status == 403
                     ? "This RomM account may not do all the console needs (" + refusal(answer) +
                           "): give it the role editor"
                     : "RomM made no sign-in for the console (" + refusal(answer) + ")";
        return false;
    }
    *entry = Json::record();
    entry->set("url", Json::of(client->url()));
    entry->set("token", Json::of(made["raw_token"].str()));
    /* Who it is, as RomM spells it. */
    *user = name;
    if (client->send(nullptr, "/api/users/me", {}, &answer, error) && answer.status == 200)
    {
        Json me;
        if (Json::parse(answer.body, &me) && !me["username"].str().empty())
            *user = me["username"].str();
    }
    error->clear();
    return true;
}
} // namespace

const ServerSignIn sign_in = {probe_address, sign_in_account};
} // namespace ps5::remote::romm
