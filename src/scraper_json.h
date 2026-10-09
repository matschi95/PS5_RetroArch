/* Copyright (C) 2026 Mihawk; SPDX-License-Identifier: GPL-3.0-or-later */
/* A small JSON tree for the scraper's sources (ScreenScraper answers in JSON) and the
 * download sources (src/remote/): values are read whole into a tree, malformed input
 * reads as null. An object keeps its members' order, so a file a player edits by hand
 * (config/remote/save-sync.json) is written back in the order it was written in. */
#pragma once
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ps5_scraper
{
struct Json
{
    enum Kind
    {
        null,
        boolean,
        number,
        string,
        array,
        object
    } kind = null;
    bool flag = false;
    double value = 0;
    std::string text;
    std::vector<Json> items;
    std::map<std::string, Json> fields;
    std::vector<std::string> order; /* an object's keys, in the order read or set */

    /* A field, or a null value when there is none. */
    const Json &operator[](const std::string &key) const
    {
        static const Json none;
        auto it = fields.find(key);
        return it == fields.end() ? none : it->second;
    }
    /* A string, or a number written as text ("" for anything else). */
    std::string str() const
    {
        if (kind == string)
            return text;
        if (kind == number)
        {
            char out[32];
            std::snprintf(out, sizeof out, "%.15g", value);
            return out;
        }
        return "";
    }

    static Json parse(const std::string &input)
    {
        Json out;
        return parse(input, &out) ? out : Json();
    }
    /* The same, saying whether the whole input is JSON (null is not malformed input). */
    static bool parse(const std::string &input, Json *out)
    {
        size_t at = 0;
        Json value;
        if (!read(input, at, value, 0))
            return false;
        space(input, at);
        if (at != input.size())
            return false;
        *out = std::move(value);
        return true;
    }

    /* A number as an unsigned integer (a string of digits too); 0 for anything else. */
    unsigned long long whole() const
    {
        if (kind == number)
            return value > 0 ? static_cast<unsigned long long>(value) : 0;
        if (kind == string && !text.empty() &&
            text.find_first_not_of("0123456789") == std::string::npos)
            return std::strtoull(text.c_str(), nullptr, 10);
        return 0;
    }
    /* true for true; false for anything else. */
    bool yes() const
    {
        return kind == boolean && flag;
    }

    /* ---- building and writing ---- */
    static Json of(const std::string &text)
    {
        Json out;
        out.kind = string;
        out.text = text;
        return out;
    }
    static Json of(const char *text)
    {
        return of(std::string(text ? text : ""));
    }
    static Json of(double number)
    {
        Json out;
        out.kind = Json::number;
        out.value = number;
        return out;
    }
    static Json of(bool truth)
    {
        Json out;
        out.kind = boolean;
        out.flag = truth;
        return out;
    }
    static Json list()
    {
        Json out;
        out.kind = array;
        return out;
    }
    static Json record()
    {
        Json out;
        out.kind = object;
        return out;
    }
    /* Sets a member (it keeps its place when it is there), making this an object. */
    Json &set(const std::string &key, Json member)
    {
        kind = object;
        auto it = fields.find(key);
        if (it == fields.end())
            order.push_back(key);
        Json &slot = fields[key];
        slot = std::move(member);
        return slot;
    }
    void erase(const std::string &key)
    {
        if (fields.erase(key))
            for (size_t i = 0; i < order.size(); i++)
                if (order[i] == key)
                {
                    order.erase(order.begin() + long(i));
                    break;
                }
    }
    bool has(const std::string &key) const
    {
        return fields.count(key) != 0;
    }
    /* Adds an element, making this an array. */
    Json &push(Json element)
    {
        kind = array;
        items.push_back(std::move(element));
        return items.back();
    }
    /* As JSON text: indented by two spaces a level when pretty, else on one line. */
    std::string write(bool pretty = false) const
    {
        std::string out;
        write(out, pretty, 0);
        if (pretty)
            out += '\n';
        return out;
    }
    /* text as a JSON string, quotes included. */
    static std::string quote(const std::string &text)
    {
        std::string out = "\"";
        for (unsigned char c : text)
        {
            if (c == '"' || c == '\\')
            {
                out += '\\';
                out += char(c);
            }
            else if (c < 32)
            {
                char escape[7];
                std::snprintf(escape, sizeof escape, "\\u%04x", c);
                out += escape;
            }
            else
                out += char(c);
        }
        return out + '"';
    }
    bool operator==(const Json &other) const
    {
        return write() == other.write();
    }
    bool operator!=(const Json &other) const
    {
        return !(*this == other);
    }

  private:
    void write(std::string &out, bool pretty, int depth) const
    {
        const auto line = [&](int level)
        {
            if (pretty)
                out += '\n' + std::string(size_t(level) * 2, ' ');
        };
        switch (kind)
        {
        case null:
            out += "null";
            return;
        case boolean:
            out += flag ? "true" : "false";
            return;
        case number:
            out += str();
            return;
        case string:
            out += quote(text);
            return;
        case array:
            out += '[';
            for (size_t i = 0; i < items.size(); i++)
            {
                out += i ? "," : "";
                line(depth + 1);
                items[i].write(out, pretty, depth + 1);
            }
            if (!items.empty())
                line(depth);
            out += ']';
            return;
        case object:
            out += '{';
            for (size_t i = 0; i < order.size(); i++)
            {
                out += i ? "," : "";
                line(depth + 1);
                out += quote(order[i]) + (pretty ? ": " : ":");
                fields.at(order[i]).write(out, pretty, depth + 1);
            }
            if (!order.empty())
                line(depth);
            out += '}';
            return;
        }
    }
    static void space(const std::string &s, size_t &at)
    {
        while (at < s.size() && (s[at] == ' ' || s[at] == '\t' || s[at] == '\n' || s[at] == '\r'))
            ++at;
    }
    static void utf8(std::string &out, unsigned code)
    {
        if (code < 0x80)
            out += char(code);
        else if (code < 0x800)
            out += char(0xC0 | code >> 6), out += char(0x80 | (code & 0x3F));
        else if (code < 0x10000)
            out += char(0xE0 | code >> 12), out += char(0x80 | (code >> 6 & 0x3F)),
                out += char(0x80 | (code & 0x3F));
        else
            out += char(0xF0 | code >> 18), out += char(0x80 | (code >> 12 & 0x3F)),
                out += char(0x80 | (code >> 6 & 0x3F)), out += char(0x80 | (code & 0x3F));
    }
    static bool read_string(const std::string &s, size_t &at, std::string &out)
    {
        if (at >= s.size() || s[at] != '"')
            return false;
        ++at;
        while (at < s.size() && s[at] != '"')
        {
            char c = s[at++];
            if (c != '\\')
            {
                out += c;
                continue;
            }
            if (at >= s.size())
                return false;
            c = s[at++];
            switch (c)
            {
            case 'n':
                out += '\n';
                break;
            case 't':
                out += '\t';
                break;
            case 'r':
                out += '\r';
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'u':
            {
                if (at + 4 > s.size())
                    return false;
                unsigned code = unsigned(std::strtoul(s.substr(at, 4).c_str(), nullptr, 16));
                at += 4;
                /* A surrogate pair is one character. */
                if (code >= 0xD800 && code < 0xDC00 && at + 6 <= s.size() && s[at] == '\\' &&
                    s[at + 1] == 'u')
                {
                    const unsigned low =
                        unsigned(std::strtoul(s.substr(at + 2, 4).c_str(), nullptr, 16));
                    if (low >= 0xDC00 && low < 0xE000)
                    {
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                        at += 6;
                    }
                }
                utf8(out, code);
                break;
            }
            default:
                out += c; /* \" \\ \/ */
            }
        }
        if (at >= s.size())
            return false;
        ++at;
        return true;
    }
    static bool read(const std::string &s, size_t &at, Json &out, int depth)
    {
        if (depth > 64)
            return false;
        space(s, at);
        if (at >= s.size())
            return false;
        const char c = s[at];
        if (c == '{')
        {
            out.kind = object;
            ++at;
            space(s, at);
            if (at < s.size() && s[at] == '}')
                return ++at, true;
            for (;;)
            {
                std::string key;
                space(s, at);
                if (!read_string(s, at, key))
                    return false;
                space(s, at);
                if (at >= s.size() || s[at++] != ':')
                    return false;
                if (!out.fields.count(key))
                    out.order.push_back(key);
                Json &member = out.fields[key];
                member = Json();
                if (!read(s, at, member, depth + 1))
                    return false;
                space(s, at);
                if (at < s.size() && s[at] == ',')
                {
                    ++at;
                    continue;
                }
                return at < s.size() && s[at++] == '}';
            }
        }
        if (c == '[')
        {
            out.kind = array;
            ++at;
            space(s, at);
            if (at < s.size() && s[at] == ']')
                return ++at, true;
            for (;;)
            {
                out.items.emplace_back();
                if (!read(s, at, out.items.back(), depth + 1))
                    return false;
                space(s, at);
                if (at < s.size() && s[at] == ',')
                {
                    ++at;
                    continue;
                }
                return at < s.size() && s[at++] == ']';
            }
        }
        if (c == '"')
        {
            out.kind = string;
            return read_string(s, at, out.text);
        }
        if (s.compare(at, 4, "true") == 0)
            return out.kind = boolean, out.flag = true, at += 4, true;
        if (s.compare(at, 5, "false") == 0)
            return out.kind = boolean, at += 5, true;
        if (s.compare(at, 4, "null") == 0)
            return at += 4, true;
        char *end = nullptr;
        out.value = std::strtod(s.c_str() + at, &end);
        if (end == s.c_str() + at)
            return false;
        out.kind = number;
        at = size_t(end - s.c_str());
        return true;
    }
};
} // namespace ps5_scraper
