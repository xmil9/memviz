// MemViz - live 1KB-block memory map of a running Windows process.
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "gfx.h"
#include "memscan.h"
#include "model.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

using clock_t2 = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// Shared state between the UI thread and the scanning worker.
// ---------------------------------------------------------------------------
struct Shared {
    std::mutex mtx;

    // requests: UI -> worker
    DWORD reqPid = 0;
    bool  reqAttach = false;
    bool  reqDetach = false;
    bool  reqRefreshOnce = false;
    model::ColorMode mode = model::ColorMode::Metadata;
    int   gridWidth = 512;
    int   refreshMs = 1000;
    bool  autoRefresh = true;
    uint64_t contentBudget = 256ull << 20;  // cap bytes read per content pass

    // results: worker -> UI
    std::shared_ptr<const model::Grid> grid;
    uint64_t     gridVersion = 0;
    std::string  status = "Not attached";
    DWORD        attachedPid = 0;
    std::wstring attachedName;
    HANDLE       handle = nullptr;  // read under mtx for on-demand byte reads

    std::atomic<bool> running{true};
};

static Shared g_shared;

static std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static double msSince(clock_t2::time_point t) {
    return std::chrono::duration<double, std::milli>(clock_t2::now() - t).count();
}

// ---------------------------------------------------------------------------
// Worker thread: attach, enumerate regions, build grid, sample content.
// ---------------------------------------------------------------------------
static void workerMain(Shared* S) {
    HANDLE h = nullptr;
    std::shared_ptr<const model::Grid> prev;
    uintptr_t lo = 0, hi = 0;
    uint32_t gran = 0;
    memscan::appAddressRange(lo, hi, gran);
    auto lastScan = clock_t2::now() - std::chrono::hours(1);

    while (S->running.load()) {
        DWORD pid; bool attach, detach, once, autoR;
        model::ColorMode mode; int width, rms; uint64_t budget;
        {
            std::lock_guard<std::mutex> lk(S->mtx);
            pid = S->reqPid; attach = S->reqAttach; detach = S->reqDetach;
            once = S->reqRefreshOnce; autoR = S->autoRefresh; mode = S->mode;
            width = S->gridWidth; rms = S->refreshMs; budget = S->contentBudget;
            S->reqAttach = S->reqDetach = S->reqRefreshOnce = false;
        }

        if (detach) {
            if (h) { CloseHandle(h); h = nullptr; }
            prev.reset();
            std::lock_guard<std::mutex> lk(S->mtx);
            S->handle = nullptr; S->attachedPid = 0; S->attachedName.clear();
            S->grid.reset(); S->gridVersion++; S->status = "Detached";
        }

        if (attach) {
            if (h) { CloseHandle(h); h = nullptr; }
            h = memscan::openProcess(pid);
            prev.reset();
            if (h) {
                std::wstring name = memscan::processName(pid);
                std::lock_guard<std::mutex> lk(S->mtx);
                S->handle = h; S->attachedPid = pid; S->attachedName = name;
                S->status = "Attached to " + narrow(name) + " (pid " + std::to_string(pid) + ")";
            } else {
                DWORD e = GetLastError();
                std::lock_guard<std::mutex> lk(S->mtx);
                S->handle = nullptr; S->attachedPid = 0; S->attachedName.clear();
                S->status = "OpenProcess failed (err " + std::to_string(e) +
                            ") - try running MemViz as administrator";
            }
            once = true;
            lastScan = clock_t2::now() - std::chrono::hours(1);
        }

        bool doScan = h && (once || (autoR && msSince(lastScan) >= rms));
        if (doScan) {
            auto regions = memscan::enumRegions(h, lo, hi);
            auto mods = memscan::enumModules(h);
            memscan::annotateModules(regions, mods);
            auto grid = model::buildGrid(regions, width);
            if (mode == model::ColorMode::Content) {
                model::fillContent(h, *grid, budget);
                if (prev) model::markChanges(*prev, *grid);
            }
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "%zu regions | committed %.1f MB | reserved %.1f MB",
                          grid->stats.regionCount,
                          grid->stats.committedBytes / 1048576.0,
                          grid->stats.reservedBytes / 1048576.0);
            {
                std::lock_guard<std::mutex> lk(S->mtx);
                S->grid = grid; S->gridVersion++;
                S->status = buf;
            }
            prev = grid;
            lastScan = clock_t2::now();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (h) CloseHandle(h);
}

// ---------------------------------------------------------------------------
// UI helpers
// ---------------------------------------------------------------------------
static size_t findRegion(const model::Grid& g, uintptr_t addr) {
    const auto& R = g.regions;
    size_t lo = 0, hi = R.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (R[mid].base <= addr) lo = mid + 1;
        else hi = mid;
    }
    if (lo == 0) return SIZE_MAX;
    size_t idx = lo - 1;
    if (addr >= R[idx].base && addr < R[idx].base + R[idx].size) return idx;
    return SIZE_MAX;
}

static void colorSwatch(uint32_t c) {
    ImU32 col = (ImU32)c;  // model rgba() packing matches ImGui IM_COL32 order
    ImGui::ColorButton("##sw", ImGui::ColorConvertU32ToFloat4(col),
                       ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                       ImVec2(14, 14));
    ImGui::SameLine();
}

// UI-thread state
static std::vector<memscan::ProcessInfo> g_procList;
static char     g_filter[128] = "";
static DWORD    g_selPid = 0;
static float    g_zoom = 2.0f;
static int      g_texCellPx = 1;   // pixels per grid cell in the uploaded texture
static uint64_t g_texSourceVersion = 0;
static std::vector<uint32_t> g_upscaledPixels;
static bool     g_hasSel = false;
static uintptr_t g_selAddr = 0;

static void refreshProcessList() { g_procList = memscan::listProcesses(); }

static int computeCellPx(float zoom, const model::Grid& grid) {
    int cp = (zoom >= 1.0f) ? (int)std::ceil(zoom) : 1;
    int maxCp = model::maxCellPixels(grid.width, grid.height);
    if (cp > maxCp) cp = maxCp;
    return std::max(1, cp);
}

static void refreshGridTexture(const std::shared_ptr<const model::Grid>& grid,
                               gfx::Texture& tex, float zoom) {
    if (!grid || grid->totalCells == 0) {
        tex.release();
        g_texCellPx = 0;
        return;
    }
    g_texCellPx = computeCellPx(zoom, *grid);
    if (g_texCellPx == 1) {
        tex.update(grid->pixels.data(), grid->width, grid->height);
        return;
    }
    int dstW = 0, dstH = 0;
    model::upscalePixels(grid->pixels, grid->width, grid->height,
                         g_texCellPx, g_upscaledPixels, dstW, dstH);
    tex.update(g_upscaledPixels.data(), dstW, dstH);
}

static void imguiBindPointSampler(const ImDrawList*, const ImDrawCmd*) {
    gfx::bindPointSampler();
}

static void imguiBindLinearSampler(const ImDrawList*, const ImDrawCmd*) {
    gfx::bindLinearSampler();
}

static void drawToolbar(Shared& S) {
    int mode = (int)S.mode;
    ImGui::SetNextItemWidth(140);
    if (ImGui::Combo("Color", &mode, "Region metadata\0Byte content\0")) {
        std::lock_guard<std::mutex> lk(S.mtx);
        S.mode = (model::ColorMode)mode;
        S.reqRefreshOnce = true;
    }
    ImGui::SameLine();

    static const int widths[] = {256, 512, 1024};
    int wsel = (S.gridWidth == 256) ? 0 : (S.gridWidth == 1024 ? 2 : 1);
    ImGui::SetNextItemWidth(110);
    if (ImGui::Combo("Row width", &wsel, "256\0" "512\0" "1024\0\0")) {
        std::lock_guard<std::mutex> lk(S.mtx);
        S.gridWidth = widths[wsel];
        S.reqRefreshOnce = true;
    }
    ImGui::SameLine();

    bool autoR = S.autoRefresh;
    if (ImGui::Checkbox("Auto", &autoR)) {
        std::lock_guard<std::mutex> lk(S.mtx);
        S.autoRefresh = autoR;
    }
    ImGui::SameLine();
    int rms = S.refreshMs;
    ImGui::SetNextItemWidth(160);
    if (ImGui::SliderInt("Interval (ms)", &rms, 100, 5000)) {
        std::lock_guard<std::mutex> lk(S.mtx);
        S.refreshMs = rms;
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh now")) {
        std::lock_guard<std::mutex> lk(S.mtx);
        S.reqRefreshOnce = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    ImGui::SliderFloat("Zoom", &g_zoom, 0.2f, 40.0f, "%.1fx");
    ImGui::SameLine();
    if (ImGui::Button("Detach")) {
        std::lock_guard<std::mutex> lk(S.mtx);
        S.reqDetach = true;
    }
}

static void drawProcesses(Shared& S) {
    ImGui::TextUnformatted("Processes");
    if (ImGui::Button("Refresh list")) refreshProcessList();
    ImGui::SameLine();
    if (ImGui::Button("Attach") && g_selPid) {
        std::lock_guard<std::mutex> lk(S.mtx);
        S.reqPid = g_selPid;
        S.reqAttach = true;
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##filter", "filter by name or pid", g_filter, sizeof(g_filter));

    ImGui::BeginChild("proclist", ImVec2(0, 0), ImGuiChildFlags_Border);
    char lower[128];
    for (int i = 0; g_filter[i] && i < 127; ++i) lower[i] = (char)tolower(g_filter[i]), lower[i + 1] = 0;
    if (!g_filter[0]) lower[0] = 0;

    for (const auto& p : g_procList) {
        std::string name = narrow(p.name);
        if (lower[0]) {
            std::string ln = name;
            for (auto& c : ln) c = (char)tolower(c);
            std::string pidstr = std::to_string(p.pid);
            if (ln.find(lower) == std::string::npos && pidstr.find(lower) == std::string::npos)
                continue;
        }
        char label[300];
        std::snprintf(label, sizeof(label), "%-6lu  %s", (unsigned long)p.pid, name.c_str());
        bool sel = (p.pid == g_selPid);
        if (ImGui::Selectable(label, sel, ImGuiSelectableFlags_AllowDoubleClick)) {
            g_selPid = p.pid;
            if (ImGui::IsMouseDoubleClicked(0)) {
                std::lock_guard<std::mutex> lk(S.mtx);
                S.reqPid = p.pid;
                S.reqAttach = true;
            }
        }
    }
    ImGui::EndChild();
}

static void drawCanvas(Shared& S, const std::shared_ptr<const model::Grid>& grid, gfx::Texture& tex) {
    if (!grid || grid->totalCells == 0 || tex.width() == 0) {
        ImGui::TextDisabled("Attach to a process to view its memory map.");
        return;
    }

    ImVec2 size((float)grid->width * g_zoom, (float)grid->height * g_zoom);
    ImGui::InvisibleButton("##map", size);
    bool hovered = ImGui::IsItemHovered();
    ImVec2 origin = ImGui::GetItemRectMin();

    if (ImGui::IsItemVisible()) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 end(origin.x + size.x, origin.y + size.y);
        dl->AddCallback(imguiBindPointSampler, nullptr);
        dl->AddImage(tex.id(), origin, end);
        dl->AddCallback(imguiBindLinearSampler, nullptr);
    }

    ImGuiIO& io = ImGui::GetIO();
    const float cellSize = g_zoom;

    // Wheel zoom while hovering the map.
    if (hovered && io.MouseWheel != 0.0f) {
        g_zoom *= (1.0f + 0.15f * io.MouseWheel);
        if (g_zoom < 0.2f) g_zoom = 0.2f;
        if (g_zoom > 40.0f) g_zoom = 40.0f;
    }
    // Right-drag to pan.
    if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
        ImGui::SetScrollX(ImGui::GetScrollX() - io.MouseDelta.x);
        ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
    }

    if (hovered) {
        int cx = (int)((io.MousePos.x - origin.x) / cellSize);
        int cy = (int)((io.MousePos.y - origin.y) / cellSize);
        size_t idx = model::cellIndex(*grid, cx, cy);
        if (idx != SIZE_MAX) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            // outline hovered cell
            ImVec2 a(origin.x + cx * cellSize, origin.y + cy * cellSize);
            ImVec2 b(a.x + cellSize, a.y + cellSize);
            dl->AddRect(a, b, IM_COL32(255, 255, 255, 220));

            size_t ri; uintptr_t addr;
            if (model::resolveCell(*grid, idx, ri, addr)) {
                const auto& r = grid->regions[ri];
                ImGui::BeginTooltip();
                ImGui::Text("0x%016llX", (unsigned long long)addr);
                ImGui::Text("cell = %zu KB", grid->cellBytes() / 1024);
                ImGui::Separator();
                ImGui::Text("region base 0x%016llX", (unsigned long long)r.base);
                ImGui::Text("region size %.2f KB", r.size / 1024.0);
                ImGui::Text("state   %s", memscan::stateStr(r.state));
                ImGui::Text("protect %s", memscan::protectStr(r.protect).c_str());
                ImGui::Text("type    %s", memscan::typeStr(r.type).c_str());
                if (!r.module.empty())
                    ImGui::Text("module  %s", narrow(r.module).c_str());
                ImGui::EndTooltip();

                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    g_hasSel = true;
                    g_selAddr = addr;
                }
            }
        }
    }

    // Persistent marker for the selected cell.
    if (g_hasSel) {
        size_t ri = findRegion(*grid, g_selAddr);
        if (ri != SIZE_MAX) {
            size_t offCells = (g_selAddr - grid->regions[ri].base) / grid->cellBytes();
            size_t idx = grid->regionCellStart[ri] + offCells;
            if (idx < grid->totalCells) {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                int cx = (int)(idx % grid->width);
                int cy = (int)(idx / grid->width);
                ImVec2 a(origin.x + cx * cellSize, origin.y + cy * cellSize);
                ImVec2 b(a.x + cellSize, a.y + cellSize);
                dl->AddRect(a, b, IM_COL32(255, 80, 80, 255), 0, 0, 2.0f);
            }
        }
    }
}

static void drawDetails(Shared& S, const std::shared_ptr<const model::Grid>& grid) {
    ImGui::TextUnformatted("Legend");
    ImGui::Separator();
    auto legend = (S.mode == model::ColorMode::Content) ? model::contentLegend()
                                                        : model::metadataLegend();
    for (const auto& it : legend) {
        colorSwatch(it.color);
        ImGui::TextUnformatted(it.label);
    }

    ImGui::Dummy(ImVec2(0, 6));
    ImGui::TextUnformatted("Statistics");
    ImGui::Separator();
    if (grid) {
        ImGui::Text("Regions:   %zu", grid->stats.regionCount);
        ImGui::Text("Committed: %.1f MB", grid->stats.committedBytes / 1048576.0);
        ImGui::Text("Reserved:  %.1f MB", grid->stats.reservedBytes / 1048576.0);
        ImGui::Text("Cell size: %zu KB", grid->cellBytes() / 1024);
        ImGui::Text("Grid:      %d x %d", grid->width, grid->height);
        if (grid->mode == model::ColorMode::Content) {
            ImGui::Text("Sampled:   %.1f MB", grid->stats.sampledBytes / 1048576.0);
            ImGui::Text("Changed:   %zu cells", grid->stats.changedCells);
        }
    } else {
        ImGui::TextDisabled("No data.");
    }

    ImGui::Dummy(ImVec2(0, 6));
    ImGui::TextUnformatted("Selected block");
    ImGui::Separator();
    if (!g_hasSel || !grid) {
        ImGui::TextDisabled("Click a cell to inspect it.");
        return;
    }

    ImGui::Text("Address: 0x%016llX", (unsigned long long)g_selAddr);
    size_t ri = findRegion(*grid, g_selAddr);
    if (ri != SIZE_MAX) {
        const auto& r = grid->regions[ri];
        ImGui::Text("State:   %s", memscan::stateStr(r.state));
        ImGui::Text("Protect: %s", memscan::protectStr(r.protect).c_str());
        ImGui::Text("Type:    %s", memscan::typeStr(r.type).c_str());
        if (!r.module.empty()) ImGui::Text("Module:  %s", narrow(r.module).c_str());
    }

    // On-demand read of the selected block for metrics + hex preview.
    const size_t previewBytes = 256;
    std::vector<uint8_t> data(previewBytes);
    size_t got = 0;
    {
        std::lock_guard<std::mutex> lk(S.mtx);
        if (S.handle) got = memscan::readMemory(S.handle, g_selAddr, data.data(), previewBytes);
    }
    if (got == 0) {
        ImGui::TextDisabled("Bytes not readable.");
        return;
    }
    model::BlockMetrics m = model::analyzeBytes(data.data(), got);
    ImGui::Text("Entropy:   %.3f", m.entropy);
    ImGui::Text("Zero ratio: %.3f", m.zeroRatio);

    ImGui::Dummy(ImVec2(0, 4));
    ImGui::TextUnformatted("Hex preview:");
    ImGui::BeginChild("hex", ImVec2(0, 160), ImGuiChildFlags_Border,
                      ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushFont(nullptr);
    for (size_t off = 0; off < got; off += 16) {
        char line[128];
        int n = std::snprintf(line, sizeof(line), "%04zX  ", off);
        for (size_t i = 0; i < 16 && off + i < got; ++i)
            n += std::snprintf(line + n, sizeof(line) - n, "%02X ", data[off + i]);
        n += std::snprintf(line + n, sizeof(line) - n, " ");
        for (size_t i = 0; i < 16 && off + i < got; ++i) {
            uint8_t c = data[off + i];
            n += std::snprintf(line + n, sizeof(line) - n, "%c",
                               (c >= 32 && c < 127) ? c : '.');
        }
        ImGui::TextUnformatted(line);
    }
    ImGui::PopFont();
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Win32 plumbing
// ---------------------------------------------------------------------------
static UINT g_resizeW = 0, g_resizeH = 0;

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) {
                g_resizeW = (UINT)LOWORD(lParam);
                g_resizeH = (UINT)HIWORD(lParam);
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    memscan::enableSeDebug();

    WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, WndProc, 0, 0, hInst,
                      nullptr, nullptr, nullptr, nullptr, L"MemVizWnd", nullptr};
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"MemViz - Memory Visualizer",
                              WS_OVERLAPPEDWINDOW, 80, 60, 1360, 860,
                              nullptr, nullptr, wc.hInstance, nullptr);

    if (!gfx::createDevice(hwnd)) {
        gfx::cleanupDevice();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        MessageBoxW(nullptr, L"Failed to create D3D11 device.", L"MemViz", MB_ICONERROR);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;  // don't litter an imgui.ini next to the exe
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(gfx::device(), gfx::context());

    refreshProcessList();
    std::thread worker(workerMain, &g_shared);

    gfx::Texture gridTex;
    std::shared_ptr<const model::Grid> uiGrid;
    uint64_t uiVersion = 0;

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (g_resizeW != 0 && g_resizeH != 0) {
            gfx::resizeBuffers(g_resizeW, g_resizeH);
            g_resizeW = g_resizeH = 0;
        }

        // Pull the latest published grid and refresh the display texture when the
        // grid or zoom bucket (integer pixels-per-cell) changes.
        std::string status;
        DWORD attachedPid;
        {
            std::lock_guard<std::mutex> lk(g_shared.mtx);
            if (g_shared.gridVersion != uiVersion) {
                uiGrid = g_shared.grid;
                uiVersion = g_shared.gridVersion;
            }
            status = g_shared.status;
            attachedPid = g_shared.attachedPid;
        }
        if (uiGrid && uiGrid->totalCells) {
            int wantCellPx = computeCellPx(g_zoom, *uiGrid);
            if (uiVersion != g_texSourceVersion || wantCellPx != g_texCellPx)
                refreshGridTexture(uiGrid, gridTex, g_zoom);
            g_texSourceVersion = uiVersion;
        } else if (gridTex.width() != 0) {
            gridTex.release();
            g_texCellPx = 0;
            g_texSourceVersion = 0;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoBringToFrontOnFocus |
                                 ImGuiWindowFlags_NoSavedSettings;
        ImGui::Begin("MemViz", nullptr, flags);

        drawToolbar(g_shared);
        ImGui::Separator();

        float statusH = ImGui::GetFrameHeightWithSpacing();
        float rowH = ImGui::GetContentRegionAvail().y - statusH;
        const float leftW = 300.0f, rightW = 340.0f;
        float centerW = ImGui::GetContentRegionAvail().x - leftW - rightW - 16.0f;
        if (centerW < 100.0f) centerW = 100.0f;

        ImGui::BeginChild("left", ImVec2(leftW, rowH), ImGuiChildFlags_Border);
        drawProcesses(g_shared);
        ImGui::EndChild();
        ImGui::SameLine();

        ImGui::BeginChild("center", ImVec2(centerW, rowH), ImGuiChildFlags_Border,
                          ImGuiWindowFlags_HorizontalScrollbar);
        drawCanvas(g_shared, uiGrid, gridTex);
        ImGui::EndChild();
        ImGui::SameLine();

        ImGui::BeginChild("right", ImVec2(rightW, rowH), ImGuiChildFlags_Border);
        drawDetails(g_shared, uiGrid);
        ImGui::EndChild();

        ImGui::Separator();
        if (attachedPid)
            ImGui::Text("[pid %lu]  %s", (unsigned long)attachedPid, status.c_str());
        else
            ImGui::TextUnformatted(status.c_str());

        ImGui::End();

        ImGui::Render();
        const float clear[4] = {0.06f, 0.06f, 0.08f, 1.0f};
        ID3D11RenderTargetView* rtv = gfx::renderTargetView();
        gfx::context()->OMSetRenderTargets(1, &rtv, nullptr);
        gfx::context()->ClearRenderTargetView(rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        gfx::present(true);
    }

    g_shared.running.store(false);
    worker.join();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    gfx::cleanupDevice();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
