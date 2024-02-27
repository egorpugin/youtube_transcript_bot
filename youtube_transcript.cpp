/*
c++: 23
deps:
    - org.sw.demo.nlohmann.json.natvis
    - pub.egorpugin.primitives.http
    - org.sw.demo.tgbot
    - org.sw.demo.zeux.pugixml
*/

#include "curl_http_client.h"
#include <pugixml.hpp>
#include <primitives/http.h>
#include <nlohmann/json.hpp>
#include <format>
#include <string>

struct youtube_subscript {
    static inline nlohmann::json cache;
    std::string id;

    auto data() {
        if (!cache[id].empty()) {
            return cache[id];
        }
        HttpRequest req{httpSettings};
        req.url = std::format("https://www.youtube.com/watch?v={}", id);
        auto resp = url_request(req);
        auto &s = resp.response;
        auto p = s.find("\"captionTracks\"");
        if (p == -1) {
            return nlohmann::json{};
        }
        auto e = s.find("}]", p);
        s = std::format("{{{}}}", s.substr(p,(e+2)-p));
        if (cache.size() > 100) {
            cache.clear();
        }
        return cache[id] = nlohmann::json::parse(s)["captionTracks"];
    }
    auto languages() {
        std::vector<std::pair<std::string, std::string>> codes;
        for (auto &&d : data()) {
            auto code = d["languageCode"];
            auto kind = ""s;
            if (d.contains("kind")) {
                kind = d["kind"];
            }
            codes.emplace_back(code, kind);
        }
        return codes;
    }
    std::string url(const std::string &lang, const std::string &kind) {
        for (auto &&d : data()) {
            if (d["languageCode"] == lang && (!d.contains("kind") || d["kind"] == kind)) {
                return d["baseUrl"];
            }
        }
        return {};
    }
    static std::string concat_lang_and_kind(const std::string &lang, const std::string &kind) {
        auto label = lang;
        if (!kind.empty()) {
            if (kind == "asr"s) {
                label += " (auto)";
            } else {
                label += " (unk)";
            }
        }
        return label;
    }
};

// https://telegra.ph/api
struct telegraph {
    static inline constexpr auto page_content_limit = (64 - 1) * 1024;

    std::string name;
    std::string access_token;

    auto base_url() {
        return "https://api.telegra.ph/"s;
    }
    nlohmann::json request(auto &&name, auto && ... args) {
        HttpRequest req{httpSettings};
        req.type = HttpRequest::Post;
        req.url = base_url() + name + "?"s;
        auto f = [&](this auto &&f, auto &&k, auto &&v, auto &&...args) {
            req.data_kv[k] = v;
            if constexpr (sizeof...(args)) {
                f(args...);
            }
        };
        f(args...);
        auto resp = url_request(req);
        if (resp.http_code != 200) {
            throw std::runtime_error{"bad telegraph query"};
        }
        auto j = nlohmann::json::parse(resp.response);
        if (j["ok"] == false) {
            auto err = j["error"].get<std::string>();
            auto fw = "FLOOD_WAIT_"s;
            if (err.starts_with(fw)) {
                auto secs = std::stoi(err.substr(fw.size()));
                std::this_thread::sleep_for(std::chrono::seconds(secs+1));
                return request(name, args...);
            }
            throw std::runtime_error{"bad telegraph query: "s + err};
        }
        return j["result"];
    }
    void create_account() {
        access_token = request("createAccount", "short_name", name)["access_token"];
    }
    std::string create_page(auto &&title, auto &&content) {
        if (access_token.empty()) {
            create_account();
        }
        auto resp = request("createPage", "access_token", access_token, "title", title, "content", content);
        return resp["url"];
    }
};

struct tg_bot : tgbot::bot<curl_http_client> {
    using base = tgbot::bot<curl_http_client>;

    std::string botname;
    std::string botvisiblename;

    static const int default_update_limit = 100;
    static const int default_update_timeout = 10;
    int net_delay_on_error = 1;

public:
    using base::base;

    void init() {
        auto me = api().getMe();
        if (!me.username)
            throw SW_RUNTIME_ERROR("Empty bot name");
        botname = *me.username;
        botvisiblename = me.first_name;
        printf("bot username: %s (%s)\n", me.username->c_str(), me.first_name.c_str());
    }
    tgbot::Integer process_updates(tgbot::Integer offset = 0, tgbot::Integer limit = default_update_limit,
                                   tgbot::Integer timeout = default_update_timeout,
                                   const tgbot::Optional<tgbot::Vector<String>> &allowed_updates = {}) {
        // update timeout here for getUpdates()
        ((curl_http_client &)http_client()).set_timeout(timeout);

        auto updates = api().getUpdates(offset, limit, timeout, allowed_updates);
        for (const auto &item : updates) {
            // if updates come unsorted, we must check this
            if (item.update_id >= offset)
                offset = item.update_id + 1;
            process_update(item);
        }
        return offset;
    }
    void process_update(const tgbot::Update &update) {
        try {
            handle_update(update);
        } catch (std::exception &e) {
            printf("error: %s\n", e.what());

            std::this_thread::sleep_for(std::chrono::seconds(net_delay_on_error));
            if (net_delay_on_error < 30) {
                net_delay_on_error *= 2;
            }
        }
    }
    void long_poll(tgbot::Integer limit = default_update_limit, tgbot::Integer timeout = default_update_timeout,
                   const tgbot::Optional<tgbot::Vector<String>> &allowed_updates = {}) {
        tgbot::Integer offset = 0;
        while (1) {
            try {
                offset = process_updates(offset, limit, timeout, allowed_updates);
            } catch (std::exception &e) {
                printf("error: %s\n", e.what());
                ++offset;
            }
        }
    }
    ///
    virtual void handle_update(const tgbot::Update &update) = 0;
};

struct tg_report_detection_bot : tg_bot {
    using tg_bot::tg_bot;

    void handle_update(const tgbot::Update &update) override {
        if (update.message) {
            handle_message(*update.message);
        }
        if (update.callback_query) {
            handle_callback_query(*update.callback_query);
        }
    }
    void handle_callback_query(const tgbot::CallbackQuery &query) {
        if (!query.message || !query.data) {
            return;
        }
        auto v = split_string(*query.data, ",", true);
        auto &lang = v.at(0);
        auto &kind = v.at(1);
        auto &id = v.at(2);

        youtube_subscript s{id};
        auto url = s.url(lang, kind);
        auto xml = download_file(url);
        pugi::xml_document doc;
        if (!doc.load_string(xml.data())) {
            reply(*query.message, "bad subscript xml");
            return;
        }
        constexpr auto array_len = 2;
        constexpr auto object_len = 2;
        constexpr auto text_quotes_len = 2;
        constexpr auto comma_len = 1;
        constexpr auto colon_len = 1;
        auto maxlen = telegraph::page_content_limit;
        maxlen -= array_len;
        auto count_len = [&](auto &&v) {
            maxlen -= v.size() + text_quotes_len;
            return std::forward<decltype(v)>(v);
        };
        std::vector<nlohmann::json> j;
        j.emplace_back();
        for (auto &&xn : doc.select_nodes(pugi::xpath_query{"/transcript/text"})) {
            auto n = xn.node();
            auto start = n.attribute("start").value();
            auto dur = n.attribute("dur").value();
            auto text = n.first_child().text().get();
            std::string t = text;

            static std::regex repl10{"&#(\\d+);"};
            static std::regex repl16{"&#[xX](\\d+);"};
            auto repl = [&](auto &&r, int base) {
                std::smatch m;
                while (std::regex_search(t, m, r)) {
                    auto code = std::stoi(m[1].str(), 0, base);
                    if (code > 255) {
                        SW_UNIMPLEMENTED;
                    }
                    t = m.prefix().str() + (char)code + m.suffix().str();
                }
            };
            repl(repl10, 10);
            repl(repl16, 16);

            auto ms = std::stof(start);
            auto sec = (int)std::floor(ms);
            auto min = sec / 60;
            auto hr = min / 60;
            auto tm = std::format("{}:{:02}:{:02}", hr, min % 60, sec % 60);

            nlohmann::json time;
            maxlen -= object_len;
            time[count_len("tag"s)] = count_len("a"s);
            maxlen -= colon_len + comma_len;
            time[count_len("attrs"s)][count_len("href"s)] = count_len(std::format("https://youtu.be/{}?t={}", id, sec));
            maxlen -= colon_len + object_len + colon_len;
            time[count_len("children"s)].push_back(count_len(tm));
            maxlen -= colon_len + array_len;

            nlohmann::json line;
            maxlen -= object_len;
            line[count_len("tag"s)] = count_len("p"s);
            maxlen -= colon_len + comma_len;
            line[count_len("children"s)].push_back(count_len(" "s));
            maxlen -= comma_len;
            line[count_len("children"s)].push_back(count_len(t));
            maxlen -= colon_len + array_len;
            if (maxlen < 0) {
                maxlen += telegraph::page_content_limit;
                j.emplace_back();
            }
            j.back().push_back(time);
            j.back().push_back(line);
        }
        try {
            telegraph ph{"mybot"};
            std::vector<std::string> links;
            for (auto &&page : j) {
                links.emplace_back(ph.create_page("subscript for "s + id, page.dump()));
            }
            tgbot::sendMessageRequest req;
            if (query.inline_message_id) {
                req.reply_parameters = tgbot::ReplyParameters{std::stoll(*query.inline_message_id)};
            }
            req.chat_id = query.message->chat->id;
            auto add_with_type = [&](auto &&text, auto &&type, auto && ... args) {
                req.entities.push_back(tgbot::MessageEntity{type, (int)req.text.size(), (int)text.size(), args...});
                req.text += text;
            };
            req.text += "your ";
            add_with_type(youtube_subscript::concat_lang_and_kind(lang,kind), "code");
            req.text += " subscript for ";
            auto add_with_link = [&](auto &&text, auto &&link) {
                add_with_type(text, "text_link", link);
            };
            add_with_link("video"s, "https://www.youtube.com/watch?v="s + id);
            req.text += ":\n";
            for (int i = 0; auto &&l : links) {
                auto s = std::format("page {}", ++i);
                add_with_link(s, l);
                req.text += "\n";
            }
            req.link_preview_options = tgbot::LinkPreviewOptions{true};
            api().sendMessage(req);
        } catch (std::exception &e) {
            reply(*query.message, "error:\n"s + e.what());
        }
    }
    void handle_message(const tgbot::Message &message) {
        if (!message.text || message.text->starts_with("/"sv)) {
            if (1 || message.text && *message.text == "/start") {
                reply(message, "send me a youtube link");
            }
            return;
        }

        std::string id;
        auto &t = *message.text;
        auto anchor = "?v="s;
        auto pos = t.find(anchor);
        if (pos == -1) {
            pos = t.rfind("/");
            if (pos == -1) {
                //reply(message, "can't extract youtube id");
                id = t;
            } else {
                id = t.substr(pos + 1);
            }
        } else {
            auto e = t.find('&', pos);
            id = t.substr(pos + anchor.size(), e == -1 ? e : (e - (pos + anchor.size())));
        }

        youtube_subscript s{id};
        auto langs = s.languages();
        if (langs.empty()) {
            reply(message, "no subscripts");
            return;
        }

        tgbot::sendMessageRequest req;
        req.chat_id = message.chat->id;
        req.text = "select subscript language";
        tgbot::InlineKeyboardMarkup m;
        for (auto &&langs2 : langs | std::views::chunk(3)) {
            decltype(m.inline_keyboard)::value_type v;
            for (auto &&[code,kind] : langs2) {
                v.emplace_back(std::make_unique<tgbot::InlineKeyboardButton>(youtube_subscript::concat_lang_and_kind(code,kind),
                    "", code + "," + kind + "," + id));
            }
            m.inline_keyboard.emplace_back(std::move(v));
        }
        req.reply_markup = std::move(m);
        api().sendMessage(req);
    }
    void reply(auto &&message, auto &&t) {
        tgbot::sendMessageRequest req;
        req.chat_id = message.chat->id;
        req.text = t;
        api().sendMessage(req);
    }
};

int main(int argc, char *argv[]) {
    primitives::http::setupSafeTls();
    curl_http_client client;
    auto bot_token = getenv("BOT_TOKEN");
    auto bot = std::make_unique<tg_report_detection_bot>(bot_token ? bot_token : "", client);
    bot->init();
    bot->long_poll();
    return 0;
}
