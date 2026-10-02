#pragma once
#include "diagram.h"
#include "document.h"
#include "worker.h"
#include <chrono>
#include <d2d1.h>
#include <dwrite.h>
#include <filesystem>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <wincodec.h>
#include <windows.h>
#include <wrl/client.h>

namespace keepmd {
using Microsoft::WRL::ComPtr;
constexpr UINT WM_ASSET_READY = WM_APP + 11;
struct DiagramScene {
    mermaid::Diagram graph;
    mermaid::Layout layout;
    std::vector<mermaid::Rect> groups;
    float width = 0, height = 0;
};
struct Asset {
    enum Status { Pending, Ready, Failed } status = Pending;
    ComPtr<ID2D1Bitmap> bitmap;
    std::shared_ptr<DiagramScene> scene;
    std::wstring error;
    UINT width = 0, height = 0;
    uint64_t use = 0;
    size_t bytes = 0;
};
class View {
  public:
    explicit View(HWND hwnd);
    ~View();
    void set_document(std::shared_ptr<Document> doc, std::filesystem::path path);
    void resize();
    void set_render_dpi(unsigned value) {
        dpiOverride_ = value;
        resize();
    }
    float dpi_scale() const {
        return dpi_;
    }
    void paint();
    void scroll(float dy);
    void hscroll(float dx);
    void scroll_to(float y);
    void goto_block(size_t block);
    void set_zoom(float factor);
    void set_dark(bool value);
    void set_reading_width(float value);
    void collect_assets();
    void invalidate();
    void reset_device();
    void cancel_drag() {
        if (dragging_) {
            dragging_ = false;
            clickedLink_.clear();
        }
    }
    void mouse_down(float x, float y, bool extend);
    void mouse_move(float x, float y);
    void mouse_up(float x, float y);
    void select_all();
    std::wstring selection() const;
    std::wstring link_at(float x, float y);
    std::wstring source_at(float x, float y) const;
    void toggle_diagram(float x, float y);
    void set_matches(std::vector<Match> matches, int active);
    bool snapshot(const std::filesystem::path &path);
    float scroll_y() const {
        return scrollY_;
    }
    float scroll_x() const {
        return scrollX_;
    }
    float total_height() const {
        return heights_.total();
    }
    float page_height() const {
        return height_;
    }
    float zoom() const {
        return zoom_;
    }
    bool dark() const {
        return dark_;
    }
    float reading_width() const {
        return readingWidth_;
    }
    size_t layout_count() const {
        return cache_.size();
    }
    size_t asset_bytes() const {
        return assetBytes_;
    }
    size_t paint_count() const {
        return paintCount_;
    }
    size_t reused_layouts() const {
        return reusedLayouts_;
    }
    size_t reused_diagrams() const {
        return reusedDiagrams_;
    }
    double last_paint_ms() const {
        return lastPaintMs_;
    }
    struct FrameSample {
        double draw_ms = 0, dispatch_ms = 0;
    };
    const std::vector<FrameSample> &scroll_samples() const {
        return frameSamples_;
    }
    bool assets_pending() const;
    size_t anchor_block() const {
        return heights_.locate(scrollY_);
    }
    std::shared_ptr<Document> document() const {
        return doc_;
    }
    std::function<void(const std::wstring &)> on_link;

  private:
    struct Entry {
        std::vector<ComPtr<IDWriteTextLayout>> layouts;
        std::vector<float> offsets;
        std::vector<uint32_t> textOffsets;
        float height = 0, width = 0;
        uint64_t use = 0;
        size_t chars = 0;
    };
    struct Result {
        uint64_t generation = 0;
        size_t block = 0;
        UINT width = 0, height = 0;
        std::vector<uint8_t> pixels;
        std::shared_ptr<DiagramScene> scene;
        std::wstring error;
    };
    HWND hwnd_;
    std::shared_ptr<Document> doc_;
    std::filesystem::path path_;
    ComPtr<ID2D1Factory> d2d_;
    ComPtr<IDWriteFactory> dw_;
    ComPtr<ID2D1HwndRenderTarget> target_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<ID2D1StrokeStyle> dashed_;
    std::unordered_map<size_t, Entry> cache_;
    std::unordered_map<size_t, Asset> assets_;
    std::unordered_set<size_t> expandedDiagrams_;
    size_t cacheChars_ = 0, assetBytes_ = 0, paintCount_ = 0;
    size_t reusedLayouts_ = 0, reusedDiagrams_ = 0;
    uint64_t clock_ = 0, generation_ = 0;
    float width_ = 800, height_ = 600, dpi_ = 1, zoom_ = 1, readingWidth_ = 920;
    unsigned dpiOverride_ = 0;
    float scrollY_ = 0, scrollX_ = 0, widest_ = 0;
    bool dark_ = false, dragging_ = false, moved_ = false;
    size_t selectFrom_ = 0, selectTo_ = 0;
    D2D1_POINT_2F mouseStart_{};
    std::wstring clickedLink_;
    Heights heights_;
    std::vector<Match> matches_;
    int activeMatch_ = -1;
    double lastPaintMs_ = 0;
    std::chrono::steady_clock::time_point inputStarted_{};
    std::vector<FrameSample> frameSamples_;
    std::mutex resultsMutex_;
    std::vector<Result> results_;
    std::unique_ptr<Worker> worker_;
    void reset_layout(bool preserveAnchor);
    void ensure_target();
    void render(ID2D1RenderTarget *target);
    void update_scrollbars();
    float content_width() const;
    float left_margin() const;
    float font_size(const Block &block) const;
    Entry &layout(size_t index);
    void trim(size_t first, size_t last);
    void request_asset(size_t index);
    void draw_block(ID2D1RenderTarget *target, size_t index, Entry &entry, float y);
    void draw_diagram(ID2D1RenderTarget *target, const DiagramScene &scene, float x, float y, float fit);
    void draw_text(ID2D1RenderTarget *target, const std::wstring &text, D2D1_RECT_F rect, float size,
                   D2D1_COLOR_F color, bool bold = false);
    void fill(ID2D1RenderTarget *target, D2D1_RECT_F rect, D2D1_COLOR_F color);
    void range_rects(ID2D1RenderTarget *target, IDWriteTextLayout *layout, uint32_t start, uint32_t length,
                     float x, float y, D2D1_COLOR_F color);
    size_t hit(float x, float y, bool strict = false);
    D2D1_COLOR_F foreground() const;
    D2D1_COLOR_F background() const;
};
} // namespace keepmd
