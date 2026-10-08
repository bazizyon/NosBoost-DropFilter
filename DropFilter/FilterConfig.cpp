#define NOMINMAX
#include <windows.h>

#include "FilterConfig.h"
#include <algorithm>
#include <fstream>
#include <sstream>

namespace FilterConfig {
    namespace {
        constexpr wchar_t ConfigExtension[] = L".txt";

        std::wstring ModsFolder() {
            wchar_t Path[MAX_PATH];
            GetModuleFileNameW(nullptr, Path, MAX_PATH);
            const std::wstring Exe(Path);
            return Exe.substr(0, Exe.find_last_of(L"\\/") + 1) + L"mods\\";
        }

        std::wstring ConfigFolder() {
            return ModsFolder() + L"DropFilter\\";
        }

        std::wstring StatePath() {
            return ModsFolder() + L"DropFilter.txt";
        }

        std::string ToUtf8(const std::wstring& Text) {
            if (Text.empty()) return {};
            const int Size = WideCharToMultiByte(CP_UTF8, 0, Text.c_str(), static_cast<int>(Text.size()), nullptr, 0, nullptr, nullptr);
            std::string Result(Size, '\0');
            WideCharToMultiByte(CP_UTF8, 0, Text.c_str(), static_cast<int>(Text.size()), Result.data(), Size, nullptr, nullptr);
            return Result;
        }

        std::wstring FromUtf8(const std::string& Text) {
            if (Text.empty()) return {};
            const int Size = MultiByteToWideChar(CP_UTF8, 0, Text.c_str(), static_cast<int>(Text.size()), nullptr, 0);
            std::wstring Result(Size, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, Text.c_str(), static_cast<int>(Text.size()), Result.data(), Size);
            return Result;
        }

        const char* ModeName(const Mode ListMode) {
            return ListMode == Mode::Whitelist ? "whitelist" : "blacklist";
        }

        void WriteConfig(std::ostringstream& Out, const Config& Config) {
            Out << "mode=" << ModeName(Config.ListMode) << "\n";
            for (const Entry& Item : Config.Entries) {
                Out << "item=" << Item.Vnum << "\t" << ToUtf8(Item.Name) << "\n";
            }
        }

        bool WriteFile(const std::wstring& Path, const std::string& Text) {
            const std::wstring Temp = Path + L".tmp";
            {
                std::ofstream File(Temp, std::ios::binary | std::ios::trunc);
                if (!File) return false;
                File << Text;
                if (!File) return false;
            }
            return MoveFileExW(Temp.c_str(), Path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
        }

        template <typename OtherFn>
        bool ReadFile(const std::wstring& Path, Config& Config, OtherFn&& OnOther) {
            std::ifstream File(Path, std::ios::binary);
            if (!File) return false;
            std::string Line;
            while (std::getline(File, Line)) {
                if (!Line.empty() && Line.back() == '\r') Line.pop_back();
                const size_t Equals = Line.find('=');
                if (Line.empty() || Line[0] == '#' || Equals == std::string::npos) continue;
                const std::string Key = Line.substr(0, Equals);
                const std::string Value = Line.substr(Equals + 1);
                if (Key == "mode") {
                    Config.ListMode = Value == "whitelist" ? Mode::Whitelist : Mode::Blacklist;
                } else if (Key == "item") {
                    const size_t Tab = Value.find('\t');
                    Entry Item;
                    Item.Vnum = std::atoi(Value.substr(0, Tab).c_str());
                    if (Tab != std::string::npos) Item.Name = FromUtf8(Value.substr(Tab + 1));
                    const bool Duplicate = std::any_of(Config.Entries.begin(), Config.Entries.end(),
                                                       [&](const Entry& Other) { return Other.Vnum == Item.Vnum; });
                    if (Item.Vnum > 0 && !Duplicate) Config.Entries.push_back(std::move(Item));
                } else {
                    OnOther(Key, Value);
                }
            }
            return true;
        }
    }

    std::vector<std::wstring> List() {
        std::vector<std::wstring> Names;
        WIN32_FIND_DATAW Found{};
        const HANDLE Search = FindFirstFileW((ConfigFolder() + L"*" + ConfigExtension).c_str(), &Found);
        if (Search == INVALID_HANDLE_VALUE) return Names;
        do {
            if (Found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring Name = Found.cFileName;
            Names.push_back(Name.substr(0, Name.size() - (std::size(ConfigExtension) - 1)));
        } while (FindNextFileW(Search, &Found));
        FindClose(Search);
        std::sort(Names.begin(), Names.end(), [](const std::wstring& A, const std::wstring& B) {
            return _wcsicmp(A.c_str(), B.c_str()) < 0;
        });
        return Names;
    }

    bool IsValidName(const std::wstring& Name) {
        if (Name.empty() || Name.size() > 40) return false;
        if (Name.front() == L' ' || Name.back() == L' ' || Name.back() == L'.') return false;
        return Name.find_first_of(L"\\/:*?\"<>|\t") == std::wstring::npos;
    }

    bool Save(const std::wstring& Name, const Config& Config) {
        if (!IsValidName(Name)) return false;
        CreateDirectoryW(ConfigFolder().c_str(), nullptr);
        std::ostringstream Out;
        Out << "# NosBoost DropFilter config\n";
        WriteConfig(Out, Config);
        return WriteFile(ConfigFolder() + Name + ConfigExtension, Out.str());
    }

    std::optional<Config> Load(const std::wstring& Name) {
        if (!IsValidName(Name)) return std::nullopt;
        Config Result;
        if (!ReadFile(ConfigFolder() + Name + ConfigExtension, Result, [](const std::string&, const std::string&) {})) {
            return std::nullopt;
        }
        return Result;
    }

    void SaveState(const State& State) {
        std::ostringstream Out;
        Out << "# NosBoost DropFilter: what is active right now\n";
        Out << "enabled=" << (State.Enabled ? 1 : 0) << "\n";
        Out << "config=" << ToUtf8(State.ActiveConfig) << "\n";
        WriteConfig(Out, State.Current);
        WriteFile(StatePath(), Out.str());
    }

    State LoadState() {
        State Result;
        ReadFile(StatePath(), Result.Current, [&](const std::string& Key, const std::string& Value) {
            if (Key == "enabled") Result.Enabled = Value != "0";
            else if (Key == "config") Result.ActiveConfig = FromUtf8(Value);
        });
        return Result;
    }
}
