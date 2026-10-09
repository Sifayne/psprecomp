// The player's UI toolkit: Dear ImGui behind ui.h's C API. See ui.h.
//
// The host's one C++ file of its own, kept to the subset of C++ ImGui itself
// uses, and compiled like ImGui without exceptions, RTTI or thread-safe
// statics: nothing here needs the C++ runtime, so the C link every program
// already has links it (third_party/imgui/README.md).
#include "ui.h"

#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"

#include <SDL2/SDL.h>

#include <float.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- state ----------------------------------------------------------------------

static struct {
    int started;
    SDL_Window *win;
    SDL_Renderer *ren;          // the software renderer; NULL under GL
    SDL_GameController *pad;
    float scale;                // UI units per point of the base layout
    char help[512];             // the help of the widget with focus, this frame
    char shown_help[512];       // ... and what the help area shows
    int focus_next;
    int table_column;
} u;

// The launcher's colours (src/host/launcher.c), so the player is one app.
static ImVec4 rgb(int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); }
static const ImVec4 BG = rgb(16, 22, 29), PANEL = rgb(24, 32, 42), ROW = rgb(30, 40, 52),
    BORDER = rgb(49, 64, 79), TEXT = rgb(230, 237, 240), MUTED = rgb(151, 169, 181),
    ACCENT = rgb(239, 184, 90);

static void style(float scale) {
    ImGuiStyle &s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowRounding = 6; s.ChildRounding = 4; s.FrameRounding = 4; s.PopupRounding = 4;
    s.GrabRounding = 3; s.WindowBorderSize = 1; s.ChildBorderSize = 1; s.FrameBorderSize = 0;
    s.WindowPadding = ImVec2(18, 16); s.FramePadding = ImVec2(10, 6); s.ItemSpacing = ImVec2(10, 8);
    s.CellPadding = ImVec2(8, 5); s.ScrollbarSize = 12;
    ImVec4 *c = s.Colors;
    c[ImGuiCol_Text] = TEXT;                c[ImGuiCol_TextDisabled] = MUTED;
    c[ImGuiCol_WindowBg] = rgb(24, 32, 42, 0.97f);
    c[ImGuiCol_ChildBg] = BG;               c[ImGuiCol_PopupBg] = PANEL;
    c[ImGuiCol_Border] = BORDER;            c[ImGuiCol_Separator] = BORDER;
    c[ImGuiCol_FrameBg] = ROW;              c[ImGuiCol_FrameBgHovered] = BORDER;
    c[ImGuiCol_FrameBgActive] = BORDER;
    c[ImGuiCol_Button] = ROW;               c[ImGuiCol_ButtonHovered] = BORDER;
    c[ImGuiCol_ButtonActive] = rgb(239, 184, 90, 0.55f);
    c[ImGuiCol_Header] = ROW;               c[ImGuiCol_HeaderHovered] = BORDER;
    c[ImGuiCol_HeaderActive] = rgb(239, 184, 90, 0.40f);
    c[ImGuiCol_CheckMark] = ACCENT;         c[ImGuiCol_SliderGrab] = ACCENT;
    c[ImGuiCol_SliderGrabActive] = ACCENT;  c[ImGuiCol_NavCursor] = ACCENT;
    c[ImGuiCol_TableHeaderBg] = PANEL;      c[ImGuiCol_TableBorderStrong] = BORDER;
    c[ImGuiCol_TableBorderLight] = BORDER;  c[ImGuiCol_TableRowBg] = BG;
    c[ImGuiCol_TableRowBgAlt] = rgb(20, 27, 35);
    c[ImGuiCol_ScrollbarBg] = BG;           c[ImGuiCol_ScrollbarGrab] = BORDER;
    c[ImGuiCol_ModalWindowDimBg] = rgb(8, 12, 16, 0.6f);
    s.ScaleAllSizes(scale);
    s.FontSizeBase = 17.0f;
    s.FontScaleMain = scale;
}

// The UI's size follows the window: the launcher's layout is drawn for 800
// rows, a Steam Deck has 800 and a 1440p desktop 1440. Wayland reports no
// content scale (ImGui_ImplSDL2_GetContentScaleForDisplay is 1.0), so the
// window's height is the measure.
static float scale_for(int height) {
    float s = height / 720.0f;
    return s < 0.8f ? 0.8f : s > 4.0f ? 4.0f : s;
}

// ---- the GL snapshot ------------------------------------------------------------

struct snapshot {
    ImVector<psp_ui_vertex> vertices;
    ImVector<uint16_t> indices;
    ImVector<psp_ui_command> commands;
    psp_ui_frame frame;
};
static snapshot g_snap[2];
static int g_front = -1;                // what the GL thread draws; -1: nothing
static ImVector<psp_ui_texture_op> g_pending, g_taken;
static uint32_t g_next_texture = 1;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
/* The menu's own pictures: an SDL texture under software, a texture of the GL
 * thread's, made through the same queue as ImGui's, under GL. */
static struct { uint32_t gl; SDL_Texture *tex; int w, h; } g_img[PSP_UI_IMAGES];

static void free_ops(ImVector<psp_ui_texture_op> &ops) {
    for (psp_ui_texture_op &op : ops) free((void *)op.pixels);
    ops.clear();
}

static unsigned char *copy_rect(ImTextureData *tex, int x, int y, int w, int h) {
    unsigned char *out = (unsigned char *)malloc((size_t)w * h * 4);
    if (!out) return NULL;
    for (int row = 0; row < h; row++)
        memcpy(out + (size_t)row * w * 4, tex->GetPixelsAt(x, y + row), (size_t)w * 4);
    return out;
}

// ImGui's texture requests, answered at once on its side and queued for the
// GL thread, which creates the texture before it draws anything using it.
static void queue_textures(ImDrawData *data) {
    if (!data->Textures) return;
    for (ImTextureData *tex : *data->Textures) {
        if (tex->Status == ImTextureStatus_OK || tex->Status == ImTextureStatus_Destroyed) continue;
        psp_ui_texture_op op;
        memset(&op, 0, sizeof op);
        if (tex->Status == ImTextureStatus_WantCreate) {
            op.op = PSP_UI_TEXTURE_CREATE;
            op.texture = g_next_texture++;
            op.w = tex->Width; op.h = tex->Height;
            op.pixels = copy_rect(tex, 0, 0, tex->Width, tex->Height);
            tex->SetTexID((ImTextureID)op.texture);
            tex->SetStatus(ImTextureStatus_OK);
            pthread_mutex_lock(&g_lock); g_pending.push_back(op); pthread_mutex_unlock(&g_lock);
        } else if (tex->Status == ImTextureStatus_WantUpdates) {
            for (ImTextureRect &r : tex->Updates) {
                op.op = PSP_UI_TEXTURE_UPDATE;
                op.texture = (uint32_t)tex->TexID;
                op.x = r.x; op.y = r.y; op.w = r.w; op.h = r.h;
                op.pixels = copy_rect(tex, r.x, r.y, r.w, r.h);
                pthread_mutex_lock(&g_lock); g_pending.push_back(op); pthread_mutex_unlock(&g_lock);
            }
            tex->SetStatus(ImTextureStatus_OK);
        } else if (tex->Status == ImTextureStatus_WantDestroy) {
            op.op = PSP_UI_TEXTURE_DESTROY;
            op.texture = (uint32_t)tex->TexID;
            pthread_mutex_lock(&g_lock); g_pending.push_back(op); pthread_mutex_unlock(&g_lock);
            tex->SetTexID(ImTextureID_Invalid);
            tex->SetStatus(ImTextureStatus_Destroyed);
        }
    }
}

// The frame, into the buffer the GL thread is not drawing, then swapped in.
static void snapshot_frame(ImDrawData *data) {
    queue_textures(data);
    pthread_mutex_lock(&g_lock);
    const int back = g_front == 0 ? 1 : 0;
    pthread_mutex_unlock(&g_lock);
    snapshot &s = g_snap[back];
    s.vertices.resize(0); s.indices.resize(0); s.commands.resize(0);
    const ImVec2 scale = data->FramebufferScale, origin = data->DisplayPos;
    for (const ImDrawList *list : data->CmdLists) {
        const uint32_t base_vertex = (uint32_t)s.vertices.Size, base_index = (uint32_t)s.indices.Size;
        for (const ImDrawVert &v : list->VtxBuffer) {
            psp_ui_vertex out = { v.pos.x - origin.x, v.pos.y - origin.y, v.uv.x, v.uv.y, v.col };
            s.vertices.push_back(out);
        }
        for (ImDrawIdx i : list->IdxBuffer) s.indices.push_back((uint16_t)i);
        for (const ImDrawCmd &cmd : list->CmdBuffer) {
            if (cmd.UserCallback) continue;
            psp_ui_command out;
            out.texture = (uint32_t)cmd.GetTexID();
            out.clip[0] = (cmd.ClipRect.x - origin.x) * scale.x;
            out.clip[1] = (cmd.ClipRect.y - origin.y) * scale.y;
            out.clip[2] = (cmd.ClipRect.z - origin.x) * scale.x;
            out.clip[3] = (cmd.ClipRect.w - origin.y) * scale.y;
            out.first_index = base_index + cmd.IdxOffset;
            out.count = cmd.ElemCount;
            out.first_vertex = base_vertex + cmd.VtxOffset;
            s.commands.push_back(out);
        }
    }
    s.frame.width = (int)(data->DisplaySize.x * scale.x);
    s.frame.height = (int)(data->DisplaySize.y * scale.y);
    s.frame.scale_x = scale.x; s.frame.scale_y = scale.y;
    s.frame.vertices = s.vertices.Data; s.frame.vertex_count = (uint32_t)s.vertices.Size;
    s.frame.indices = s.indices.Data; s.frame.index_count = (uint32_t)s.indices.Size;
    s.frame.commands = s.commands.Data; s.frame.command_count = (uint32_t)s.commands.Size;
    pthread_mutex_lock(&g_lock);
    g_front = back;
    pthread_mutex_unlock(&g_lock);
}

extern "C" const psp_ui_frame *psp_ui_gl_lock(const psp_ui_texture_op **ops, int *op_count) {
    pthread_mutex_lock(&g_lock);
    free_ops(g_taken);
    g_taken.swap(g_pending);
    *ops = g_taken.Data;
    *op_count = g_taken.Size;
    return g_front >= 0 ? &g_snap[g_front].frame : NULL;
}

extern "C" void psp_ui_gl_unlock(void) { pthread_mutex_unlock(&g_lock); }

extern "C" void psp_ui_clear(void) {
    pthread_mutex_lock(&g_lock);
    g_front = -1;
    pthread_mutex_unlock(&g_lock);
}

// ---- the SDL thread ---------------------------------------------------------------

static const char *find_font(void) {
    static const char *const fixed[] = {
        "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf", "C:/Windows/Fonts/segoeui.ttf",
    };
    const char *requested = getenv("PSPRECOMP_UI_FONT");
    if (requested && *requested) {
        FILE *f = fopen(requested, "rb");
        if (f) { fclose(f); return requested; }
    }
    for (const char *path : fixed) {
        FILE *f = fopen(path, "rb");
        if (f) { fclose(f); return path; }
    }
    return NULL;
}

extern "C" int psp_ui_start(SDL_Window *win, SDL_Renderer *ren) {
    if (u.started) return 0;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;
    io.LogFilename = NULL;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad |
                      ImGuiConfigFlags_NoMouseCursorChange;
    const int ok = ren ? ImGui_ImplSDL2_InitForSDLRenderer(win, ren) && ImGui_ImplSDLRenderer2_Init(ren)
                       : ImGui_ImplSDL2_InitForOther(win);
    if (!ok) { ImGui::DestroyContext(); return -1; }
    if (!ren) {
        io.BackendRendererName = "psprecomp_render_gl";
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
    }
    // The pad the input owner holds, and no other: ImGui opens none itself.
    ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_Manual, NULL, 0);
    const char *font = find_font();
    if (!font || !io.Fonts->AddFontFromFileTTF(font, 17.0f)) io.Fonts->AddFontDefault();
    u.win = win; u.ren = ren; u.pad = NULL; u.scale = 0; u.started = 1;
    return 0;
}

extern "C" void psp_ui_stop(void) {
    if (!u.started) return;
    psp_ui_clear();
    for (auto &im : g_img) if (im.tex) SDL_DestroyTexture(im.tex);
    memset(g_img, 0, sizeof g_img);
    if (u.ren) ImGui_ImplSDLRenderer2_Shutdown();
    else {
        // Answer the outstanding requests ourselves: the GL thread frees its
        // textures with its context.
        ImGuiPlatformIO &pio = ImGui::GetPlatformIO();
        for (ImTextureData *tex : pio.Textures)
            if (tex->RefCount == 1) { tex->SetTexID(ImTextureID_Invalid); tex->SetStatus(ImTextureStatus_Destroyed); }
        ImGui::GetIO().BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
        ImGui::GetIO().BackendRendererName = NULL;
    }
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    memset(&u, 0, sizeof u);
}

extern "C" void psp_ui_event(const SDL_Event *e) { if (u.started) ImGui_ImplSDL2_ProcessEvent(e); }

extern "C" void psp_ui_set_pad(SDL_GameController *pad) {
    if (!u.started || pad == u.pad) return;
    u.pad = pad;
    ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_Manual, pad ? &u.pad : NULL, pad ? 1 : 0);
}

extern "C" void psp_ui_begin(void) {
    int w = 0, h = 0;
    SDL_GetWindowSize(u.win, &w, &h);
    const float scale = scale_for(h);
    if (scale != u.scale) { style(scale); u.scale = scale; }
    if (u.ren) ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    snprintf(u.shown_help, sizeof u.shown_help, "%s", u.help);
    u.help[0] = 0;
}

extern "C" void psp_ui_end(void) {
    ImGui::Render();
    if (!u.ren) snapshot_frame(ImGui::GetDrawData());
}

extern "C" void psp_ui_draw(SDL_Renderer *ren) {
    // The game is drawn at the PSP's 480x272, scaled up; ImGui draws in the
    // window's pixels.
    int lw = 0, lh = 0;
    SDL_RenderGetLogicalSize(ren, &lw, &lh);
    SDL_RenderSetLogicalSize(ren, 0, 0);
    SDL_RenderSetScale(ren, 1.0f, 1.0f);
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), ren);
    if (lw && lh) SDL_RenderSetLogicalSize(ren, lw, lh);
}

// ---- widgets ------------------------------------------------------------------------

static void take_focus(void) {
    if (u.focus_next) { ImGui::SetItemDefaultFocus(); u.focus_next = 0; }
}

extern "C" void psp_ui_focus_next(void) { u.focus_next = 1; }

extern "C" void psp_ui_panel_begin(void) {
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::GetBackgroundDrawList()->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                                                  IM_COL32(8, 12, 16, 150));
    const float w = vp->Size.x * 0.86f, h = vp->Size.y * 0.86f;
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w < 1100 * u.scale ? w : 1100 * u.scale, h), ImGuiCond_Always);
    ImGui::Begin("##menu", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize);
}

extern "C" void psp_ui_panel_end(void) { ImGui::End(); }

extern "C" void psp_ui_side_begin(float width) {
    ImGui::BeginChild("##side", ImVec2(width * u.scale, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
}
extern "C" void psp_ui_side_next(void) {
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##page", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
}
extern "C" void psp_ui_side_end(void) { ImGui::EndChild(); }

extern "C" int psp_ui_nav(const char *label, int selected) {
    const int chosen = ImGui::Selectable(label, selected != 0);
    if (selected) take_focus();
    return chosen;
}

extern "C" void psp_ui_heading(const char *text) {
    ImGui::PushFont(NULL, ImGui::GetStyle().FontSizeBase * 1.3f);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}
extern "C" void psp_ui_text(const char *text) { ImGui::TextWrapped("%s", text); }
extern "C" void psp_ui_note(const char *text) {
    ImGui::PushStyleColor(ImGuiCol_Text, MUTED);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}
extern "C" void psp_ui_accent(const char *text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ACCENT);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}
extern "C" void psp_ui_separator(void) { ImGui::Separator(); }
extern "C" void psp_ui_same_line(void) { ImGui::SameLine(); }

extern "C" int psp_ui_button(const char *label) {
    const int chosen = ImGui::Button(label);
    take_focus();
    return chosen;
}

// A labelled row: the label on the left, the control filling the right half.
// Anything after "##" in a label is its identity, not its text, as in ImGui.
static void row_label(const char *label) {
    const float control_x = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x * 0.45f;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label, strstr(label, "##"));
    ImGui::SameLine(control_x);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

extern "C" int psp_ui_choice(const char *label, int *index, const char *const *labels, int count) {
    ImGui::PushID(label);
    row_label(label);
    int changed = 0;
    if (ImGui::BeginCombo("##choice", *index >= 0 && *index < count ? labels[*index] : "")) {
        for (int i = 0; i < count; i++) {
            if (ImGui::Selectable(labels[i], i == *index) && i != *index) { *index = i; changed = 1; }
            if (i == *index) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    take_focus();
    ImGui::PopID();
    return changed;
}

extern "C" int psp_ui_number(const char *label, double *value, double min, double max, const char *format) {
    ImGui::PushID(label);
    row_label(label);
    const int changed = ImGui::SliderScalar("##number", ImGuiDataType_Double, value, &min, &max, format);
    take_focus();
    ImGui::PopID();
    return changed;
}

extern "C" int psp_ui_toggle(const char *label, int *value) {
    bool on = *value != 0;
    const int changed = ImGui::Checkbox(label, &on);
    *value = on;
    take_focus();
    return changed;
}

extern "C" void psp_ui_disabled_begin(int disabled) { ImGui::BeginDisabled(disabled != 0); }
extern "C" void psp_ui_disabled_end(void) { ImGui::EndDisabled(); }

extern "C" void psp_ui_help(const char *text) {
    if (text && (ImGui::IsItemFocused() || ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)))
        snprintf(u.help, sizeof u.help, "%s", text);
}

extern "C" void psp_ui_help_area(void) {
    ImGui::Separator();
    psp_ui_note(u.shown_help[0] ? u.shown_help : " ");
}

extern "C" int psp_ui_table_begin(const char *id, int columns, const char *const *headers) {
    if (!ImGui::BeginTable(id, columns, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                           ImGuiTableFlags_SizingStretchProp,
                           ImVec2(0, -ImGui::GetTextLineHeightWithSpacing() * 2.5f))) return 0;
    ImGui::TableSetupScrollFreeze(0, 1);
    for (int i = 0; i < columns; i++) ImGui::TableSetupColumn(headers[i], 0, i == 0 ? 1.4f : 1.0f);
    ImGui::TableHeadersRow();
    return 1;
}
extern "C" void psp_ui_table_row(void) { ImGui::TableNextRow(); u.table_column = 0; }
extern "C" void psp_ui_table_text(const char *text) {
    ImGui::TableSetColumnIndex(u.table_column++);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
}
extern "C" int psp_ui_table_button(const char *id, const char *text, int highlight) {
    ImGui::TableSetColumnIndex(u.table_column++);
    ImGui::PushID(id);
    if (highlight) ImGui::PushStyleColor(ImGuiCol_Text, ACCENT);
    const int chosen = ImGui::Selectable(*text ? text : "-", highlight != 0);
    if (highlight) ImGui::PopStyleColor();
    take_focus();
    ImGui::PopID();
    return chosen;
}
extern "C" void psp_ui_table_end(void) { ImGui::EndTable(); }

extern "C" void psp_ui_prompt(const char *title, const char *text) {
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::GetForegroundDrawList()->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                                                  IM_COL32(8, 12, 16, 120));
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520 * u.scale, 0), ImGuiCond_Always);
    ImGui::SetNextWindowFocus();
    ImGui::Begin("##prompt", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav);
    psp_ui_heading(title);
    psp_ui_text(text);
    ImGui::End();
}

extern "C" int psp_ui_confirm(const char *title, const char *text, const char *yes, const char *no) {
    // A modal popup: it has the keys and the pad, and the page behind it
    // cannot be chosen until it is answered.
    if (!ImGui::IsPopupOpen("##confirm")) ImGui::OpenPopup("##confirm");
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520 * u.scale, 0), ImGuiCond_Always);
    int answer = -1;
    if (ImGui::BeginPopupModal("##confirm", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                               ImGuiWindowFlags_NoSavedSettings)) {
        psp_ui_heading(title);
        psp_ui_text(text);
        ImGui::Spacing();
        if (ImGui::Button(yes)) answer = 1;
        // The answer the player asked for has the cursor, shown, so Enter or
        // A takes it at once and the arrows move to the other.
        if (ImGui::IsWindowAppearing()) { ImGui::SetItemDefaultFocus(); ImGui::SetNavCursorVisible(true); }
        ImGui::SameLine();
        if (ImGui::Button(no)) answer = 0;
        if (answer >= 0) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    return answer;
}

extern "C" void psp_ui_confirm_close(void) {
    if (!ImGui::IsPopupOpen("##confirm")) return;
    if (ImGui::BeginPopupModal("##confirm", NULL, ImGuiWindowFlags_NoDecoration)) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

extern "C" void psp_ui_image_set(int slot, const unsigned char *rgba, int w, int h) {
    if (slot < 0 || slot >= PSP_UI_IMAGES || !u.started) return;
    auto &im = g_img[slot];
    if (u.ren) {
        if (im.tex && (!rgba || im.w != w || im.h != h)) { SDL_DestroyTexture(im.tex); im.tex = NULL; }
        if (rgba && !im.tex) im.tex = SDL_CreateTexture(u.ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
        if (rgba && im.tex) SDL_UpdateTexture(im.tex, NULL, rgba, w * 4);
    } else {
        psp_ui_texture_op op;
        memset(&op, 0, sizeof op);
        if (im.gl && (!rgba || im.w != w || im.h != h)) {
            op.op = PSP_UI_TEXTURE_DESTROY;
            op.texture = im.gl;
            pthread_mutex_lock(&g_lock); g_pending.push_back(op); pthread_mutex_unlock(&g_lock);
            im.gl = 0;
        }
        if (rgba) {
            unsigned char *copy = (unsigned char *)malloc((size_t)w * h * 4);
            if (!copy) return;
            memcpy(copy, rgba, (size_t)w * h * 4);
            op.op = im.gl ? PSP_UI_TEXTURE_UPDATE : PSP_UI_TEXTURE_CREATE;
            if (!im.gl) im.gl = g_next_texture++;
            op.texture = im.gl;
            op.x = op.y = 0; op.w = w; op.h = h;
            op.pixels = copy;
            pthread_mutex_lock(&g_lock); g_pending.push_back(op); pthread_mutex_unlock(&g_lock);
        }
    }
    im.w = rgba ? w : 0;
    im.h = rgba ? h : 0;
}

extern "C" int psp_ui_picture_row(const char *id, int image, const char *title, const char *detail, int highlight) {
    const float h = 76 * u.scale, w = h * 240.0f / 136.0f, pad = 10 * u.scale;
    ImGui::PushID(id);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const int chosen = ImGui::Selectable("##row", highlight != 0, 0, ImVec2(0, h));
    take_focus();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 a(at.x, at.y), b(at.x + w, at.y + h);
    const int have = image >= 0 && image < PSP_UI_IMAGES && g_img[image].w;
    if (have) {
        const ImTextureID tex = u.ren ? (ImTextureID)(intptr_t)g_img[image].tex : (ImTextureID)g_img[image].gl;
        dl->AddImage(ImTextureRef(tex), a, b);
    } else {
        dl->AddRectFilled(a, b, ImGui::GetColorU32(BG));
        dl->AddRect(a, b, ImGui::GetColorU32(BORDER));
    }
    const float line = ImGui::GetTextLineHeightWithSpacing();
    dl->AddText(ImVec2(b.x + pad, at.y + h * 0.5f - line), ImGui::GetColorU32(highlight ? ACCENT : TEXT), title);
    if (detail) dl->AddText(ImVec2(b.x + pad, at.y + h * 0.5f), ImGui::GetColorU32(MUTED), detail);
    ImGui::PopID();
    return chosen;
}

extern "C" void psp_ui_toast(const char *text) {
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + 24 * u.scale, vp->Pos.y + vp->Size.y - 24 * u.scale), ImGuiCond_Always,
                            ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.9f);
    ImGui::Begin("##toast", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextUnformatted(text);
    ImGui::End();
}
