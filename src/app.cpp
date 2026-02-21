#include "app.h"

#include <imgui.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <dirent.h>
#endif

extern "C" {
#include "proto.h"
#include "sync.h"
#include "dht_client.h"
#include "qrcodegen.h"
}

#define MAX_CHATS 64

/* ---- application state ---- */

static char       g_basedir[256]    = {};
static char       g_active_chat[64] = {};
static proto_chat g_chat            = {};
static bool       g_chat_open       = false;

static char g_msg_input[4096]  = {};
static char g_new_name[64]     = {};
static char g_join_name[64]    = {};
static char g_join_cmd[256]    = {};
static char g_credentials[128] = {};
static char g_search[64]       = {};

static bool g_show_new_modal   = false;
static bool g_show_join_modal  = false;
static bool g_show_cred_modal  = false;

static bool g_scroll_to_bottom = false;
static bool g_quit_requested   = false;

/* unread tracking – touched only from main thread */
static char g_unread[MAX_CHATS][64];
static int  g_unread_n = 0;

static bool has_unread(const char *name) {
    for (int i = 0; i < g_unread_n; i++)
        if (strcmp(g_unread[i], name) == 0) return true;
    return false;
}
static void set_unread(const char *name) {
    if (has_unread(name)) return;
    if (g_unread_n < MAX_CHATS) strncpy(g_unread[g_unread_n++], name, 63);
}
static void clear_unread(const char *name) {
    for (int i = 0; i < g_unread_n; i++) {
        if (strcmp(g_unread[i], name) == 0) {
            g_unread[i][0] = '\0';
            if (i < g_unread_n - 1) memcpy(g_unread[i], g_unread[g_unread_n - 1], 64);
            g_unread_n--;
            return;
        }
    }
}

/* sync thread */
static std::atomic<bool> g_sync_stop{false};
static std::atomic<bool> g_sync_delivered{false}; /* set when sync adds messages */
static std::thread       g_sync_thread;

static void color_table_reset(void);

/* ---- helpers ---- */

static int scan_chats(char names[][64])
{
    int count = 0;
    char path[512];
    snprintf(path, sizeof(path), "%s/chats", g_basedir);

#ifdef _WIN32
    char pattern[600];
    snprintf(pattern, sizeof(pattern), "%s\\*.chat", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        int len = (int)strlen(fd.cFileName);
        if (len > 5 && len - 5 < 64) {
            memcpy(names[count], fd.cFileName, (size_t)(len - 5));
            names[count][len - 5] = '\0';
            count++;
        }
    } while (count < MAX_CHATS && FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(path);
    if (!d) return 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < MAX_CHATS) {
        int len = (int)strlen(ent->d_name);
        if (len > 5 && len - 5 < 64 && strcmp(ent->d_name + len - 5, ".chat") == 0) {
            memcpy(names[count], ent->d_name, (size_t)(len - 5));
            names[count][len - 5] = '\0';
            count++;
        }
    }
    closedir(d);
#endif
    return count;
}

static void open_chat(const char *name)
{
    if (g_chat_open) {
        proto_save_chat(&g_chat, g_active_chat, g_basedir);
        proto_chat_cleanup(&g_chat);
        g_chat_open = false;
    }
    if (proto_load_chat(&g_chat, name, g_basedir) == 0) {
        strncpy(g_active_chat, name, sizeof(g_active_chat) - 1);
        g_active_chat[sizeof(g_active_chat) - 1] = '\0';
        g_chat_open = true;
        dht_client_add_chat(g_chat.user_key);
        g_scroll_to_bottom = true;
        g_msg_input[0] = '\0';
        color_table_reset();
        clear_unread(name);
    }
}

static void close_chat(void)
{
    if (g_chat_open) {
        proto_save_chat(&g_chat, g_active_chat, g_basedir);
        proto_chat_cleanup(&g_chat);
        g_chat_open = false;
    }
    g_active_chat[0] = '\0';
}

/* ---- lifecycle ---- */

static void apply_theme(void)
{
    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowRounding    = 0.f;
    s.ChildRounding     = 6.f;
    s.FrameRounding     = 6.f;
    s.PopupRounding     = 8.f;
    s.ScrollbarRounding = 6.f;
    s.GrabRounding      = 4.f;
    s.ItemSpacing       = ImVec2(8, 6);
    s.FramePadding      = ImVec2(10, 6);
    s.WindowPadding     = ImVec2(0, 0);
    s.ScrollbarSize     = 10.f;

    ImVec4 *c = s.Colors;
    c[ImGuiCol_WindowBg]             = ImVec4(0.10f, 0.10f, 0.13f, 1.f);
    c[ImGuiCol_ChildBg]              = ImVec4(0.10f, 0.10f, 0.13f, 1.f);
    c[ImGuiCol_PopupBg]              = ImVec4(0.14f, 0.14f, 0.18f, 1.f);
    c[ImGuiCol_Border]               = ImVec4(0.22f, 0.22f, 0.28f, 1.f);
    c[ImGuiCol_FrameBg]              = ImVec4(0.16f, 0.16f, 0.20f, 1.f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.20f, 0.20f, 0.26f, 1.f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.22f, 0.22f, 0.30f, 1.f);
    c[ImGuiCol_TitleBg]              = ImVec4(0.08f, 0.08f, 0.10f, 1.f);
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.10f, 0.10f, 0.13f, 1.f);
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.08f, 0.08f, 0.10f, 1.f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.28f, 0.28f, 0.34f, 1.f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.34f, 0.34f, 0.42f, 1.f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.20f, 0.76f, 0.96f, 1.f);
    c[ImGuiCol_CheckMark]            = ImVec4(0.20f, 0.76f, 0.96f, 1.f);
    c[ImGuiCol_SliderGrab]           = ImVec4(0.20f, 0.76f, 0.96f, 1.f);
    c[ImGuiCol_Button]               = ImVec4(0.20f, 0.22f, 0.28f, 1.f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.26f, 0.28f, 0.38f, 1.f);
    c[ImGuiCol_ButtonActive]         = ImVec4(0.14f, 0.58f, 0.80f, 1.f);
    c[ImGuiCol_Header]               = ImVec4(0.18f, 0.20f, 0.28f, 1.f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.22f, 0.24f, 0.34f, 1.f);
    c[ImGuiCol_HeaderActive]         = ImVec4(0.16f, 0.56f, 0.78f, 1.f);
    c[ImGuiCol_Separator]            = ImVec4(0.20f, 0.20f, 0.26f, 1.f);
    c[ImGuiCol_Text]                 = ImVec4(0.90f, 0.90f, 0.95f, 1.f);
    c[ImGuiCol_TextDisabled]         = ImVec4(0.40f, 0.40f, 0.50f, 1.f);
}

void app_init(const char *basedir)
{
    strncpy(g_basedir, basedir, sizeof(g_basedir) - 1);
    g_basedir[sizeof(g_basedir) - 1] = '\0';

    apply_theme();

    int port = sync_start_server();
    if (port > 0)
        sync_register(port);

    if (port > 0) {
        dht_client_start(port, basedir);
        char names[MAX_CHATS][64];
        int count = scan_chats(names);
        for (int i = 0; i < count; i++) {
            proto_chat tmp = {};
            if (proto_load_chat(&tmp, names[i], basedir) == 0) {
                dht_client_add_chat(tmp.user_key);
                proto_chat_cleanup(&tmp);
            }
        }
    }

    sync_with_peers();

    g_sync_stop = false;
    g_sync_thread = std::thread([]() {
        while (!g_sync_stop) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!g_sync_stop) {
                int added = sync_with_peers();
                if (added > 0)
                    g_sync_delivered = true;
            }
        }
    });
}

void app_shutdown(void)
{
    g_sync_stop = true;
    if (g_sync_thread.joinable())
        g_sync_thread.join();
    dht_client_stop();
    sync_unregister();
    if (g_chat_open) {
        proto_save_chat(&g_chat, g_active_chat, g_basedir);
        proto_chat_cleanup(&g_chat);
        g_chat_open = false;
    }
}

int app_quit_requested(void)
{
    return g_quit_requested ? 1 : 0;
}

/* ---- full-size window helper ---- */

static void begin_fullscreen(const char *id)
{
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin(id, nullptr,
        ImGuiWindowFlags_NoTitleBar     |
        ImGuiWindowFlags_NoResize       |
        ImGuiWindowFlags_NoMove         |
        ImGuiWindowFlags_NoScrollbar    |
        ImGuiWindowFlags_NoScrollWithMouse);
}

/* ---- per-entity colour assignment ---- */

static const ImVec4 k_colors[] = {
    {0.30f, 0.80f, 1.00f, 1.f},
    {1.00f, 0.80f, 0.20f, 1.f},
    {0.40f, 1.00f, 0.40f, 1.f},
    {1.00f, 0.50f, 0.30f, 1.f},
    {0.90f, 0.40f, 0.90f, 1.f},
    {0.40f, 0.90f, 0.90f, 1.f},
    {1.00f, 0.70f, 0.70f, 1.f},
    {0.75f, 0.75f, 1.00f, 1.f},
};
static constexpr int k_ncolors = (int)(sizeof(k_colors) / sizeof(k_colors[0]));

static char g_eid_map[k_ncolors][17] = {};
static int  g_eid_count = 0;

static void color_table_reset(void)
{
    memset(g_eid_map, 0, sizeof(g_eid_map));
    g_eid_count = 0;
}

static ImVec4 color_for(const char *eid)
{
    if (!eid || eid[0] == '\0')
        return ImVec4(0.6f, 0.6f, 0.6f, 1.f);
    for (int i = 0; i < g_eid_count; i++)
        if (strcmp(g_eid_map[i], eid) == 0)
            return k_colors[i];
    if (g_eid_count < k_ncolors) {
        strncpy(g_eid_map[g_eid_count], eid, 16);
        g_eid_map[g_eid_count][16] = '\0';
        return k_colors[g_eid_count++];
    }
    unsigned h = 0;
    for (const char *p = eid; *p; p++) h = h * 31u + (unsigned char)*p;
    return k_colors[h % k_ncolors];
}

/* ---- QR code renderer ---- */

static void draw_qr(const char *text)
{
    uint8_t qrcode[qrcodegen_BUFFER_LEN_MAX];
    uint8_t temp[qrcodegen_BUFFER_LEN_MAX];
    if (!qrcodegen_encodeText(text, temp, qrcode,
                              qrcodegen_Ecc_MEDIUM,
                              qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                              qrcodegen_Mask_AUTO, true))
        return;

    int sz     = qrcodegen_getSize(qrcode);
    int scale  = 5;
    int border = 4;
    float img  = (float)((sz + 2 * border) * scale);

    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + img, pos.y + img),
                      IM_COL32(255, 255, 255, 255));
    for (int y = 0; y < sz; y++) {
        for (int x = 0; x < sz; x++) {
            if (qrcodegen_getModule(qrcode, x, y)) {
                float x0 = pos.x + (float)((border + x) * scale);
                float y0 = pos.y + (float)((border + y) * scale);
                dl->AddRectFilled(ImVec2(x0, y0),
                                  ImVec2(x0 + scale, y0 + scale),
                                  IM_COL32(0, 0, 0, 255));
            }
        }
    }
    ImGui::Dummy(ImVec2(img, img));
}

/* ---- modals ---- */

static void draw_modals(void)
{
    /* New Chat */
    if (g_show_new_modal) { ImGui::OpenPopup("New Chat"); g_show_new_modal = false; }
    ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_Always);
    if (ImGui::BeginPopupModal("New Chat", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 16));
        ImGui::Spacing();
        ImGui::TextUnformatted("Chat name");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##newname", g_new_name, sizeof(g_new_name));
        ImGui::Spacing();
        bool create = ImGui::Button("Create", ImVec2(160, 0));
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(160, 0))) ImGui::CloseCurrentPopup();
        if (create && g_new_name[0] != '\0') {
            proto_chat tmp = {};
            char out_key[ID_BYTES * 2 + 1];
            char out_id[ID_BYTES * 2 + 1];
            if (proto_initialize(&tmp, out_key, out_id) == 0) {
                proto_save_chat(&tmp, g_new_name, g_basedir);
                dht_client_add_chat(tmp.user_key);
                proto_chat_cleanup(&tmp);
                snprintf(g_credentials, sizeof(g_credentials), "%s%s", out_key, out_id);
                g_show_cred_modal = true;
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }

    /* Credentials */
    if (g_show_cred_modal) { ImGui::OpenPopup("Credentials"); g_show_cred_modal = false; }
    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_Always);
    if (ImGui::BeginPopupModal("Credentials", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 16));
        ImGui::Spacing();
        ImGui::TextUnformatted("Share this token with the other device:");
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.12f, 0.12f, 0.16f, 1.f));
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##cred", g_credentials, sizeof(g_credentials),
                         ImGuiInputTextFlags_ReadOnly);
        ImGui::PopStyleColor();
        ImGui::Spacing();
        if (ImGui::Button("Copy to clipboard", ImVec2(200, 0)))
            ImGui::SetClipboardText(g_credentials);
        ImGui::SameLine();
        if (ImGui::Button("OK", ImVec2(-1, 0))) ImGui::CloseCurrentPopup();
        ImGui::Spacing();
        draw_qr(g_credentials);
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }

    /* Join Chat */
    if (g_show_join_modal) { ImGui::OpenPopup("Join Chat"); g_show_join_modal = false; }
    ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_Always);
    if (ImGui::BeginPopupModal("Join Chat", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 16));
        ImGui::Spacing();
        ImGui::TextUnformatted("Chat name");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##joinname", g_join_name, sizeof(g_join_name));
        ImGui::Spacing();
        ImGui::TextUnformatted("Token");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##joincmd", g_join_cmd, sizeof(g_join_cmd));
        ImGui::Spacing();
        bool join = ImGui::Button("Join", ImVec2(160, 0));
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(160, 0))) ImGui::CloseCurrentPopup();
        if (join && g_join_name[0] != '\0' && g_join_cmd[0] != '\0') {
            if (strlen(g_join_cmd) == ID_BYTES * 4) {
                char key[ID_BYTES * 2 + 1];
                char id[ID_BYTES * 2 + 1];
                memcpy(key, g_join_cmd, ID_BYTES * 2);
                key[ID_BYTES * 2] = '\0';
                memcpy(id, g_join_cmd + ID_BYTES * 2, ID_BYTES * 2);
                id[ID_BYTES * 2] = '\0';
                proto_chat tmp = {};
                proto_join(&tmp, key, id);
                proto_save_chat(&tmp, g_join_name, g_basedir);
                dht_client_add_chat(tmp.user_key);
                proto_chat_cleanup(&tmp);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }
}

/* ---- message bubble ---- */

static void draw_bubble(ImDrawList *dl, const char *text, bool is_own, ImVec4 sender_color)
{
    const float pad       = 10.f;
    const float margin    = 12.f;
    const float dot_r     = 5.f;
    const float rounding  = 10.f;
    const float avail     = ImGui::GetContentRegionAvail().x;
    const float max_bw    = avail * 0.68f;
    const float wrap_w    = max_bw - pad * 2.f;

    ImVec2 tsz = ImGui::CalcTextSize(text, nullptr, false, wrap_w);
    float bw = tsz.x + pad * 2.f;
    if (bw > max_bw) bw = max_bw;
    float bh = tsz.y + pad * 2.f;

    float cursor_x;
    if (is_own) {
        cursor_x = avail - bw - margin;
    } else {
        cursor_x = margin + dot_r * 2.f + 6.f;
    }

    /* draw sender dot for others */
    if (!is_own) {
        float cy = ImGui::GetCursorPosY();
        ImVec2 win_pos = ImGui::GetWindowPos();
        float scroll_y = ImGui::GetScrollY();
        ImVec2 dot_center = ImVec2(
            win_pos.x + margin + dot_r,
            win_pos.y + cy + dot_r + pad - scroll_y
        );
        dl->AddCircleFilled(dot_center, dot_r,
            ImGui::ColorConvertFloat4ToU32(sender_color));
    }

    /* draw bubble background */
    {
        float cy = ImGui::GetCursorPosY();
        ImVec2 win_pos = ImGui::GetWindowPos();
        float scroll_y = ImGui::GetScrollY();
        ImVec2 bmin = ImVec2(win_pos.x + cursor_x, win_pos.y + cy - scroll_y);
        ImVec2 bmax = ImVec2(bmin.x + bw, bmin.y + bh);

        ImU32 bg;
        if (is_own)
            bg = IM_COL32(22, 100, 140, 255);
        else
            bg = IM_COL32(32, 34, 44, 255);
        dl->AddRectFilled(bmin, bmax, bg, rounding);
    }

    /* render text */
    ImGui::SetCursorPosX(cursor_x + pad);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + pad);
    ImGui::PushTextWrapPos(cursor_x + pad + wrap_w);
    if (is_own)
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.88f, 0.94f, 1.00f, 1.f));
    else
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.88f, 0.88f, 0.92f, 1.f));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();

    /* advance past bubble bottom */
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + pad + 6.f);
    ImGui::SetCursorPosX(0.f);
}

/* ---- sidebar ---- */

static void draw_sidebar(float width, float height)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.f, 12.f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.08f, 0.10f, 1.f));
    ImGui::BeginChild("##sidebar", ImVec2(width, height), false);

    /* app name */
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.20f, 0.76f, 0.96f, 1.f));
    ImGui::Text("DUI");
    ImGui::PopStyleColor();
    ImGui::Spacing();

    /* search */
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##search", "Search...", g_search, sizeof(g_search));
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    /* chat list */
    char names[MAX_CHATS][64];
    int count = scan_chats(names);

    /* mark unread for non-active chats when sync delivered */
    if (g_sync_delivered.exchange(false)) {
        for (int i = 0; i < count; i++) {
            if (strcmp(names[i], g_active_chat) != 0)
                set_unread(names[i]);
        }
    }

    float list_h = height - ImGui::GetCursorPosY() - 52.f; /* reserve for buttons */
    ImGui::BeginChild("##chatlist", ImVec2(-1, list_h), false);

    bool any = false;
    for (int i = 0; i < count; i++) {
        if (g_search[0] != '\0') {
            /* case-insensitive substring filter */
            bool match = false;
            const char *h = names[i], *n = g_search;
            while (*h) {
                const char *hp = h, *np = n;
                while (*hp && *np && ((*hp | 32) == (*np | 32))) { hp++; np++; }
                if (*np == '\0') { match = true; break; }
                h++;
            }
            if (!match) continue;
        }
        any = true;

        bool selected = (strcmp(names[i], g_active_chat) == 0);
        bool unread   = has_unread(names[i]);

        /* highlight selected */
        if (selected)
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.14f, 0.52f, 0.74f, 1.f));

        char label[96];
        snprintf(label, sizeof(label), "##chat_%d", i);

        float item_h = ImGui::GetFrameHeightWithSpacing();
        ImVec2 item_pos = ImGui::GetCursorScreenPos();

        if (ImGui::Selectable(label, selected, 0, ImVec2(0, item_h))) {
            open_chat(names[i]);
        }

        /* overlay: name + unread dot */
        ImDrawList *dl = ImGui::GetWindowDrawList();
        float text_y = item_pos.y + (item_h - ImGui::GetTextLineHeight()) * 0.5f;
        dl->AddText(ImVec2(item_pos.x + 6, text_y),
                    IM_COL32(220, 220, 228, 255), names[i]);
        if (unread) {
            dl->AddCircleFilled(
                ImVec2(item_pos.x + ImGui::GetContentRegionAvail().x - 2.f,
                       item_pos.y + item_h * 0.5f),
                5.f, IM_COL32(32, 192, 240, 255));
        }

        if (selected)
            ImGui::PopStyleColor();
    }

    if (!any)
        ImGui::TextDisabled(count == 0 ? "No chats yet." : "No matches.");

    ImGui::EndChild();

    /* bottom buttons */
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    float bw = (width - 24.f - ImGui::GetStyle().ItemSpacing.x * 2.f) / 3.f;
    if (ImGui::Button("New",  ImVec2(bw, 0))) { g_show_new_modal  = true; g_new_name[0] = '\0'; }
    ImGui::SameLine();
    if (ImGui::Button("Join", ImVec2(bw, 0))) { g_show_join_modal = true; g_join_name[0] = '\0'; g_join_cmd[0] = '\0'; }
    ImGui::SameLine();
    if (ImGui::Button("Quit", ImVec2(bw, 0))) { g_quit_requested = true; }

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

/* ---- chat panel ---- */

static void draw_chat_panel(float height)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    ImGui::BeginChild("##chatpanel", ImVec2(0, height), false);

    if (!g_chat_open) {
        /* empty state */
        float w = ImGui::GetContentRegionAvail().x;
        float h = ImGui::GetContentRegionAvail().y;
        ImGui::SetCursorPos(ImVec2(w * 0.5f - 120.f, h * 0.5f - 10.f));
        ImGui::TextDisabled("Select or create a chat to start.");
        ImGui::EndChild();
        ImGui::PopStyleVar();
        return;
    }

    /* header */
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.f, 10.f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.12f, 0.16f, 1.f));
    ImGui::BeginChild("##chatheader", ImVec2(0, 42.f), false);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.95f, 1.00f, 1.f));
    ImGui::Text("%s", g_active_chat);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    float cred_btn_w = 100.f;
    ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - cred_btn_w + 14.f);
    if (ImGui::Button("Credentials", ImVec2(cred_btn_w, 0)))
        g_show_cred_modal = true;
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    /* messages area */
    float input_h = ImGui::GetFrameHeightWithSpacing() + 20.f;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 8.f));
    ImGui::BeginChild("##msgs", ImVec2(0, -input_h), false);

    const proto_messages *msgs = proto_list(&g_chat);
    if (msgs && msgs->count > 0) {
        color_table_reset();
        ImDrawList *dl = ImGui::GetWindowDrawList();
        for (int i = 0; i < msgs->count; i++) {
            bool is_own = (strcmp(msgs->entity_ids[i], g_chat.entity_id) == 0);
            ImVec4 col = color_for(msgs->entity_ids[i]);
            draw_bubble(dl, msgs->texts[i], is_own, col);
        }
    }

    if (g_scroll_to_bottom) {
        ImGui::SetScrollHereY(1.0f);
        g_scroll_to_bottom = false;
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();

    /* input row */
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.f, 8.f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.12f, 0.16f, 1.f));
    ImGui::BeginChild("##inputrow", ImVec2(0, 0), false);

    float send_w = 64.f;
    float input_w = ImGui::GetContentRegionAvail().x - send_w - ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetNextItemWidth(input_w);

    bool enter = ImGui::InputText("##msginput", g_msg_input, sizeof(g_msg_input),
                                  ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    bool send = ImGui::Button("Send", ImVec2(send_w, 0));

    if ((enter || send) && g_msg_input[0] != '\0') {
        proto_send(&g_chat, g_msg_input);
        g_msg_input[0] = '\0';
        g_scroll_to_bottom = true;
        ImGui::SetKeyboardFocusHere(-1);
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    ImGui::EndChild();
    ImGui::PopStyleVar();
}

/* ---- per-frame entry point ---- */

void app_frame(void)
{
    begin_fullscreen("##root");

    ImGuiIO &io = ImGui::GetIO();
    float total_h = io.DisplaySize.y;
    float status_h = ImGui::GetFrameHeightWithSpacing() + 4.f;
    float content_h = total_h - status_h;
    float sidebar_w = 260.f;

    /* vertical divider */
    ImDrawList *bg = ImGui::GetWindowDrawList();
    bg->AddLine(
        ImVec2(sidebar_w, 0),
        ImVec2(sidebar_w, content_h),
        IM_COL32(40, 40, 52, 255), 1.f);

    draw_sidebar(sidebar_w, content_h);

    ImGui::SameLine(0, 1.f);

    draw_chat_panel(content_h);

    /* status bar */
    ImGui::SetCursorPosY(content_h);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.07f, 0.07f, 0.09f, 1.f));
    ImGui::BeginChild("##statusbar", ImVec2(0, 0), false);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.f, 4.f));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.38f, 0.38f, 0.48f, 1.f));
    ImGui::Text("Live  |  v" DUI_VERSION);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleColor();

    draw_modals();

    ImGui::End();
}
