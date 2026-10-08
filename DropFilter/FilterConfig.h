#pragma once
#include <optional>
#include <string>
#include <vector>

namespace FilterConfig {
    enum class Mode {
        Blacklist,
        Whitelist,
    };

    struct Entry {
        int Vnum = 0;
        std::wstring Name;
    };

    struct Config {
        Mode ListMode = Mode::Blacklist;
        std::vector<Entry> Entries;
    };

    struct State {
        bool Enabled = true;
        std::wstring ActiveConfig;
        Config Current;
    };

    std::vector<std::wstring> List();
    bool IsValidName(const std::wstring& Name);
    bool Save(const std::wstring& Name, const Config& Config);
    std::optional<Config> Load(const std::wstring& Name);

    void SaveState(const State& State);
    State LoadState();
}
