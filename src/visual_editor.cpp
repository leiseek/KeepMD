#include "visual_editor.h"
#include "prompt_model.h"
#include "ui.h"
#include "visual_diagram.h"
#include "visual_markdown.h"
#include <algorithm>
#include <commctrl.h>
#include <imm.h>
#include <richedit.h>
#include <richole.h>
#include <unordered_map>

namespace keepmd {
namespace {
struct Buffer {
    std::wstring value;
    std::vector<size_t> objects;
    size_t size() const {
        return value.size();
    }
    wchar_t operator[](size_t i) const {
        return value[i];
    }
    bool object(size_t i) const {
        return std::binary_search(objects.begin(), objects.end(), i);
    }
};
Buffer buffer_text(HWND h) {
    GETTEXTLENGTHEX len{GTL_PRECISE | GTL_NUMCHARS, 1200};
    auto count = SendMessageW(h, EM_GETTEXTLENGTHEX, (WPARAM)&len, 0);
    if (count < 0 || count > 2 * PromptLimit)
        return {};
    std::wstring text((size_t)count + 2, 0);
    GETTEXTEX get{(DWORD)(text.size() * 2), GT_RAWTEXT, 1200, nullptr, nullptr};
    auto n = SendMessageW(h, EM_GETTEXTEX, (WPARAM)&get, (LPARAM)text.data());
    text.resize(std::max<LRESULT>(0, n));
    for (auto &c : text)
        if (c == L'\r')
            c = L'\n';
    Buffer result{std::move(text)};
    IRichEditOle *ole = nullptr;
    SendMessageW(h, EM_GETOLEINTERFACE, 0, (LPARAM)&ole);
    if (ole) {
        for (LONG i = 0; i < ole->GetObjectCount(); ++i) {
            REOBJECT object{};
            object.cbStruct = sizeof(object);
            if (SUCCEEDED(ole->GetObject(i, &object, REO_GETOBJ_NO_INTERFACES)) && object.cp >= 0)
                result.objects.push_back((size_t)object.cp);
        }
        ole->Release();
        std::sort(result.objects.begin(), result.objects.end());
    }
    return result;
}
bool clipboard(HWND owner, std::wstring_view text) {
    auto memory = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * 2);
    if (!memory)
        return false;
    auto p = (wchar_t *)GlobalLock(memory);
    if (!p) {
        GlobalFree(memory);
        return false;
    }
    memcpy(p, text.data(), text.size() * 2);
    p[text.size()] = 0;
    GlobalUnlock(memory);
    if (!OpenClipboard(owner)) {
        GlobalFree(memory);
        return false;
    }
    bool ok = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory);
    CloseClipboard();
    if (!ok)
        GlobalFree(memory);
    return ok;
}
std::wstring clipboard_text() {
    if (!OpenClipboard(nullptr))
        return {};
    auto h = GetClipboardData(CF_UNICODETEXT);
    std::wstring text;
    if (h) {
        auto p = (wchar_t *)GlobalLock(h);
        if (p) {
            size_t count = std::min<size_t>(GlobalSize(h) / 2, PromptLimit + 1);
            text.assign(p, wcsnlen(p, count));
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return text;
}
size_t logical(const Buffer &raw, size_t position) {
    size_t result = 0;
    for (size_t i = 0; i < std::min(position, raw.size()); ++i)
        if (!raw.object(i))
            ++result;
    return result;
}
size_t physical(const Buffer &raw, size_t position) {
    size_t n = 0, i = 0;
    while (i < raw.size() && n < position) {
        if (!raw.object(i))
            ++n;
        ++i;
    }
    return i;
}
std::wstring markdown(const Buffer &raw) {
    auto out = raw.value;
    for (auto i = raw.objects.rbegin(); i != raw.objects.rend(); ++i)
        if (*i < out.size())
            out.erase(*i, 1);
    return out;
}
struct InputStream {
    const std::string &data;
    size_t pos = 0;
};
class OleCallback final : public IRichEditOleCallback {
    LONG refs_ = 1;

  public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_IRichEditOleCallback) {
            *out = this;
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&refs_);
    }
    ULONG STDMETHODCALLTYPE Release() override {
        auto n = InterlockedDecrement(&refs_);
        if (!n)
            delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE GetNewStorage(LPSTORAGE *storage) override {
        ILockBytes *bytes = nullptr;
        auto hr = CreateILockBytesOnHGlobal(nullptr, TRUE, &bytes);
        if (SUCCEEDED(hr)) {
            hr = StgCreateDocfileOnILockBytes(bytes, STGM_SHARE_EXCLUSIVE | STGM_CREATE | STGM_READWRITE, 0,
                                              storage);
            bytes->Release();
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetInPlaceContext(LPOLEINPLACEFRAME *, LPOLEINPLACEUIWINDOW *,
                                                LPOLEINPLACEFRAMEINFO) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE ShowContainerUI(BOOL) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryInsertObject(LPCLSID, LPSTORAGE, LONG) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DeleteObject(LPOLEOBJECT) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryAcceptData(LPDATAOBJECT, CLIPFORMAT *format, DWORD, BOOL,
                                              HGLOBAL) override {
        if (format)
            *format = CF_UNICODETEXT;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ContextSensitiveHelp(BOOL) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetClipboardData(CHARRANGE *, DWORD, LPDATAOBJECT *) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetDragDropEffect(BOOL, DWORD, LPDWORD effect) override {
        if (effect)
            *effect = DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetContextMenu(WORD, LPOLEOBJECT, CHARRANGE *, HMENU *menu) override {
        if (menu)
            *menu = nullptr;
        return E_NOTIMPL;
    }
};
DWORD CALLBACK stream_in(DWORD_PTR cookie, LPBYTE buffer, LONG count, LONG *copied) {
    auto &in = *(InputStream *)cookie;
    *copied = (LONG)std::min<size_t>(count, in.data.size() - in.pos);
    memcpy(buffer, in.data.data() + in.pos, *copied);
    in.pos += *copied;
    return 0;
}
} // namespace
struct VisualEditor::Impl {
    HWND h = nullptr, parent = nullptr;
    int id = 0;
    HMODULE library = nullptr;
    bool loading = false, dark = false, formatDirty = true, ime = false, dialogOpen = false;
    std::wstring lastSource;
    struct State {
        std::wstring source;
        size_t a = 0, z = 0;
    };
    std::vector<State> undo, redo;
    State current;
    std::unordered_map<std::wstring, std::string> diagrams;
    size_t diagramBytes = 0;
    int width = 0;
    Impl(HWND parent, int id) : parent(parent), id(id) {
        library = LoadLibraryExW(L"Msftedit.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library)
            return;
        h = CreateWindowExW(0, MSFTEDIT_CLASS, L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_WANTRETURN |
                                ES_AUTOVSCROLL | ES_NOHIDESEL,
                            0, 0, 100, 100, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
        if (!h)
            return;
        SendMessageW(h, EM_SETTEXTMODE, TM_RICHTEXT | TM_MULTILEVELUNDO, 0);
        auto callback = new OleCallback;
        SendMessageW(h, EM_SETOLECALLBACK, 0, (LPARAM)callback);
        callback->Release();
        SendMessageW(h, EM_SETUNDOLIMIT, 0,
                     0); // Markdown snapshots also include formatting and diagram edits.
        SendMessageW(h, EM_EXLIMITTEXT, 0, PromptLimit);
        SendMessageW(h, EM_SETEVENTMASK, 0, ENM_CHANGE | ENM_SELCHANGE);
        SendMessageW(h, EM_SETTARGETDEVICE, 0, 0);
        ui::attach_scrollbars(h, ui::ScrollKind::Editor);
        SetWindowSubclass(h, proc, 8, (DWORD_PTR)this);
    }
    ~Impl() {
        if (h)
            DestroyWindow(h);
        if (library)
            FreeLibrary(library);
    }
    State state() const {
        auto raw = buffer_text(h);
        CHARRANGE range{};
        SendMessageW(h, EM_EXGETSEL, 0, (LPARAM)&range);
        return {markdown(raw), logical(raw, std::max(0L, range.cpMin)),
                logical(raw, std::max(0L, range.cpMax))};
    }
    void trim() {
        size_t bytes = 0;
        for (const auto &s : undo)
            bytes += s.source.size() * 2;
        while (undo.size() > 64 || (bytes > 8 * 1024 * 1024 && undo.size() > 1)) {
            bytes -= undo.front().source.size() * 2;
            undo.erase(undo.begin());
        }
    }
    void changed() {
        if (loading)
            return;
        if (composing()) {
            formatDirty = true;
            return;
        }
        auto next = state();
        if (next.source != current.source) {
            undo.push_back(current);
            trim();
            redo.clear();
            current = std::move(next);
        }
        formatDirty = true;
        if (!ime)
            SetTimer(h, 11, 180, nullptr);
    }
    bool composing() const {
        auto context = ImmGetContext(h);
        bool active =
            ime || dialogOpen || (context && ImmGetCompositionStringW(context, GCS_COMPSTR, nullptr, 0) > 0);
        if (context)
            ImmReleaseContext(h, context);
        return active;
    }
    void notify() {
        SendMessageW(parent, WM_COMMAND, MAKEWPARAM(id, EN_CHANGE), (LPARAM)h);
    }
    void install(State next, bool history) {
        if (next.source.size() > PromptLimit)
            return;
        if (history && next.source != current.source) {
            undo.push_back(state());
            trim();
            redo.clear();
        }
        loading = true;
        auto raw = next.source;
        for (auto &c : raw)
            if (c == L'\n')
                c = L'\r';
        SETTEXTEX set{ST_DEFAULT, 1200};
        SendMessageW(h, EM_SETTEXTEX, (WPARAM)&set, (LPARAM)raw.c_str());
        CHARRANGE range{(LONG)next.a, (LONG)next.z};
        SendMessageW(h, EM_EXSETSEL, 0, (LPARAM)&range);
        loading = false;
        current = std::move(next);
        formatDirty = true;
        refresh();
        if (history)
            notify();
    }
    void replace(size_t a, size_t z, std::wstring_view value) {
        auto next = state();
        a = std::min(a, next.source.size());
        z = std::clamp(z, a, next.source.size());
        next.source.replace(a, z - a, value);
        next.a = next.z = a + value.size();
        install(std::move(next), true);
    }
    void restore(bool forward) {
        auto &from = forward ? redo : undo;
        auto &to = forward ? undo : redo;
        auto pending = state();
        if (pending.source != current.source)
            changed();
        if (from.empty())
            return;
        to.push_back(state());
        auto next = std::move(from.back());
        from.pop_back();
        install(std::move(next), false);
        notify();
    }
    std::string build(const VisualPlan &plan) {
        auto colors = ui::palette(dark);
        auto color = [](COLORREF c) {
            return "\\red" + std::to_string(GetRValue(c)) + "\\green" + std::to_string(GetGValue(c)) +
                   "\\blue" + std::to_string(GetBValue(c)) + ";";
        };
        std::string out =
            "{\\rtf1\\ansi\\ansicpg1252\\deff0\\uc1{\\fonttbl{\\f0 Segoe UI;}{\\f1 Consolas;}}{\\colortbl;" +
            color(colors.text) + color(colors.accent) + color(colors.selected) +
            "}\\viewkind4\\viewscale100 ";
        RECT client{};
        GetClientRect(h, &client);
        int maxWidth = std::max(200, (int)(client.right / (GetDpiForWindow(h) / 96.f)) - 60);
        size_t diagram = 0;
        for (const auto &para : plan.paragraphs) {
            out += "\\pard\\plain\\f0\\fs24\\cf1\\sa100\\sl330\\slmult1 ";
            bool hasVisible = false;
            for (size_t i = para.start; i < para.end; ++i)
                if (!(plan.styles[i] & VHidden)) {
                    hasVisible = true;
                    break;
                }
            bool imageHere = diagram < plan.diagrams.size() && para.start == plan.diagrams[diagram].start;
            if (!hasVisible && !imageHere)
                out += "\\fs2\\sa0\\sb0\\sl45\\slmult0 ";
            if (para.heading)
                out += "\\b\\fs" +
                       std::to_string(para.heading == 1   ? 44
                                      : para.heading == 2 ? 34
                                                          : 28) +
                       "\\sb120\\sa130 ";
            if (para.quote)
                out += "\\li360\\cf2 ";
            if (para.code && (hasVisible || imageHere))
                out += "\\f1\\fs22\\sa0 ";
            if (para.list == 1)
                out += "\\li300\\fi-240{\\pntext\\f0\\u8226?\\tab}{\\*\\pn\\pnlvlblt\\pnf0\\pnindent240{"
                       "\\pntxtb\\u8226?}}";
            if (para.list == 2)
                out += "\\li300\\fi-240{\\pntext\\f0 "
                       "1.\\tab}{\\*\\pn\\pnlvlbody\\pndec\\pnstart1\\pnindent240{\\pntxta.}}";
            if (diagram < plan.diagrams.size() && para.start == plan.diagrams[diagram].start) {
                auto &d = plan.diagrams[diagram++];
                auto key = std::to_wstring(maxWidth) + (dark ? L"D" : L"L") + d.source;
                auto found = diagrams.find(key);
                if (found == diagrams.end()) {
                    auto value = visual_diagram_rtf(d.source, maxWidth, dark);
                    if (diagramBytes + value.size() > 12 * 1024 * 1024) {
                        diagrams.clear();
                        diagramBytes = 0;
                    }
                    diagramBytes += value.size();
                    found = diagrams.emplace(std::move(key), std::move(value)).first;
                }
                out += "\\v0 " + found->second;
            }
            auto end = std::min(para.end + 1, plan.source.size());
            for (size_t i = para.start; i < end;) {
                auto style = plan.styles[i];
                size_t z = i + 1;
                while (z < end && plan.styles[z] == style)
                    ++z;
                out += "{";
                if (style & VHidden)
                    out += "\\v ";
                else
                    out += "\\v0 ";
                if (style & VBold)
                    out += "\\b ";
                if (style & VItalic)
                    out += "\\i ";
                if (style & VStrike)
                    out += "\\strike ";
                if (style & VCode)
                    out += "\\f1\\fs22\\highlight3 ";
                if (style & VLink)
                    out += "\\ul\\cf2 ";
                out += rtf_escape(std::wstring_view(plan.source).substr(i, z - i));
                out += '}';
                i = z;
            }
        }
        out += "\\par}";
        return out;
    }
    void refresh() {
        KillTimer(h, 11);
        if (loading || composing() || !formatDirty)
            return;
        auto before = state();
        auto plan = visual_plan(before.source);
        auto rtf = build(plan);
        POINT scroll{};
        SendMessageW(h, EM_GETSCROLLPOS, 0, (LPARAM)&scroll);
        loading = true;
        SendMessageW(h, WM_SETREDRAW, FALSE, 0);
        InputStream in{rtf};
        EDITSTREAM stream{(DWORD_PTR)&in, 0, stream_in};
        SendMessageW(h, EM_STREAMIN, SF_RTF, (LPARAM)&stream);
        auto raw = buffer_text(h);
        auto actual = markdown(raw);
        // Never allow a RichEdit RTF conversion to silently alter a stored prompt.
        if (stream.dwError || actual != plan.source) {
            auto fallback = plan.source;
            for (auto &c : fallback)
                if (c == L'\n')
                    c = L'\r';
            SETTEXTEX set{ST_DEFAULT, 1200};
            SendMessageW(h, EM_SETTEXTEX, (WPARAM)&set, (LPARAM)fallback.c_str());
            raw = buffer_text(h);
        }
        CHARRANGE range{(LONG)physical(raw, before.a), (LONG)physical(raw, before.z)};
        SendMessageW(h, EM_EXSETSEL, 0, (LPARAM)&range);
        if (range.cpMin == range.cpMax) {
            CHARFORMAT2W visible{};
            visible.cbSize = sizeof(visible);
            visible.dwMask = CFM_HIDDEN;
            visible.dwEffects = 0;
            SendMessageW(h, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&visible);
        }
        SendMessageW(h, EM_SETSCROLLPOS, 0, (LPARAM)&scroll);
        SendMessageW(h, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(h, nullptr, TRUE);
        SendMessageW(h, EM_EMPTYUNDOBUFFER, 0, 0);
        loading = false;
        formatDirty = false;
        lastSource = plan.source;
        ui::sync_scrollbars(h);
    }
    void toggle_inline(std::wstring mark) {
        auto s = state();
        size_t a = s.a, z = s.z;
        if (a == z) {
            auto placeholder = mark == L"`" ? L"代码" : L"文字";
            s.source.insert(a, placeholder);
            z = a + wcslen(placeholder);
        }
        if (a >= mark.size() && s.source.substr(a - mark.size(), mark.size()) == mark &&
            s.source.substr(z, mark.size()) == mark) {
            s.source.erase(z, mark.size());
            s.source.erase(a - mark.size(), mark.size());
            s.a = a - mark.size();
            s.z = z - mark.size();
        } else {
            s.source.insert(z, mark);
            s.source.insert(a, mark);
            s.a = a + mark.size();
            s.z = z + mark.size();
        }
        install(std::move(s), true);
        SetFocus(h);
    }
    void paragraph(std::wstring prefix) {
        auto s = state();
        auto a = s.source.rfind(L'\n', s.a ? s.a - 1 : 0);
        a = a == std::wstring::npos ? 0 : a + 1;
        if (!s.a)
            a = 0;
        auto z = s.source.find(L'\n', a);
        if (z == std::wstring::npos)
            z = s.source.size();
        size_t remove = 0;
        while (a + remove < z && s.source[a + remove] == L'#' && remove < 6)
            ++remove;
        if (remove && a + remove < z && s.source[a + remove] == L' ')
            ++remove;
        else if (remove)
            remove = 0;
        if (!remove && z - a >= 2 && (s.source[a] == L'-' || s.source[a] == L'>' || s.source[a] == L'*') &&
            s.source[a + 1] == L' ')
            remove = 2;
        if (!remove) {
            size_t n = a;
            while (n < z && iswdigit(s.source[n]))
                ++n;
            if (n > a && n + 1 < z && s.source[n] == L'.' && s.source[n + 1] == L' ')
                remove = n - a + 2;
        }
        s.source.replace(a, remove, prefix);
        s.a = s.z = a + prefix.size() + std::min(s.a >= a + remove ? s.a - a - remove : 0, z - a - remove);
        install(std::move(s), true);
        SetFocus(h);
    }
    void diagram_dialog() {
        if (composing())
            return;
        auto s = state();
        auto plan = visual_plan(s.source);
        size_t a = s.a, z = s.z;
        std::wstring body = L"flowchart LR\nA[输入] --> B[处理] --> C[结果]\n";
        for (auto &d : plan.diagrams)
            if (s.a >= d.start && (s.a < d.end || (s.a == d.end && s.a == s.source.size()))) {
                a = d.start;
                z = d.end;
                body = d.source;
                break;
            }
        struct Dialog {
            HWND edit = nullptr, window = nullptr;
            HFONT font = nullptr;
            HBRUSH background = nullptr;
            std::wstring text;
            bool accepted = false, done = false, dark = false;
            ~Dialog() {
                if (font)
                    DeleteObject(font);
                if (background)
                    DeleteObject(background);
            }
        } dialog;
        dialog.text = body;
        dialog.dark = dark;
        dialog.font = ui::font(GetDpiForWindow(parent));
        dialog.background = CreateSolidBrush(ui::palette(dark).background);
        WNDCLASSW cls{};
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"KeepMD.DiagramInput";
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
            auto p = (Dialog *)GetWindowLongPtrW(h, GWLP_USERDATA);
            if (m == WM_NCCREATE) {
                p = (Dialog *)((CREATESTRUCTW *)l)->lpCreateParams;
                p->window = h;
                SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)p);
            }
            if (!p)
                return DefWindowProcW(h, m, w, l);
            if (m == WM_ERASEBKGND)
                return 1;
            if (m == WM_PAINT) {
                PAINTSTRUCT ps{};
                auto dc = BeginPaint(h, &ps);
                RECT rect{};
                GetClientRect(h, &rect);
                ui::fill(dc, rect, ui::palette(p->dark).background);
                EndPaint(h, &ps);
                return 0;
            }
            if (m == WM_DRAWITEM) {
                ui::draw_button(*(DRAWITEMSTRUCT *)l, p->dark);
                return TRUE;
            }
            if (m == WM_CTLCOLORSTATIC) {
                auto c = ui::palette(p->dark);
                SetTextColor((HDC)w, c.muted);
                SetBkColor((HDC)w, c.background);
                return (LRESULT)p->background;
            }
            if (m == WM_CREATE) {
                auto px = [&](int n) { return MulDiv(n, GetDpiForWindow(h), 96); };
                CreateWindowExW(0, L"STATIC", L"Mermaid 流程图 · 修改语法后点“应用”", WS_CHILD | WS_VISIBLE,
                                px(18), px(16), px(590), px(24), h, nullptr, nullptr, nullptr);
                p->edit = CreateWindowExW(0, MSFTEDIT_CLASS, L"",
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE |
                                              ES_WANTRETURN | ES_AUTOVSCROLL,
                                          px(18), px(48), px(590), px(300), h, (HMENU)1001, nullptr, nullptr);
                SendMessageW(p->edit, EM_SETTEXTMODE, TM_PLAINTEXT, 0);
                SendMessageW(p->edit, EM_EXLIMITTEXT, 0, 65536);
                SetWindowTextW(p->edit, p->text.c_str());
                auto colors = ui::palette(p->dark);
                SendMessageW(p->edit, EM_SETBKGNDCOLOR, 0, colors.surface);
                CHARFORMAT2W style{};
                style.cbSize = sizeof(style);
                style.dwMask = CFM_FACE | CFM_SIZE | CFM_COLOR;
                style.yHeight = 220;
                style.crTextColor = colors.text;
                wcscpy_s(style.szFaceName, L"Consolas");
                SendMessageW(p->edit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&style);
                ui::attach_scrollbars(p->edit, ui::ScrollKind::Editor);
                ui::scroll_theme(p->edit, p->dark);
                CreateWindowExW(0, L"BUTTON", L"应用", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                px(420), px(364), px(88), px(32), h, (HMENU)IDOK, nullptr, nullptr);
                CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP, px(520), px(364),
                                px(88), px(32), h, (HMENU)IDCANCEL, nullptr, nullptr);
                for (auto child = GetWindow(h, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
                    if (child != p->edit)
                        SendMessageW(child, WM_SETFONT, (WPARAM)p->font, TRUE);
                ui::style_button(GetDlgItem(h, IDOK), true);
                ui::style_button(GetDlgItem(h, IDCANCEL));
                ui::titlebar(h, p->dark);
                SetFocus(p->edit);
                return 0;
            }
            if (m == WM_COMMAND && HIWORD(w) == 0 && (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL)) {
                if (LOWORD(w) == IDOK) {
                    int n = GetWindowTextLengthW(p->edit);
                    p->text.resize(n + 1);
                    GetWindowTextW(p->edit, p->text.data(), n + 1);
                    p->text.resize(n);
                    p->accepted = true;
                }
                p->done = true;
                return 0;
            }
            if (m == WM_CLOSE) {
                p->done = true;
                return 0;
            }
            return DefWindowProcW(h, m, w, l);
        };
        RegisterClassW(&cls);
        RECT rect{};
        GetWindowRect(parent, &rect);
        auto window = CreateWindowExW(
            WS_EX_DLGMODALFRAME, cls.lpszClassName, L"编辑流程图", WS_CAPTION | WS_SYSMENU, rect.left + 50,
            rect.top + 50, MulDiv(640, GetDpiForWindow(parent), 96), MulDiv(448, GetDpiForWindow(parent), 96),
            parent, nullptr, cls.hInstance, &dialog);
        if (!window)
            return;
        dialogOpen = true;
        EnableWindow(parent, FALSE);
        ShowWindow(window, SW_SHOW);
        SetForegroundWindow(window);
        SetFocus(dialog.edit);
        MSG msg{};
        while (!dialog.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
                dialog.done = true;
                break;
            }
            if (!IsDialogMessageW(window, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        dialogOpen = false;
        EnableWindow(parent, TRUE);
        DestroyWindow(window);
        SetForegroundWindow(parent);
        SetFocus(h);
        if (dialog.accepted) {
            auto editedBody = visual_plan(dialog.text).source;
            if (!editedBody.empty() && editedBody.back() != L'\n')
                editedBody += L'\n';
            auto prefix = a && s.source[a - 1] != L'\n' ? L"\n\n" : L"";
            replace(a, z, std::wstring(prefix) + L"```mermaid\n" + editedBody + L"```\n");
        }
    }
    void command(VisualCommand cmd) {
        if (composing())
            return;
        if (cmd == VisualCommand::Bold)
            toggle_inline(L"**");
        else if (cmd == VisualCommand::Italic)
            toggle_inline(L"*");
        else if (cmd == VisualCommand::Code)
            toggle_inline(L"`");
        else if (cmd == VisualCommand::H1)
            paragraph(L"# ");
        else if (cmd == VisualCommand::H2)
            paragraph(L"## ");
        else if (cmd == VisualCommand::H3)
            paragraph(L"### ");
        else if (cmd == VisualCommand::Bullet)
            paragraph(L"- ");
        else if (cmd == VisualCommand::Numbered)
            paragraph(L"1. ");
        else if (cmd == VisualCommand::Quote)
            paragraph(L"> ");
        else if (cmd == VisualCommand::Paragraph)
            paragraph(L"");
        else if (cmd == VisualCommand::Diagram)
            diagram_dialog();
    }
    static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
        auto p = (Impl *)data;
        if (m == WM_NCDESTROY) {
            RemoveWindowSubclass(h, proc, id);
            return DefSubclassProc(h, m, w, l);
        }
        if (p->loading)
            return DefSubclassProc(h, m, w, l);
        if (m == WM_IME_STARTCOMPOSITION)
            p->ime = true;
        if (m == WM_IME_ENDCOMPOSITION) {
            auto r = DefSubclassProc(h, m, w, l);
            p->ime = false;
            p->changed();
            p->formatDirty = true;
            SetTimer(h, 11, 180, nullptr);
            return r;
        }
        if (m == WM_TIMER && w == 11) {
            p->refresh();
            return 0;
        }
        if (m == WM_PASTE) {
            auto text = clipboard_text();
            if (text.find(L'\ufffc') != std::wstring::npos) {
                MessageBoxW(p->parent, L"粘贴内容包含保留的对象占位符，未插入。请先移除该字符。", L"KeepMD",
                            MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            if (text.size() <= PromptLimit) {
                auto s = p->state();
                p->replace(s.a, s.z, visual_plan(text).source);
            }
            return 0;
        }
        if (m == WM_COPY || m == WM_CUT) {
            auto s = p->state();
            if (s.a < s.z && clipboard(h, std::wstring_view(s.source).substr(s.a, s.z - s.a)) && m == WM_CUT)
                p->replace(s.a, s.z, L"");
            return 0;
        }
        if (m == WM_UNDO || m == EM_UNDO) {
            p->restore(false);
            return 1;
        }
        if (m == EM_REDO) {
            p->restore(true);
            return 1;
        }
        if (m == WM_LBUTTONDBLCLK) {
            auto raw = buffer_text(h);
            POINTL point{(LONG)(short)LOWORD(l), (LONG)(short)HIWORD(l)};
            auto hit = SendMessageW(h, EM_CHARFROMPOS, 0, (LPARAM)&point);
            if (hit >= 0) {
                auto at = logical(raw, (size_t)hit);
                auto plan = visual_plan(markdown(raw));
                for (const auto &diagram : plan.diagrams)
                    if (at >= diagram.start &&
                        (at < diagram.end || (at == diagram.end && at == plan.source.size()))) {
                        CHARRANGE range{(LONG)physical(raw, diagram.start),
                                        (LONG)physical(raw, diagram.start)};
                        SendMessageW(h, EM_EXSETSEL, 0, (LPARAM)&range);
                        p->diagram_dialog();
                        return 0;
                    }
            }
        }
        if (m == WM_KEYDOWN && !p->composing()) {
            bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
            if (ctrl && w == 'Z') {
                p->restore(shift);
                return 0;
            }
            if (ctrl && w == 'Y') {
                p->restore(true);
                return 0;
            }
            if (ctrl && w == 'B') {
                p->command(VisualCommand::Bold);
                return 0;
            }
            if (ctrl && w == 'I') {
                p->command(VisualCommand::Italic);
                return 0;
            }
            if (ctrl && w == 'V') {
                SendMessageW(h, WM_PASTE, 0, 0);
                return 0;
            }
            if (ctrl && (w == 'C' || w == 'X')) {
                SendMessageW(h, w == 'C' ? WM_COPY : WM_CUT, 0, 0);
                return 0;
            }
            if (w == VK_RETURN && !ctrl) {
                auto s = p->state();
                size_t a = s.source.rfind(L'\n', s.a ? s.a - 1 : 0);
                a = a == std::wstring::npos ? 0 : a + 1;
                if (!s.a)
                    a = 0;
                auto line = s.source.substr(a, s.a - a);
                std::wstring prefix;
                if (line.starts_with(L"- "))
                    prefix = L"- ";
                else if (line.starts_with(L"> "))
                    prefix = L"> ";
                else {
                    size_t n = 0;
                    while (n < line.size() && iswdigit(line[n]))
                        ++n;
                    if (n && line.substr(n, 2) == L". ")
                        prefix = L"1. ";
                }
                if (line == prefix && !prefix.empty())
                    p->replace(a, s.z, L"");
                else
                    p->replace(s.a, s.z, L"\n" + (shift ? L"" : prefix));
                return 0;
            }
            if (w == VK_DELETE || w == VK_BACK) {
                auto raw = buffer_text(h);
                CHARRANGE r{};
                SendMessageW(h, EM_EXGETSEL, 0, (LPARAM)&r);
                size_t pos = w == VK_BACK && r.cpMin == r.cpMax && r.cpMin > 0 ? r.cpMin - 1 : r.cpMin;
                if (pos < raw.size() && raw.object(pos)) {
                    auto s = p->state();
                    auto plan = visual_plan(s.source);
                    auto at = logical(raw, pos);
                    for (auto &d : plan.diagrams)
                        if (d.start == at) {
                            p->replace(d.start, d.end, L"");
                            return 0;
                        }
                }
            }
        }
        if (m == WM_CHAR && w == VK_RETURN)
            return 0;
        return DefSubclassProc(h, m, w, l);
    }
};
VisualEditor::VisualEditor(HWND parent, int id) : p_(std::make_unique<Impl>(parent, id)) {}
VisualEditor::~VisualEditor() = default;
HWND VisualEditor::hwnd() const {
    return p_->h;
}
bool VisualEditor::valid() const {
    return p_->h != nullptr;
}
void VisualEditor::load(std::wstring_view source) {
    p_->undo.clear();
    p_->redo.clear();
    p_->install({visual_plan(std::wstring(source)).source, 0, 0}, false);
}
void VisualEditor::replace_document(std::wstring_view source) {
    auto s = visual_plan(std::wstring(source)).source;
    p_->install({s, s.size(), s.size()}, true);
}
std::wstring VisualEditor::text(bool crlf) const {
    auto s = markdown(buffer_text(p_->h));
    if (!crlf)
        return s;
    std::wstring out;
    for (auto c : s) {
        if (c == L'\n')
            out += L'\r';
        out += c;
    }
    return out;
}
void VisualEditor::saved() {
    SendMessageW(p_->h, EM_SETMODIFY, FALSE, 0);
}
void VisualEditor::theme(bool dark) {
    p_->dark = dark;
    p_->loading = true;
    SendMessageW(p_->h, EM_SETBKGNDCOLOR, 0, dark ? RGB(24, 30, 40) : RGB(252, 252, 251));
    p_->loading = false;
    p_->formatDirty = true;
    p_->refresh();
    ui::scroll_theme(p_->h, dark);
}
void VisualEditor::inset(unsigned dpi) {
    RECT r{};
    GetClientRect(p_->h, &r);
    int pad = MulDiv(26, dpi, 96);
    int width = r.right;
    r.left += pad;
    r.right = std::max(r.left + 1, r.right - pad);
    r.top += pad;
    r.bottom = std::max(r.top + 1, r.bottom - pad);
    SendMessageW(p_->h, EM_SETRECT, 0, (LPARAM)&r);
    if (width != p_->width) {
        p_->width = width;
        p_->formatDirty = true;
        SetTimer(p_->h, 11, 180, nullptr);
    }
}
void VisualEditor::changed() {
    p_->changed();
}
void VisualEditor::refresh() {
    p_->refresh();
}
void VisualEditor::command(VisualCommand command) {
    p_->command(command);
}
bool VisualEditor::composing() const {
    return p_->composing();
}
bool VisualEditor::internal() const {
    return p_->loading;
}
void VisualEditor::clear() {
    p_->install({L"", 0, 0}, true);
    SetFocus(p_->h);
}
} // namespace keepmd
