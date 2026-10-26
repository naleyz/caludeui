#include "ui/layout_panel.hpp"
#include "theme/theme.hpp"

#include <imgui.h>

#include <cstddef>

namespace ui
{
namespace
{
const char* PanelLabel(const std::string& id)
{
    if (id == "sessions") return "Список сессий";
    if (id == "usage") return "Лимиты и расход";
    return id.c_str(); // shouldn't happen: AppSettingsStore drops unknown ids on load
}
} // namespace

void LayoutPanel::Draw()
{
    ImGui::TextColored(theme::palette::MidGray, "Какие панели показывать в боковой панели и в каком порядке.");
    ImGui::Spacing();

    // Edited as a local copy for the frame; committed to the store below if
    // a checkbox or a drag-drop actually changed something.
    auto panels = app_.Get().sidebarPanels;
    bool changed = false;

    for (std::size_t i = 0; i < panels.size(); ++i)
    {
        ImGui::PushID(static_cast<int>(i));
        bool visible = panels[i].visible;
        if (ImGui::Checkbox("##visible", &visible))
        {
            panels[i].visible = visible;
            changed = true;
        }
        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Text, visible ? theme::palette::Light : theme::palette::MidGray);
        ImGui::Selectable(PanelLabel(panels[i].id), false, 0, {ImGui::GetContentRegionAvail().x, 0});
        ImGui::PopStyleColor();

        if (ImGui::BeginDragDropSource())
        {
            ImGui::SetDragDropPayload("PANEL_ROW", &i, sizeof(i));
            ImGui::TextUnformatted(PanelLabel(panels[i].id));
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget())
        {
            if (const auto* payload = ImGui::AcceptDragDropPayload("PANEL_ROW"))
            {
                const std::size_t src = *static_cast<const std::size_t*>(payload->Data);
                if (src != i)
                {
                    auto moved = panels[src];
                    panels.erase(panels.begin() + static_cast<std::ptrdiff_t>(src));
                    panels.insert(panels.begin() + static_cast<std::ptrdiff_t>(i), moved);
                    changed = true;
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::PopID();
    }

    ImGui::Spacing();
    ImGui::TextColored(theme::palette::MidGray,
                       "Потяните строку, чтобы поменять порядок панелей.");
    if (!error_.empty()) ImGui::TextColored({0.85f, 0.37f, 0.34f, 1.0f}, "%s", error_.c_str());

    if (changed) app_.SetSidebarPanels(panels, &error_);
}
} // namespace ui
