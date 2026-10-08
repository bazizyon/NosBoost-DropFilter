#define NOMINMAX
#include <windows.h>

#include "ItemCatalog.h"
#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace ItemCatalog {
    namespace {
        constexpr char ListClassName[] = "TNTCItemDataList";
        constexpr uint32_t VmtSelfPtr = 0x4C;
        constexpr uint32_t VmtClassName = 0x2C;
        constexpr uint32_t ListItems = 0x04;
        constexpr uint32_t ListCount = 0x08;
        constexpr uint32_t RecordVnum = 0x30;
        constexpr uint32_t RecordName = 0x38;
        constexpr uint32_t RecordIcon = 0x4A;
        constexpr int MaxItems = 100000;

        std::vector<Item> Items;
        std::unordered_map<int, size_t> ByVnum;

        struct Image {
            const uint8_t* Start = nullptr;
            size_t Size = 0;
        };

        Image GameImage() {
            const auto Base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
            const auto* Dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(Base);
            const auto* Nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(Base + Dos->e_lfanew);
            return {Base, Nt->OptionalHeader.SizeOfImage};
        }

        size_t ReadableSpan(const void* Address) {
            MEMORY_BASIC_INFORMATION Info{};
            if (!Address || !VirtualQuery(Address, &Info, sizeof(Info)) || Info.State != MEM_COMMIT) return 0;
            if (Info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
            const auto End = static_cast<const uint8_t*>(Info.BaseAddress) + Info.RegionSize;
            return static_cast<size_t>(End - static_cast<const uint8_t*>(Address));
        }

        bool Readable(const void* Address, const size_t Size) {
            return ReadableSpan(Address) >= Size;
        }

        uint32_t Dword(const uint8_t* At) {
            uint32_t Value;
            std::memcpy(&Value, At, sizeof(Value));
            return Value;
        }

        const uint8_t* FindBytes(const Image& Area, const uint8_t* Bytes, const size_t Length, const uint8_t* From) {
            const uint8_t* End = Area.Start + Area.Size;
            const uint8_t* At = From;
            while (At < End) {
                MEMORY_BASIC_INFORMATION Info{};
                if (!VirtualQuery(At, &Info, sizeof(Info))) return nullptr;
                const uint8_t* RegionEnd = std::min(End, static_cast<const uint8_t*>(Info.BaseAddress) + Info.RegionSize);
                const bool Usable = Info.State == MEM_COMMIT && !(Info.Protect & (PAGE_NOACCESS | PAGE_GUARD));
                if (Usable) {
                    for (; At + Length <= RegionEnd; At++) {
                        if (*At == Bytes[0] && std::memcmp(At, Bytes, Length) == 0) return At;
                    }
                }
                At = RegionEnd;
            }
            return nullptr;
        }

        uintptr_t FindListVmt(const Image& Area) {
            uint8_t Name[sizeof(ListClassName)];
            Name[0] = static_cast<uint8_t>(sizeof(ListClassName) - 1);
            std::memcpy(Name + 1, ListClassName, sizeof(ListClassName) - 1);
            for (const uint8_t* Text = FindBytes(Area, Name, sizeof(Name), Area.Start); Text;
                 Text = FindBytes(Area, Name, sizeof(Name), Text + 1)) {
                const auto TextAddress = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(Text));
                const auto* Needle = reinterpret_cast<const uint8_t*>(&TextAddress);
                for (const uint8_t* Ref = FindBytes(Area, Needle, 4, Area.Start); Ref;
                     Ref = FindBytes(Area, Needle, 4, Ref + 1)) {
                    const uintptr_t Vmt = reinterpret_cast<uintptr_t>(Ref) + VmtClassName;
                    const auto* Self = reinterpret_cast<const uint8_t*>(Vmt - VmtSelfPtr);
                    if (Readable(Self, 4) && Dword(Self) == Vmt) return Vmt;
                }
            }
            return 0;
        }

        uintptr_t FindListGlobal(const Image& Area, const uintptr_t Vmt) {
            const auto SelfPtr = static_cast<uint32_t>(Vmt - VmtSelfPtr);
            uint8_t Load[5] = {0xA1};
            std::memcpy(Load + 1, &SelfPtr, 4);
            for (const uint8_t* At = FindBytes(Area, Load, sizeof(Load), Area.Start); At;
                 At = FindBytes(Area, Load, sizeof(Load), At + 1)) {
                if (!Readable(At, 15) || At[5] != 0xE8 || At[10] != 0xA3) continue;
                return Dword(At + 11);
            }
            return 0;
        }

        uintptr_t FindList() {
            static uintptr_t Global = 0;
            static uintptr_t Vmt = 0;
            if (!Global) {
                const Image Area = GameImage();
                Vmt = FindListVmt(Area);
                if (Vmt) Global = FindListGlobal(Area, Vmt);
                if (!Global) return 0;
            }
            const auto* Slot = reinterpret_cast<const uint8_t*>(Global);
            if (!Readable(Slot, 4)) return 0;
            const uintptr_t List = Dword(Slot);
            const auto* Object = reinterpret_cast<const uint8_t*>(List);
            if (!List || !Readable(Object, 0x10) || Dword(Object) != Vmt) return 0;
            return List;
        }

        std::wstring Widen(const char* Text, const size_t Length) {
            if (!Length) return {};
            const int Size = MultiByteToWideChar(CP_ACP, 0, Text, static_cast<int>(Length), nullptr, 0);
            std::wstring Result(Size, L'\0');
            MultiByteToWideChar(CP_ACP, 0, Text, static_cast<int>(Length), Result.data(), Size);
            return Result;
        }

        bool ReadRecord(const uint8_t* Record, Item& Out) {
            if (!Readable(Record, RecordIcon + 2)) return false;
            Out.Vnum = static_cast<int>(Dword(Record + RecordVnum));
            int16_t Icon;
            std::memcpy(&Icon, Record + RecordIcon, sizeof(Icon));
            Out.Icon = Icon;
            const auto* Name = reinterpret_cast<const char*>(Dword(Record + RecordName));
            const size_t Limit = std::min<size_t>(256, ReadableSpan(Name));
            size_t Length = 0;
            while (Length < Limit && Name[Length]) Length++;
            Out.Name = Widen(Name, Length);
            return true;
        }
    }

    bool Load() {
        const uintptr_t List = FindList();
        if (!List) return false;
        const auto* Object = reinterpret_cast<const uint8_t*>(List);
        const auto* Entries = reinterpret_cast<const uint8_t*>(Dword(Object + ListItems));
        const int Count = static_cast<int>(Dword(Object + ListCount));
        if (Count <= 0 || Count > MaxItems || !Readable(Entries, Count * 4)) return false;

        std::vector<Item> Loaded;
        Loaded.reserve(Count);
        for (int i = 0; i < Count; i++) {
            Item Entry;
            if (!ReadRecord(reinterpret_cast<const uint8_t*>(Dword(Entries + i * 4)), Entry)) continue;
            if (Entry.Name.empty()) continue;
            Loaded.push_back(std::move(Entry));
        }
        std::stable_sort(Loaded.begin(), Loaded.end(), [](const Item& A, const Item& B) {
            return _wcsicmp(A.Name.c_str(), B.Name.c_str()) < 0;
        });

        Items = std::move(Loaded);
        ByVnum.clear();
        for (size_t i = 0; i < Items.size(); i++) ByVnum[Items[i].Vnum] = i;
        return !Items.empty();
    }

    const std::vector<Item>& All() {
        return Items;
    }

    const Item* Find(const int Vnum) {
        const auto Found = ByVnum.find(Vnum);
        return Found == ByVnum.end() ? nullptr : &Items[Found->second];
    }
}
