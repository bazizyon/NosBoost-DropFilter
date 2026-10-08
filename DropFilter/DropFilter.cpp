#define NOMINMAX
#include <windows.h>

#include "ModContract.h"
#include "WidgetKit.h"
#include "TLBSWidget.h"
#include "TEWLabel.h"
#include "TEWEditWidget.h"
#include "TEWButtonWidget.h"
#include "TEWCustomPanelWidget.h"
#include "TEWGraphicButtonWidget.h"
#include "TNTIconWidget.h"
#include "DropPacket.h"
#include "InPacket.h"
#include "OutPacket.h"
#include "GetPacket.h"
#include "CMapPacket.h"
#include "FilterConfig.h"
#include "ItemCatalog.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace {
    constexpr ModClassRequirement Requirements[] = {
        {TLBSWidget::ClassName, TLBSWidget::Version, TLBSWidget::ExpectedSize},
        {TEWCustomPanelWidget::ClassName, TEWCustomPanelWidget::Version, TEWCustomPanelWidget::ExpectedSize},
        {TEWGraphicButtonWidget::ClassName, TEWGraphicButtonWidget::Version, TEWGraphicButtonWidget::ExpectedSize},
        {TEWButtonWidget::ClassName, TEWButtonWidget::Version, TEWButtonWidget::ExpectedSize},
        {TEWEditWidget::ClassName, TEWEditWidget::Version, TEWEditWidget::ExpectedSize},
        {TEWLabel::ClassName, TEWLabel::Version, TEWLabel::ExpectedSize},
        {TNTIconWidget::ClassName, TNTIconWidget::Version, TNTIconWidget::ExpectedSize},
    };

    constexpr uint16_t WindowWidth = 300;
    constexpr uint16_t WindowHeight = 480;
    constexpr int16_t Pad = 14;
    constexpr int16_t ContentWidth = WindowWidth - 2 * Pad;
    constexpr int16_t DropdownArrow = 17;
    constexpr int16_t TextNudge = -1;
    constexpr int16_t OptionsTop = 42;
    constexpr int16_t HintTop = 64;
    constexpr int16_t LoadTop = 88;
    constexpr int16_t SaveTop = 112;
    constexpr int16_t PickerTop = 164;
    constexpr int16_t ListTop = 190;
    constexpr int16_t RowHeight = 22;
    constexpr int VisibleRows = 10;
    constexpr int16_t PagerTop = ListTop + VisibleRows * RowHeight + 2;
    constexpr int16_t StatusTop = PagerTop + 24;
    constexpr int16_t IconSize = 18;
    constexpr int16_t RemoveSize = 20;
    constexpr int16_t RemoveX = WindowWidth - Pad - RemoveSize;
    constexpr int16_t NameX = Pad + IconSize + 6;
    constexpr int16_t NameWidth = RemoveX - 4 - NameX;

    const Color PlainColor(255, 255, 255, 255);
    const Color HintColor(255, 190, 190, 190);
    const Color RenamedColor(255, 255, 110, 90);
    const Color MissingColor(255, 150, 150, 150);
    const Color GoodColor(255, 140, 220, 120);

    const ModHost* CachedHost = nullptr;
    TLBSWidget* AttachedRoot = nullptr;

    FilterConfig::State Settings;

    std::mutex FilterLock;
    bool FilterEnabled = true;
    FilterConfig::Mode FilterMode = FilterConfig::Mode::Blacklist;
    std::unordered_set<int> FilterVnums;
    std::unordered_set<long> HiddenDrops;
    std::atomic<int> HiddenCount{0};

    enum class EntryState { Ok, Renamed, Missing };

    struct ListRow {
        TNTIconWidget* Icon = nullptr;
        TEWLabel* Name = nullptr;
        TEWGraphicButtonWidget* Remove = nullptr;
        int Slot = 0;
    };

    TEWCustomPanelWidget* Window = nullptr;
    bool EnabledValue = true;
    TLBSWidget* EnabledBox = nullptr;
    WidgetKit::Dropdown* ModePicker = nullptr;
    TEWLabel* ModeHint = nullptr;
    WidgetKit::Dropdown* ConfigPicker = nullptr;
    WidgetKit::EditBox NameBox{};
    WidgetKit::Dropdown* ItemPicker = nullptr;
    ListRow Rows[VisibleRows];
    TEWLabel* EmptyHint = nullptr;
    TEWGraphicButtonWidget* PrevPage = nullptr;
    TEWGraphicButtonWidget* NextPage = nullptr;
    TEWLabel* PageLabel = nullptr;
    TEWLabel* StatusLines[2] = {};
    TEWLabel* HiddenLabel = nullptr;
    int ShownHidden = -1;
    int ListScroll = 0;
    bool PendingToggle = false;
    bool CatalogReady = false;
    std::vector<int> PickerVnums;
    std::vector<std::wstring> ConfigNames;

    void RebuildFilter() {
        std::lock_guard Lock(FilterLock);
        FilterEnabled = Settings.Enabled;
        FilterMode = Settings.Current.ListMode;
        FilterVnums.clear();
        for (const auto& Entry : Settings.Current.Entries) FilterVnums.insert(Entry.Vnum);
    }

    bool HideAndRemember(const int Vnum, const long DropId) {
        std::lock_guard Lock(FilterLock);
        if (!FilterEnabled) return false;
        const bool Listed = FilterVnums.contains(Vnum);
        const bool Hide = FilterMode == FilterConfig::Mode::Blacklist ? Listed : !Listed;
        if (Hide) HiddenDrops.insert(DropId);
        return Hide;
    }

    bool OnDrop(const Packet::DropPacket& Packet) {
        if (!HideAndRemember(Packet.itemVNum, Packet.dropID)) return true;
        ++HiddenCount;
        return false;
    }

    bool OnIn(const Packet::InPacket& Packet) {
        if (Packet.entityType != EntityType::Object) return true;
        if (!HideAndRemember(Packet.entityVNum, Packet.entityID)) return true;
        ++HiddenCount;
        return false;
    }

    bool OnOut(const Packet::OutPacket& Packet) {
        if (Packet.entityType != EntityType::Object) return true;
        std::lock_guard Lock(FilterLock);
        return HiddenDrops.erase(Packet.entityID) == 0;
    }

    bool OnPickUp(const Packet::GetPacket& Packet) {
        std::lock_guard Lock(FilterLock);
        return HiddenDrops.erase(Packet.dropID) == 0;
    }

    bool OnMapChange(const Packet::CMapPacket&) {
        std::lock_guard Lock(FilterLock);
        HiddenDrops.clear();
        HiddenCount = 0;
        return true;
    }

    void Attach(TLBSWidget* Parent, TLBSWidget* Child) {
        if (!Parent || !Child) return;
        Child->parent = Parent;
        Parent->childrenList->push_back(Child);
    }

    void Show(TLBSWidget* Widget, const bool Visible) {
        if (Widget && Widget->isVisible != Visible) Widget->isVisible = Visible;
    }

    void Place(TLBSWidget* Widget, const int16_t X, const int16_t Y, const int16_t Width, const int16_t Height) {
        if (Widget) Widget->rect = {X, Y, static_cast<int16_t>(X + Width), static_cast<int16_t>(Y + Height)};
    }

    TEWLabel* AddLabel(TLBSWidget* Parent, const int16_t X, const int16_t TextY, const int16_t Width,
                       const uint8_t Alignment, const wchar_t* Text) {
        TEWLabel* Label = Widget::Create<TEWLabel>(CachedHost);
        if (!Label) return nullptr;
        Place(Label, X, static_cast<int16_t>(TextY + TextNudge), Width, 30);
        Label->textAlignment = Alignment;
        Label->pxPerLine = Width;
        Label->SetText(Text);
        Attach(Parent, Label);
        return Label;
    }

    void SetLabel(TEWLabel* Label, const std::wstring& Text, const Color& TextColor) {
        if (!Label) return;
        Label->SetText(Text.c_str());
        Label->textColor = TextColor;
    }

    std::wstring Fit(const std::wstring& Text, const int Width) {
        if (WidgetKit::MeasureText(1, Text.c_str()) <= Width) return Text;
        std::wstring Cut = Text;
        while (!Cut.empty() && WidgetKit::MeasureText(1, (Cut + L"..").c_str()) > Width) Cut.pop_back();
        return Cut + L"..";
    }

    TEWGraphicButtonWidget* AddSpriteButton(TLBSWidget* Parent, std::initializer_list<AtlasFrame> Frames,
                                            const WidgetKit::ClickFn OnClick, void* Argument) {
        TEWGraphicButtonWidget* Button = Widget::Create<TEWGraphicButtonWidget>(CachedHost);
        if (!Button) return nullptr;
        delete[] Button->imageData.atlasFrames;
        Button->imageData.imageName = 1593835585;
        Button->imageData.imageWidth = 512;
        Button->imageData.imageHeight = 512;
        Button->imageData.frameCount = static_cast<int16_t>(Frames.size());
        Button->imageData.atlasFrames = new AtlasFrame[Frames.size()];
        std::copy(Frames.begin(), Frames.end(), Button->imageData.atlasFrames);
        Button->drawMode = 0;
        WidgetKit::SetOnClick(Button, OnClick, Argument);
        Attach(Parent, Button);
        return Button;
    }

    TEWGraphicButtonWidget* AddBlueButton(TLBSWidget* Parent, const int16_t X, const int16_t Y, const wchar_t* Text,
                                          const WidgetKit::ClickFn OnClick) {
        TEWGraphicButtonWidget* Button = AddSpriteButton(Parent, {
            {419, 308, 5, 24}, {424, 308, 10, 24}, {409, 308, 10, 24},
            {445, 308, 5, 24}, {450, 308, 10, 24}, {435, 308, 10, 24},
            {471, 308, 5, 24}, {476, 308, 10, 24}, {461, 308, 10, 24},
        }, OnClick, nullptr);
        if (!Button) return nullptr;
        constexpr int16_t Cap = 10;
        constexpr int16_t Air = 8;
        const int Measured = WidgetKit::MeasureText(1, Text);
        const auto Width = static_cast<int16_t>(std::max(40, (Measured > 0 ? Measured : 30) + 2 * (Cap + Air)));
        Button->nineSliceInfo = {static_cast<uint16_t>(Width - 2 * Cap), 25, static_cast<uint16_t>(Width - Cap), 25,
                                 Cap, 0, Cap, 0};
        Button->sliceCount = 3;
        Button->drawMode = 4;
        Button->rect = {X, Y, static_cast<int16_t>(X + Width), static_cast<int16_t>(Y + 25)};
        TEWLabel* Caption = AddLabel(Button, 0, 6, Width, 3, Text);
        if (Caption) {
            Caption->textColor = Color(255, 201, 227, 252);
            Caption->shadowColor = Color(255, 1, 61, 255);
        }
        return Button;
    }

    void SetStatus(const std::wstring& First, const Color& FirstColor, const std::wstring& Second = L"",
                   const Color& SecondColor = HintColor) {
        SetLabel(StatusLines[0], Fit(First, ContentWidth), FirstColor);
        SetLabel(StatusLines[1], Fit(Second, ContentWidth), SecondColor);
    }

    EntryState StateOf(const FilterConfig::Entry& Entry) {
        if (!CatalogReady) return EntryState::Ok;
        const ItemCatalog::Item* Item = ItemCatalog::Find(Entry.Vnum);
        if (!Item) return EntryState::Missing;
        return Item->Name == Entry.Name ? EntryState::Ok : EntryState::Renamed;
    }

    void RefreshModeHint() {
        const bool Whitelist = Settings.Current.ListMode == FilterConfig::Mode::Whitelist;
        SetLabel(ModeHint, Whitelist ? L"Hides every drop that is not in the list."
                                     : L"Hides the drops that are in the list.", HintColor);
    }

    void RefreshList() {
        const auto& Entries = Settings.Current.Entries;
        const int Total = static_cast<int>(Entries.size());
        ListScroll = std::clamp(ListScroll, 0, std::max(0, Total - VisibleRows));

        for (int i = 0; i < VisibleRows; i++) {
            ListRow& Row = Rows[i];
            const int Index = ListScroll + i;
            const bool Visible = Index < Total;
            Show(Row.Name, Visible);
            Show(Row.Remove, Visible);
            if (!Visible) {
                WidgetKit::SetItemIcon(Row.Icon, -1);
                continue;
            }
            const FilterConfig::Entry& Entry = Entries[Index];
            const ItemCatalog::Item* Item = ItemCatalog::Find(Entry.Vnum);
            WidgetKit::SetItemIcon(Row.Icon, Item ? Item->Icon : static_cast<int16_t>(-1));
            switch (StateOf(Entry)) {
                case EntryState::Ok:
                    SetLabel(Row.Name, Fit(Item ? Item->Name : Entry.Name, NameWidth), PlainColor);
                    break;
                case EntryState::Renamed:
                    SetLabel(Row.Name, Fit(Item->Name + L" (saved: " + Entry.Name + L")", NameWidth), RenamedColor);
                    break;
                case EntryState::Missing:
                    SetLabel(Row.Name, Fit(Entry.Name + L" (unknown id " + std::to_wstring(Entry.Vnum) + L")", NameWidth),
                             MissingColor);
                    break;
            }
        }

        Show(EmptyHint, Total == 0);
        const bool Paged = Total > VisibleRows;
        Show(PrevPage, Paged && ListScroll > 0);
        Show(NextPage, Paged && ListScroll + VisibleRows < Total);
        Show(PageLabel, Paged);
        if (Paged && PageLabel) {
            PageLabel->SetText((std::to_wstring(ListScroll + 1) + L"-"
                                + std::to_wstring(std::min(Total, ListScroll + VisibleRows)) + L" / "
                                + std::to_wstring(Total)).c_str());
        }
    }

    void Commit() {
        FilterConfig::SaveState(Settings);
        RebuildFilter();
        RefreshList();
    }

    void RefreshConfigs(const std::wstring& Select) {
        ConfigNames = FilterConfig::List();
        if (!ConfigPicker) return;
        ConfigPicker->SetItems(ConfigNames);
        const auto Found = std::find(ConfigNames.begin(), ConfigNames.end(), Select);
        ConfigPicker->SetSelected(Found == ConfigNames.end() ? -1 : static_cast<int>(Found - ConfigNames.begin()));
    }

    void FillItemPicker() {
        if (!ItemPicker) return;
        CatalogReady = ItemCatalog::Load();
        std::vector<std::wstring> Names;
        std::vector<int16_t> Icons;
        PickerVnums.clear();
        for (const auto& Item : ItemCatalog::All()) {
            Names.push_back(Item.Name);
            Icons.push_back(Item.Icon);
            PickerVnums.push_back(Item.Vnum);
        }
        ItemPicker->SetItems(std::move(Names), std::move(Icons));
    }

    void ReportLoaded(const std::wstring& Name) {
        int Renamed = 0, Missing = 0;
        for (const auto& Entry : Settings.Current.Entries) {
            const EntryState State = StateOf(Entry);
            if (State == EntryState::Renamed) Renamed++;
            if (State == EntryState::Missing) Missing++;
        }
        const std::wstring First = L"Loaded '" + Name + L"': " + std::to_wstring(Settings.Current.Entries.size())
                                   + L" items.";
        if (!CatalogReady) {
            SetStatus(First, GoodColor, L"Item list not found, names not checked.", RenamedColor);
        } else if (Renamed || Missing) {
            std::wstring Second = L"Check the red rows:";
            if (Renamed) Second += L" " + std::to_wstring(Renamed) + L" renamed";
            if (Missing) Second += std::wstring(Renamed ? L"," : L"") + L" " + std::to_wstring(Missing) + L" unknown";
            SetStatus(First, GoodColor, Second + L".", RenamedColor);
        } else {
            SetStatus(First, GoodColor, L"Every id still has its saved name.");
        }
    }

    void __cdecl OnEnabledChanged(void*) {
        Settings.Enabled = EnabledValue;
        Commit();
    }

    void __cdecl OnModePicked(void*, const int Index) {
        Settings.Current.ListMode = Index == 1 ? FilterConfig::Mode::Whitelist : FilterConfig::Mode::Blacklist;
        RefreshModeHint();
        Commit();
    }

    void __cdecl OnItemPicked(void*, const int Index) {
        if (ItemPicker) ItemPicker->SetSelected(-1);
        if (Index < 0 || Index >= static_cast<int>(PickerVnums.size())) return;
        const int Vnum = PickerVnums[Index];
        auto& Entries = Settings.Current.Entries;
        const auto Existing = std::find_if(Entries.begin(), Entries.end(),
                                           [&](const FilterConfig::Entry& Entry) { return Entry.Vnum == Vnum; });
        const ItemCatalog::Item* Item = ItemCatalog::Find(Vnum);
        if (Existing != Entries.end()) {
            ListScroll = static_cast<int>(Existing - Entries.begin());
            SetStatus((Item ? Item->Name : std::to_wstring(Vnum)) + L" is already in the list.", HintColor);
            RefreshList();
            return;
        }
        Entries.push_back({Vnum, Item ? Item->Name : L""});
        ListScroll = static_cast<int>(Entries.size()) - VisibleRows;
        Commit();
        SetStatus(L"Added " + (Item ? Item->Name : std::to_wstring(Vnum)) + L".", GoodColor);
    }

    void __cdecl OnRemoveRow(void* Argument) {
        const auto* Row = static_cast<ListRow*>(Argument);
        auto& Entries = Settings.Current.Entries;
        const int Index = ListScroll + Row->Slot;
        if (Index >= static_cast<int>(Entries.size())) return;
        Entries.erase(Entries.begin() + Index);
        Commit();
    }

    void __cdecl OnPrevPage(void*) {
        ListScroll -= VisibleRows;
        RefreshList();
    }

    void __cdecl OnNextPage(void*) {
        ListScroll += VisibleRows;
        RefreshList();
    }

    void __cdecl OnConfigPicked(void*, const int Index) {
        if (Index < 0 || Index >= static_cast<int>(ConfigNames.size())) return;
        const std::wstring Name = ConfigNames[Index];
        const auto Loaded = FilterConfig::Load(Name);
        if (!Loaded) {
            SetStatus(L"Couldn't read '" + Name + L"'.", RenamedColor);
            return;
        }
        Settings.Current = *Loaded;
        Settings.ActiveConfig = Name;
        if (NameBox.Edit) NameBox.Edit->SetText(Name.c_str());
        if (ModePicker) ModePicker->SetSelected(Settings.Current.ListMode == FilterConfig::Mode::Whitelist ? 1 : 0);
        RefreshModeHint();
        ListScroll = 0;
        Commit();
        ReportLoaded(Name);
    }

    void __cdecl OnSave(void*) {
        const std::wstring Name = NameBox.Edit ? NameBox.Edit->GetText() : L"";
        if (!FilterConfig::IsValidName(Name)) {
            SetStatus(L"Type a name first (no \\ / : * ? \" < > |).", RenamedColor);
            return;
        }
        const bool Existed = std::find(ConfigNames.begin(), ConfigNames.end(), Name) != ConfigNames.end();
        if (!FilterConfig::Save(Name, Settings.Current)) {
            SetStatus(L"Couldn't save '" + Name + L"'.", RenamedColor);
            return;
        }
        Settings.ActiveConfig = Name;
        FilterConfig::SaveState(Settings);
        RefreshConfigs(Name);
        SetStatus((Existed ? L"Saved over '" : L"Saved '") + Name + L"'.", GoodColor);
    }

    void CreateFilterWindow(TLBSWidget* Root) {
        const WidgetKit::WindowDesc Desc{WidgetKit::WindowStyle::Plain, 220, 120, WindowWidth, WindowHeight,
                                         L"Drop Filter", true};
        Window = WidgetKit::CreateGameWindow(CachedHost, Desc);
        if (!Window) return;
        Window->isVisible = false;
        Window->isRestrictedToScreen = true;

        HiddenLabel = AddLabel(Window, WindowWidth - 34 - 120, 15, 120, 2, L"");
        if (HiddenLabel) HiddenLabel->textColor = HintColor;
        ShownHidden = -1;

        EnabledValue = Settings.Enabled;
        EnabledBox = WidgetKit::CreateLabeledCheckbox(CachedHost, Pad, OptionsTop, L"Enabled", 70, &EnabledValue,
                                                      &OnEnabledChanged);
        Attach(Window, EnabledBox);
        ModeHint = AddLabel(Window, Pad, HintTop, ContentWidth, 1, L"");

        AddLabel(Window, Pad, LoadTop + 2, 50, 1, L"Config");
        AddLabel(Window, Pad, SaveTop + 4, 50, 1, L"Name");
        NameBox = WidgetKit::CreateEditBox(CachedHost, Pad + 50, SaveTop, 150, 22);
        Attach(Window, NameBox.Frame);
        if (NameBox.Edit) NameBox.Edit->SetText(Settings.ActiveConfig.c_str());
        AddBlueButton(Window, Pad + 50 + 156, SaveTop - 1, L"Save", &OnSave);

        TEWLabel* ListHeader = AddLabel(Window, Pad, PickerTop - 20, ContentWidth, 1, L"Items");
        if (ListHeader) ListHeader->textColor = HintColor;

        for (int i = 0; i < VisibleRows; i++) {
            ListRow& Row = Rows[i];
            Row.Slot = i;
            const auto Y = static_cast<int16_t>(ListTop + i * RowHeight);
            Row.Icon = WidgetKit::CreateItemIcon(CachedHost, Pad, static_cast<int16_t>(Y + 1), IconSize, -1);
            Attach(Window, Row.Icon);
            Row.Name = AddLabel(Window, NameX, static_cast<int16_t>(Y + 4), NameWidth, 1, L"");
            Row.Remove = AddSpriteButton(Window, {{244, 2, 20, 20}, {264, 2, 20, 20}, {244, 22, 20, 20}},
                                         &OnRemoveRow, &Row);
            Place(Row.Remove, RemoveX, Y, RemoveSize, RemoveSize);
        }
        EmptyHint = AddLabel(Window, Pad, ListTop + 4, ContentWidth, 1, L"Nothing listed yet. Add items above.");
        if (EmptyHint) EmptyHint->textColor = HintColor;

        PrevPage = AddSpriteButton(Window, {{476, 71, 12, 19}, {476, 49, 12, 19}, {476, 71, 12, 19}}, &OnPrevPage, nullptr);
        NextPage = AddSpriteButton(Window, {{495, 71, 12, 19}, {495, 49, 12, 19}, {495, 71, 12, 19}}, &OnNextPage, nullptr);
        Place(PrevPage, Pad, PagerTop, 12, 19);
        Place(NextPage, static_cast<int16_t>(WindowWidth - Pad - 12), PagerTop, 12, 19);
        PageLabel = AddLabel(Window, Pad + 12, PagerTop + 3, ContentWidth - 24, 3, L"");

        StatusLines[0] = AddLabel(Window, Pad, StatusTop, ContentWidth, 1, L"");
        StatusLines[1] = AddLabel(Window, Pad, StatusTop + 18, ContentWidth, 1, L"");

        WidgetKit::Dropdown::Desc PickerDesc;
        PickerDesc.X = Pad;
        PickerDesc.Y = PickerTop;
        PickerDesc.Width = ContentWidth - DropdownArrow;
        PickerDesc.VisibleRows = 12;
        PickerDesc.Placeholder = L"+ Add item (type to search)";
        PickerDesc.ShowIcons = true;
        PickerDesc.OnPick = &OnItemPicked;
        ItemPicker = WidgetKit::Dropdown::Create(CachedHost, PickerDesc);
        if (ItemPicker) Attach(Window, ItemPicker->Widget());

        WidgetKit::Dropdown::Desc ConfigDesc;
        ConfigDesc.X = Pad + 50;
        ConfigDesc.Y = LoadTop;
        ConfigDesc.Width = ContentWidth - 50 - DropdownArrow;
        ConfigDesc.VisibleRows = 8;
        ConfigDesc.Placeholder = L"Load a saved config";
        ConfigDesc.OnPick = &OnConfigPicked;
        ConfigPicker = WidgetKit::Dropdown::Create(CachedHost, ConfigDesc);
        if (ConfigPicker) Attach(Window, ConfigPicker->Widget());

        WidgetKit::Dropdown::Desc ModeDesc;
        ModeDesc.X = Pad + 110;
        ModeDesc.Y = OptionsTop - 1;
        ModeDesc.Width = 100;
        ModeDesc.VisibleRows = 2;
        ModeDesc.Filterable = false;
        ModeDesc.OnPick = &OnModePicked;
        ModePicker = WidgetKit::Dropdown::Create(CachedHost, ModeDesc);
        if (ModePicker) {
            ModePicker->SetItems({L"Blacklist", L"Whitelist"});
            ModePicker->SetSelected(Settings.Current.ListMode == FilterConfig::Mode::Whitelist ? 1 : 0);
            Attach(Window, ModePicker->Widget());
        }

        FillItemPicker();
        RefreshConfigs(Settings.ActiveConfig);
        RefreshModeHint();
        ListScroll = 0;
        RefreshList();
        if (CatalogReady) {
            SetStatus(std::to_wstring(ItemCatalog::All().size()) + L" items to pick from.", HintColor);
        } else {
            SetStatus(L"Couldn't read the game's item list.", RenamedColor, L"Saved lists still filter by id.");
        }
        Attach(Root, Window);
    }

    void CloseDropdowns() {
        for (WidgetKit::Dropdown* Picker : {ItemPicker, ConfigPicker, ModePicker}) {
            if (Picker) Picker->SetOpen(false);
        }
    }

    void Detach(TLBSWidget* Widget) {
        if (!Widget || !Widget->parent || !Widget->parent->childrenList) return;
        Widget->parent->childrenList->remove(Widget);
        Widget->parent = nullptr;
    }

    void ForgetWindow() {
        Window = nullptr;
        EnabledBox = nullptr;
        ModePicker = nullptr;
        ModeHint = nullptr;
        ConfigPicker = nullptr;
        NameBox = {};
        ItemPicker = nullptr;
        for (ListRow& Row : Rows) Row = {};
        EmptyHint = nullptr;
        PrevPage = nullptr;
        NextPage = nullptr;
        PageLabel = nullptr;
        StatusLines[0] = StatusLines[1] = nullptr;
        HiddenLabel = nullptr;
    }
}

extern "C" {
    __declspec(dllexport) const ModClassRequirement* ModGetRequirements(size_t* OutCount) {
        *OutCount = std::size(Requirements);
        return Requirements;
    }

    __declspec(dllexport) void ModStartup(ImGuiContext* Context, const ImGuiMemAllocFunc AllocFunc,
                                          const ImGuiMemFreeFunc FreeFunc, void* AllocUserData,
                                          const ModHost* Host) {
        ImGui::SetCurrentContext(Context);
        ImGui::SetAllocatorFunctions(AllocFunc, FreeFunc, AllocUserData);
        CachedHost = Host;
        Settings = FilterConfig::LoadState();
        RebuildFilter();
        Packet::SubscribePacket(Host, &OnDrop);
        Packet::SubscribePacket(Host, &OnIn);
        Packet::SubscribePacket(Host, &OnOut);
        Packet::SubscribePacket(Host, &OnPickUp);
        Packet::SubscribePacket(Host, &OnMapChange);
    }

    __declspec(dllexport) void ModShutdown() {
        Detach(Window);
        ForgetWindow();
        AttachedRoot = nullptr;
        CachedHost = nullptr;
    }

    __declspec(dllexport) void ModEarlyTick(const TLBSWidget* RootWidget, const TickContext tickContext) {
        TLBSWidget* Root = const_cast<TLBSWidget*>(RootWidget);
        if (!CachedHost || !Root || !Root->childrenList) return;
        if (!tickContext.isPlayerLoaded) {
            Show(Window, false);
            return;
        }

        if (Root != AttachedRoot) {
            ForgetWindow();
            AttachedRoot = Root;
            CreateFilterWindow(Root);
        }
        if (!Window) {
            CachedHost->ReportStatus(ModHealthLevel::Broken, "Couldn't create the filter window.");
            return;
        }
        if (CatalogReady) {
            CachedHost->ReportStatus(ModHealthLevel::Ok, "");
        } else {
            CachedHost->ReportStatus(ModHealthLevel::Warning, "Game item list not found; the item picker is empty.");
        }

        if (PendingToggle) {
            PendingToggle = false;
            const bool Open = !Window->isVisible;
            if (Open) {
                if (!CatalogReady) FillItemPicker();
                RefreshConfigs(Settings.ActiveConfig);
                RefreshList();
            } else {
                CloseDropdowns();
            }
            Show(Window, Open);
            if (Open) Window->BubbleUp();
        }
        if (!Window->isVisible) return;

        const int Hidden = HiddenCount.load();
        if (Hidden != ShownHidden && HiddenLabel) {
            ShownHidden = Hidden;
            HiddenLabel->SetText(Hidden > 0 ? (std::to_wstring(Hidden) + L" hidden on this map").c_str() : L"");
        }
        for (WidgetKit::Dropdown* Picker : {ItemPicker, ConfigPicker, ModePicker}) {
            if (Picker) Picker->Tick(tickContext);
        }
    }

    __declspec(dllexport) void ModTick(const TLBSWidget*, TickContext) {}

    __declspec(dllexport) void ModToggleMainWindow() {
        PendingToggle = true;
    }
}
