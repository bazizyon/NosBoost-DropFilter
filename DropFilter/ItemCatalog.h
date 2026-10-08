#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ItemCatalog {
    struct Item {
        int Vnum = 0;
        int16_t Icon = -1;
        std::wstring Name;
    };

    const std::vector<Item>& All();
    const Item* Find(int Vnum);
    bool Load();
}
