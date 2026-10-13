// ============================================================================
// 虚拟内存实时监控可视化窗口（仅 Debug）
//
// 用法：
//   1) 分配器初始化时调用 Attach(base, size) + SetBlockQuery(fn)
//   2) 渲染循环里，ImGui::NewFrame() 之后、ImGui::Render() 之前：
//          MemDebugView::Get().Draw();
//
// 功能：
//   - 标签页：Heatmap / Hex View / Block View
//   - Hex 视图：左边 hex 数值，右边 ASCII（类 hex 查看器）
//   - 字节变化：old -> new，附 ASCII
//   - 块级视图：按 used/free 着色 + 块列表
//   - Auto Refresh 复选框；关闭后仅点击“Capture Memory”按钮才刷新
// ============================================================================
#pragma once

// 调试开关
#ifndef MEM_DEBUG_VIEW_ENABLED
// #define MEM_DEBUG_VIEW_ENABLED 1
#endif

#if defined(_MSC_VER)
#  include <intrin.h>
#endif

#ifdef MEM_DEBUG_VIEW_ENABLED

#include <imgui.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <vector>
#include <chrono>
#include <algorithm>
#include <functional>
#include <climits>

class MemDebugView {
public:
    struct BlockEntry {
        uintptr_t addr;
        size_t    total_size;
        size_t    payload_size;
        bool      is_free;
    };

    static MemDebugView& Get() {
        static MemDebugView inst;
        return inst;
    }

    using BlockQueryFn = std::function<void(std::vector<BlockEntry>&)>;

    // ---------- 生命周期 ----------
    void Attach(void* base, size_t size) {
        m_base = static_cast<uint8_t*>(base);
        m_size = size;
        if (!m_base || m_size == 0) { m_active = false; return; }

        m_buf[0].assign(m_size, 0);
        m_buf[1].assign(m_size, 0);
        std::memcpy(m_buf[0].data(), m_base, m_size);
        std::memcpy(m_buf[1].data(), m_base, m_size);
        m_cur = 0;

        m_cellShift = 0;
        while ((size_t(1) << m_cellShift) * kMaxCells < m_size)
            ++m_cellShift;
        m_cellSize = size_t(1) << m_cellShift;
        m_cellCount = (m_size + m_cellSize - 1) / m_cellSize;
        if (m_cellCount > kMaxCells) m_cellCount = kMaxCells;

        std::fill(m_heat, m_heat + kMaxCells, 0.0f);
        m_changedBytes = 0;
        m_prevChanged = 0;
        m_selectedCell = -1;
        m_active = true;
        m_open = true;
        m_autoRefresh = true;
        m_pendingSample = false;
        m_lastSample = Clock::now();
        m_pendingScrollLine = -1;
        m_lastSampleMs = 0.0f;
        m_blocks.clear();
    }

    void Detach() {
        m_active = false;
        m_base = nullptr;
        m_size = 0;
        m_buf[0].clear();
        m_buf[1].clear();
        m_blocks.clear();
        m_blockQuery = nullptr;
    }

    void SetBlockQuery(BlockQueryFn fn) { m_blockQuery = std::move(fn); }

    bool IsActive() const { return m_active; }
    bool IsOpen()   const { return m_open; }
    void SetOpen(bool v) { m_open = v; }

    // ---------- 每帧调用 ----------
    void Draw() {
        if (!m_active || !m_base) return;

        if (m_autoRefresh) SampleIfDue();
        if (m_pendingSample) { Sample(); m_pendingSample = false; }

        if (!m_open) return;

        ImGui::SetNextWindowSize(ImVec2(1100, 760), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Virtual Memory Monitor (Debug)", &m_open)) {
            ImGui::End();
            return;
        }

        DrawToolbar();
        ImGui::Separator();
        DrawStats();
        ImGui::Separator();

        if (ImGui::BeginTabBar("MemDebugTabs")) {
            if (ImGui::BeginTabItem("Heatmap")) {
                DrawHeatmap();
                ImGui::Spacing();
                ImGui::Separator();
                DrawChangeGroups();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Hex View")) {
                DrawHexView();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Block View")) {
                DrawBlockView();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::End();
    }

private:
    MemDebugView() = default;
    MemDebugView(const MemDebugView&) = delete;
    MemDebugView& operator=(const MemDebugView&) = delete;

    static constexpr size_t kMaxCells = 128;
    using Clock = std::chrono::steady_clock;

    uint8_t* m_base = nullptr;
    size_t   m_size = 0;
    bool     m_active = false;
    bool     m_open = true;

    std::vector<uint8_t> m_buf[2];
    int m_cur = 0;

    size_t m_cellSize = 1;
    size_t m_cellCount = 0;
    int    m_cellShift = 0;

    Clock::time_point m_lastSample{};
    int   m_sampleIntervalMs = 500;
    bool  m_autoRefresh = true;
    bool  m_pendingSample = false;
    float m_lastSampleMs = 0.0f;

    size_t m_changedBytes = 0;
    size_t m_prevChanged = 0;

    float m_heat[kMaxCells] = {};
    int   m_selectedCell = -1;

    std::vector<BlockEntry> m_blocks;
    BlockQueryFn            m_blockQuery;

    int m_pendingScrollLine = -1;

    // ---------- 采样 ----------
    void SampleIfDue() {
        auto now = Clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - m_lastSample).count();
        if (ms < m_sampleIntervalMs) return;
        m_lastSample = now;
        Sample();
    }

    void Sample() {
        auto t0 = Clock::now();
        int other = 1 - m_cur;
        std::memcpy(m_buf[other].data(), m_base, m_size);

        const uint8_t* oldb = m_buf[m_cur].data();
        const uint8_t* newb = m_buf[other].data();

        size_t changed = 0;
        std::fill(m_heat, m_heat + kMaxCells, 0.0f);

        for (size_t i = 0; i < m_size; ++i) {
            if (oldb[i] != newb[i]) {
                ++changed;
                size_t c = i >> m_cellShift;
                if (c < kMaxCells) m_heat[c] += 1.0f;
            }
        }
        for (size_t c = 0; c < kMaxCells; ++c) {
            m_heat[c] /= (float)m_cellSize;
            if (m_heat[c] > 1.0f) m_heat[c] = 1.0f;
        }

        m_prevChanged = m_changedBytes;
        m_changedBytes = changed;
        m_cur = other;

        auto t1 = Clock::now();
        m_lastSampleMs = std::chrono::duration<float, std::milli>(t1 - t0).count();
    }

    // ---------- UI ----------
    void DrawToolbar() {
        ImGui::Checkbox("Auto Refresh", &m_autoRefresh);
        ImGui::SameLine();
        if (ImGui::Button("Capture Memory")) m_pendingSample = true;
        ImGui::SameLine();

        ImGui::BeginDisabled(!m_autoRefresh);
        ImGui::SetNextItemWidth(180);
        ImGui::SliderInt("Sample (ms)", &m_sampleIntervalMs, 50, 2000);
        ImGui::EndDisabled();

        ImGui::Text("Base: %p   Size: %.2f MB   Last sample: %.1f ms",
            (void*)m_base, m_size / 1048576.0, m_lastSampleMs);
    }

    void DrawStats() {
        ImGui::Text("Changed bytes: %zu  (prev: %zu)   Heat cells: %zu x %zu B",
            m_changedBytes, m_prevChanged, m_cellCount, m_cellSize);
    }

    // ---- 热力图 ----
    void DrawHeatmap() {
        ImGui::Text("Change heatmap (click a cell to inspect):");

        ImVec2 avail = ImGui::GetContentRegionAvail();
        float  cellW = avail.x / (float)kMaxCells;
        float  cellH = 36.0f;

        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        for (size_t i = 0; i < kMaxCells; ++i) {
            float t = m_heat[i];
            ImU32 color;
            if (t <= 0.0f) {
                color = IM_COL32(45, 45, 45, 255);
            }
            else {
                int r = (int)(40 + 215 * t);
                int g = (int)(180 * (1.0f - t));
                int b = (int)(60 * (1.0f - t));
                color = IM_COL32(r, g, b, 255);
            }
            ImVec2 a(origin.x + i * cellW, origin.y);
            ImVec2 b(origin.x + (i + 1) * cellW, origin.y + cellH);
            dl->AddRectFilled(a, b, color);
            if ((int)i == m_selectedCell)
                dl->AddRect(a, b, IM_COL32(255, 255, 0, 255), 0, 0, 2.0f);
        }

        ImGui::InvisibleButton("##heatmap", ImVec2(avail.x, cellH));
        if (ImGui::IsItemClicked()) {
            ImVec2 mouse = ImGui::GetIO().MousePos;
            int idx = (int)((mouse.x - origin.x) / cellW);
            if (idx >= 0 && idx < (int)kMaxCells) m_selectedCell = idx;
        }
    }

    // ---- 字节变化（hex + ASCII） ----
    static void buildAscii(const uint8_t* p, size_t n, char* out, size_t cap) {
        size_t k = 0;
        for (; k < n && k + 1 < cap; ++k)
            out[k] = (p[k] >= 32 && p[k] < 127) ? (char)p[k] : '.';
        out[k] = '\0';
    }

    void DrawChangeGroups() {
        ImGui::Text("Byte changes:");
        const float footer = ImGui::GetStyle().ItemSpacing.y +
            ImGui::GetFrameHeightWithSpacing();
        ImGui::BeginChild("##difflist", ImVec2(0, -footer), true,
            ImGuiWindowFlags_HorizontalScrollbar);

        if (m_selectedCell < 0) {
            ImGui::TextDisabled("(click a heatmap cell to inspect that region)");
            ImGui::EndChild();
            return;
        }

        size_t begin = (size_t)m_selectedCell * m_cellSize;
        size_t end = std::min(begin + m_cellSize, m_size);

        const uint8_t* oldb = m_buf[1 - m_cur].data();
        const uint8_t* newb = m_buf[m_cur].data();

        ImGui::Text("Region [0x%zX .. 0x%zX)  %zu bytes",
            begin, end, end - begin);

        const int    kMaxGroups = 500;
        const size_t kPerLine = 16;
        int shownGroups = 0;

        size_t i = begin;
        while (i < end && shownGroups < kMaxGroups) {
            if (oldb[i] == newb[i]) { ++i; continue; }

            size_t j = i;
            while (j < end && oldb[j] != newb[j]) ++j;

            size_t n = std::min(j - i, kPerLine);

            char oldHex[3 * 16 + 1] = { 0 };
            char newHex[3 * 16 + 1] = { 0 };
            char oldAsc[17] = { 0 };
            char newAsc[17] = { 0 };

            size_t hx = 0;
            for (size_t k = 0; k < n; ++k) {
                hx += (size_t)std::snprintf(oldHex + hx, sizeof(oldHex) - hx,
                    "%02X ", oldb[i + k]);
                hx += (size_t)std::snprintf(newHex + hx, sizeof(newHex) - hx,
                    "%02X ", newb[i + k]);
            }
            buildAscii(oldb + i, n, oldAsc, sizeof(oldAsc));
            buildAscii(newb + i, n, newAsc, sizeof(newAsc));

            ImGui::Text("0x%08zX  %s-> %s | %s -> %s",
                i, oldHex, newHex, oldAsc, newAsc);
            ++shownGroups;
            i = j;
        }

        if (shownGroups == 0)
            ImGui::TextDisabled("(no changes in this region in last sample)");
        else if (shownGroups >= kMaxGroups)
            ImGui::TextDisabled("(truncated, too many changes)");

        ImGui::EndChild();
    }

    // ---- Hex 视图 ----
    void DrawHexView() {
        // 工具条：跳转到选中单元格
        if (m_selectedCell >= 0) {
            if (ImGui::Button("Jump to selected cell")) {
                size_t off = (size_t)m_selectedCell * m_cellSize;
                m_pendingScrollLine = (int)(off / 16);
            }
            ImGui::SameLine();
            size_t off = (size_t)m_selectedCell * m_cellSize;
            ImGui::TextDisabled("Selected: 0x%zX", off);
        }
        else {
            ImGui::TextDisabled("(no cell selected)");
        }

        const uint8_t* oldb = m_buf[1 - m_cur].data();
        const uint8_t* newb = m_buf[m_cur].data();

        const size_t kBytesPerLine = 16;
        size_t totalLines = (m_size + kBytesPerLine - 1) / kBytesPerLine;
        if (totalLines > (size_t)INT_MAX) totalLines = (size_t)INT_MAX;

        ImGui::BeginChild("##hexview", ImVec2(0, 0), true,
            ImGuiWindowFlags_HorizontalScrollbar);

        // 渲染前处理跳转
        if (m_pendingScrollLine >= 0) {
            float lineH = ImGui::GetTextLineHeightWithSpacing();
            ImGui::SetScrollY(m_pendingScrollLine * lineH);
            m_pendingScrollLine = -1;
        }

        ImGuiListClipper clipper;
        clipper.Begin((int)totalLines, ImGui::GetTextLineHeightWithSpacing());

        while (clipper.Step()) {
            for (int line = clipper.DisplayStart; line < clipper.DisplayEnd; ++line) {
                size_t lineStart = (size_t)line * kBytesPerLine;
                size_t lineEnd = std::min(lineStart + kBytesPerLine, m_size);

                // 地址列
                ImGui::Text("%08zX  ", lineStart);
                ImGui::SameLine(0, 0);

                // Hex 列
                for (size_t k = 0; k < kBytesPerLine; ++k) {
                    if (lineStart + k < lineEnd) {
                        size_t idx = lineStart + k;
                        bool changed = (oldb[idx] != newb[idx]);
                        ImVec4 c = changed ? ImVec4(1.0f, 0.9f, 0.2f, 1.0f)
                            : ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
                        ImGui::TextColored(c, "%02X", newb[idx]);
                    }
                    else {
                        ImGui::TextUnformatted("  ");
                    }
                    ImGui::SameLine(0, 4);
                }

                // 分隔符
                ImGui::SameLine(0, 8);
                ImGui::TextUnformatted("| ");
                ImGui::SameLine(0, 0);

                // ASCII 列
                for (size_t k = 0; k < kBytesPerLine; ++k) {
                    if (lineStart + k < lineEnd) {
                        size_t idx = lineStart + k;
                        bool changed = (oldb[idx] != newb[idx]);
                        uint8_t b = newb[idx];
                        char ch = (b >= 32 && b < 127) ? (char)b : '.';
                        ImVec4 c = changed ? ImVec4(1.0f, 0.9f, 0.2f, 1.0f)
                            : ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
                        ImGui::TextColored(c, "%c", ch);
                    }
                    else {
                        ImGui::TextUnformatted(" ");
                    }
                    ImGui::SameLine(0, 0);
                }
                ImGui::NewLine();
            }
        }

        ImGui::EndChild();
    }

    // ---- 块视图 ----
    void DrawBlockView() {
        if (!m_blockQuery) {
            ImGui::TextDisabled("(block query not registered)");
            return;
        }

        m_blocks.clear();
        m_blockQuery(m_blocks);

        size_t totalBytes = 0, freeBytes = 0, usedBytes = 0;
        size_t freeBlocks = 0, usedBlocks = 0;
        for (const auto& b : m_blocks) {
            totalBytes += b.total_size;
            if (b.is_free) { freeBytes += b.total_size; ++freeBlocks; }
            else { usedBytes += b.total_size; ++usedBlocks; }
        }

        float freePct = totalBytes ? 100.0f * (float)freeBytes / (float)totalBytes : 0.0f;

        ImGui::Text("Total blocks: %zu   Free: %zu (%.1f%%)   Used: %zu (%.1f%%)",
            m_blocks.size(), freeBlocks, freePct, usedBlocks, 100.0f - freePct);
        ImGui::Text("Bytes: total=%zu  free=%zu  used=%zu", totalBytes, freeBytes, usedBytes);

        // ---- 条形可视化 ----
        ImVec2 avail = ImGui::GetContentRegionAvail();
        float  barH = 44.0f;
        ImVec2 origin = ImGui::GetCursorScreenPos();

        ImGui::InvisibleButton("##blockbar", ImVec2(avail.x, barH));
        bool barHovered = ImGui::IsItemHovered();

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(origin,
            ImVec2(origin.x + avail.x, origin.y + barH),
            IM_COL32(30, 30, 30, 255));

        int hoveredBlockIdx = -1;
        if (totalBytes > 0 && !m_blocks.empty()) {
            float x = origin.x;
            const int kMaxDraw = 20000;
            int drawCount = (int)std::min<size_t>(m_blocks.size(), (size_t)kMaxDraw);
            for (int bi = 0; bi < drawCount; ++bi) {
                const auto& b = m_blocks[bi];
                float w = avail.x * (float)b.total_size / (float)totalBytes;
                if (w < 0.5f) w = 0.5f;
                ImU32 c = b.is_free ? IM_COL32(60, 190, 90, 255)
                    : IM_COL32(210, 80, 80, 255);
                dl->AddRectFilled(ImVec2(x, origin.y),
                    ImVec2(x + w, origin.y + barH), c);
                x += w;
                if (x > origin.x + avail.x) break;
            }
        }

        if (barHovered && totalBytes > 0) {
            ImVec2 mouse = ImGui::GetIO().MousePos;
            float rel = (mouse.x - origin.x) / avail.x;
            if (rel < 0.0f) rel = 0.0f;
            if (rel > 1.0f) rel = 1.0f;
            size_t target = (size_t)(rel * (float)totalBytes);
            size_t cum = 0;
            for (size_t bi = 0; bi < m_blocks.size(); ++bi) {
                const auto& b = m_blocks[bi];
                if (target < cum + b.total_size) {
                    hoveredBlockIdx = (int)bi;
                    break;
                }
                cum += b.total_size;
            }
        }

        if (hoveredBlockIdx >= 0) {
            const auto& b = m_blocks[hoveredBlockIdx];
            ImGui::BeginTooltip();
            ImGui::Text("Index: %d / %zu", hoveredBlockIdx, m_blocks.size());
            ImGui::Text("Addr:  0x%zX", b.addr);
            ImGui::Text("Total: %zu B", b.total_size);
            ImGui::Text("Payload: %zu B", b.payload_size);
            ImGui::TextColored(b.is_free ? ImVec4(0.3f, 0.9f, 0.4f, 1)
                : ImVec4(0.9f, 0.4f, 0.4f, 1),
                "State: %s", b.is_free ? "FREE" : "USED");
            ImGui::EndTooltip();
        }

        ImGui::Spacing();
        ImGui::Separator();

        // ---- 列表 ----
        ImGuiTableFlags tflags = ImGuiTableFlags_Borders
            | ImGuiTableFlags_RowBg
            | ImGuiTableFlags_ScrollY
            | ImGuiTableFlags_Resizable;
        if (ImGui::BeginTable("##blocks", 5, tflags, ImVec2(0, -1))) {
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 140);
            ImGui::TableSetupColumn("Total", ImGuiTableColumnFlags_WidthFixed, 100);
            ImGui::TableSetupColumn("Payload", ImGuiTableColumnFlags_WidthFixed, 100);
            ImGui::TableSetupColumn("State");
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();

            ImGuiListClipper clipper;
            clipper.Begin((int)m_blocks.size());
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    const auto& b = m_blocks[i];
                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%d", i);

                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("0x%zX", b.addr);

                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%zu", b.total_size);

                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%zu", b.payload_size);

                    ImGui::TableSetColumnIndex(4);
                    if (b.is_free)
                        ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1), "FREE");
                    else
                        ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1), "USED");
                }
            }
            ImGui::EndTable();
        }
    }
};

#endif // MEM_DEBUG_VIEW_ENABLED 

// 调试激活需要：

// class TLSFAllocator {...}中加入
#ifdef MEM_DEBUG_VIEW_ENABLED
struct DebugBlockInfo {
    uintptr_t addr;          // 块头地址
    size_t    total_size;    // 整块字节数(含 header)
    size_t    payload_size;  // 可用负载字节数
    bool      is_free;
};

// 遍历所有物理块（包含头尾哨兵），供调试可视化使用
template <typename Fn>
void debug_walk(Fn&& fn) const {
    if (!m_head || !m_tail) return;
    const Block* b = m_head;
    while (b != m_tail) {
        size_t sz = block_size(b);
        if (sz == 0) break;
        DebugBlockInfo info{
            (uintptr_t)b,
            sz,
            sz > sizeof(Block) ? sz - sizeof(Block) : 0,
            block_is_free(b)
        };
        fn(info);
        const Block* nb = (const Block*)((uintptr_t)b + sz);
        if (nb <= b) break;                 // 防御性
        b = nb;
    }
}
#endif // MEM_DEBUG_VIEW_ENABLED 

// IE_malloc_init 中加入：

#ifdef MEM_DEBUG_VIEW_ENABLED 
MemDebugView::Get().Detach();
#endif // MEM_DEBUG_VIEW_ENABLED 

#ifdef MEM_DEBUG_VIEW_ENABLED 
MemDebugView::Get().Attach(data, total);
MemDebugView::Get().SetBlockQuery(
    [this](std::vector<MemDebugView::BlockEntry>& out) {
        tlsf.debug_walk([&](const TLSFAllocator::DebugBlockInfo& info) {
            out.push_back({ info.addr, info.total_size,
                            info.payload_size, info.is_free });
            });
    });
#endif // MEM_DEBUG_VIEW_ENABLED 

// ~TextGameCore()中加入
#ifdef MEM_DEBUG_VIEW_ENABLED
MemDebugView::Get().Detach();   // 解除回调，防止悬空 this
#endif // MEM_DEBUG_VIEW_ENABLED

// imgui主循环中加入：
#ifdef MEM_DEBUG_VIEW_ENABLED
MemDebugView::Get().Draw();
#endif // MEM_DEBUG_VIEW_ENABLED 