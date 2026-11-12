#include "ui/UiLayout.hpp"
#include "core/AssetStore.hpp"
#include "core/LoaderResources.hpp"
#include "core/Localization.hpp"
#include "graphics/BitmapFontRenderer.hpp"
#include "graphics/Renderer.hpp"

#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace OpenArtOfWar::uilayout
{

using Vars = std::map<std::string, float>;

namespace
{

// ---- expressions: + - * / ( ), numbers with an optional `u`, names (W, H, u, <id>.x ...), min / max / clamp ----
struct ExprParser
{
    const std::string &s;
    const Vars &vars;
    float u;
    std::size_t i {0};
    std::string error {};

    void Ws()
    {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
            ++i;
    }
    bool Eat(char c)
    {
        Ws();
        if (i < s.size() && s[i] == c)
        {
            ++i;
            return true;
        }
        return false;
    }
    float Expr()
    {
        float v = Term();
        for (;;)
        {
            if (Eat('+'))
                v += Term();
            else if (Eat('-'))
                v -= Term();
            else
                return v;
        }
    }
    float Term()
    {
        float v = Unary();
        for (;;)
        {
            if (Eat('*'))
                v *= Unary();
            else if (Eat('/'))
            {
                const float d = Unary();
                v = d != 0.0f ? v / d : 0.0f;
            }
            else
                return v;
        }
    }
    float Unary()
    {
        if (Eat('-'))
            return -Unary();
        return Primary();
    }
    float Primary()
    {
        Ws();
        if (Eat('('))
        {
            const float v = Expr();
            if (!Eat(')'))
                error = "missing )";
            return v;
        }
        if (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.'))
        {
            char *end = nullptr;
            const float v = std::strtof(s.c_str() + i, &end);
            i = static_cast<std::size_t>(end - s.c_str());
            if (i < s.size() && s[i] == 'u'
                && (i + 1 >= s.size() || !std::isalnum(static_cast<unsigned char>(s[i + 1]))))
            {
                ++i;
                return v * u;
            }
            return v;
        }
        if (i < s.size() && (std::isalpha(static_cast<unsigned char>(s[i])) || s[i] == '_'))
        {
            std::size_t j = i;
            while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_' || s[j] == '.'))
                ++j;
            const std::string name = s.substr(i, j - i);
            i = j;
            if (Eat('('))
            {
                std::vector<float> a;
                if (!Eat(')'))
                {
                    do
                        a.push_back(Expr());
                    while (Eat(','));
                    if (!Eat(')'))
                        error = "missing )";
                }
                if (name == "min" && a.size() == 2)
                    return std::min(a[0], a[1]);
                if (name == "max" && a.size() == 2)
                    return std::max(a[0], a[1]);
                if (name == "clamp" && a.size() == 3)
                    return std::clamp(a[0], a[1], a[2]);
                error = "unknown function " + name;
                return 0.0f;
            }
            auto it = vars.find(name);
            if (it == vars.end())
            {
                error = "unknown name " + name;
                return 0.0f;
            }
            return it->second;
        }
        error = "unexpected '" + s.substr(i, 1) + "'";
        return 0.0f;
    }
};

float Eval(const std::string &text, const Vars &vars, float u, std::string *err = nullptr)
{
    ExprParser p {text, vars, u};
    const float v = p.Expr();
    p.Ws();
    if (p.error.empty() && p.i < text.size())
        p.error = "stray '" + text.substr(p.i, 1) + "'";
    if (!p.error.empty() && err && err->empty())
        *err = p.error + " in \"" + text + "\"";
    return v;
}

// ---- conditions: names with ! && || ( ) ----
struct CondParser
{
    const std::string &s;
    const Conditions &cond;
    std::size_t i {0};
    void Ws()
    {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
            ++i;
    }
    bool Or()
    {
        bool v = And();
        for (;;)
        {
            Ws();
            if (s.compare(i, 2, "||") == 0)
            {
                i += 2;
                const bool r = And();
                v = v || r;
            }
            else
                return v;
        }
    }
    bool And()
    {
        bool v = Not();
        for (;;)
        {
            Ws();
            if (s.compare(i, 2, "&&") == 0)
            {
                i += 2;
                const bool r = Not();
                v = v && r;
            }
            else
                return v;
        }
    }
    bool Not()
    {
        Ws();
        if (i < s.size() && s[i] == '!')
        {
            ++i;
            return !Not();
        }
        if (i < s.size() && s[i] == '(')
        {
            ++i;
            const bool v = Or();
            Ws();
            if (i < s.size() && s[i] == ')')
                ++i;
            return v;
        }
        std::size_t j = i;
        while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_' || s[j] == '.'))
            ++j;
        const std::string name = s.substr(i, j - i);
        i = j;
        return !name.empty() && cond && cond(name);
    }
};

bool EvalCond(const std::string &text, const Conditions &cond)
{
    if (text.empty())
        return true;
    CondParser p {text, cond};
    return p.Or();
}

} // namespace

struct Layout::Def
{
    struct NodeDef
    {
        std::string id, type, label, action, style, visibleIf, activeIf, disabledIf;
        std::string rect[4];
        bool hasRect {false};
        std::string dir; // flow: "row" / "column"
        std::string gap {"0"};
        std::string size[2]; // a child of a flow
        std::vector<NodeDef> children;
        float fontScale {1.0F};
    };
    std::string scale {"1"};
    std::vector<NodeDef> nodes;
};

namespace
{

std::string Scalar(const YAML::Node &n)
{
    return n && n.IsScalar() ? n.as<std::string>() : std::string();
}

Layout::Def::NodeDef ReadNode(const YAML::Node &n, std::string &error)
{
    Layout::Def::NodeDef d;
    d.id = Scalar(n["id"]);
    d.type = Scalar(n["type"]);
    d.label = Scalar(n["label"]);
    d.action = Scalar(n["action"]);
    d.style = Scalar(n["style"]);
    d.visibleIf = Scalar(n["visible_if"]);
    d.fontScale = Scalar(n["font_scale"]).empty() ? 1.0F : std::stof(Scalar(n["font_scale"]));
    d.activeIf = Scalar(n["active_if"]);
    d.disabledIf = Scalar(n["disabled_if"]);
    d.dir = Scalar(n["dir"]);
    if (n["gap"])
        d.gap = Scalar(n["gap"]);
    auto four = [&](const char *key, std::string *out, std::size_t count) {
        const YAML::Node r = n[key];
        if (!r)
            return false;
        if (!r.IsSequence() || r.size() != count)
        {
            error = "node '" + d.id + "': " + key + " needs " + std::to_string(count) + " values (line "
                    + std::to_string(r.Mark().line + 1) + ")";
            return false;
        }
        for (std::size_t k = 0; k < count; ++k)
            out[k] = Scalar(r[k]);
        return true;
    };
    d.hasRect = four("rect", d.rect, 4);
    four("size", d.size, 2);
    if (n["children"] && n["children"].IsSequence())
        for (const auto &c : n["children"])
            d.children.push_back(ReadNode(c, error));
    if (d.id.empty())
        error = "a node without id (line " + std::to_string(n.Mark().line + 1) + ")";
    return d;
}

} // namespace

bool Layout::Load(const std::string &yamlText, std::string &error)
{
    try
    {
        const YAML::Node root = YAML::Load(yamlText);
        const YAML::Node screens = root["screens"];
        if (!screens || !screens.IsMap())
        {
            error = "no 'screens' map";
            return false;
        }
        auto defs = std::make_shared<std::map<std::string, Def>>();
        std::string err;
        for (const auto &sc : screens)
        {
            for (const auto &pr : sc.second)
            {
                Def d;
                d.scale = pr.second["scale"] ? Scalar(pr.second["scale"]) : "1";
                if (pr.second["nodes"])
                    for (const auto &n : pr.second["nodes"])
                        d.nodes.push_back(ReadNode(n, err));
                (*defs)[sc.first.as<std::string>() + "/" + pr.first.as<std::string>()] = std::move(d);
            }
        }
        if (!err.empty())
        {
            error = err;
            return false;
        }
        m_defs = defs;
        return true;
    }
    catch (const std::exception &e)
    {
        error = e.what();
        return false;
    }
}

bool Layout::Has(const std::string &screen, const std::string &profile) const
{
    return m_defs && m_defs->count(screen + "/" + profile) > 0;
}

Solved Layout::Solve(
    const std::string &screen,
    const std::string &profile,
    int windowW,
    int windowH,
    const Conditions &cond,
    const std::map<std::string, float> *extra) const
{
    Solved out;
    if (!m_defs)
        return out;
    auto it = m_defs->find(screen + "/" + profile);
    if (it == m_defs->end())
        return out;
    Vars vars {{"W", static_cast<float>(windowW)}, {"H", static_cast<float>(windowH)}, {"u", 1.0f}};
    if (extra)
        for (const auto &kv : *extra)
            vars[kv.first] = kv.second;
    std::string err;
    out.u = std::max(0.25f, Eval(it->second.scale, vars, 1.0f, &err));
    vars["u"] = out.u;

    auto publish = [&](const std::string &id, const ui::UiRect &r) {
        vars[id + ".x"] = static_cast<float>(r.x);
        vars[id + ".y"] = static_cast<float>(r.y);
        vars[id + ".w"] = static_cast<float>(r.w);
        vars[id + ".h"] = static_cast<float>(r.h);
        vars[id + ".right"] = static_cast<float>(r.x + r.w);
        vars[id + ".bottom"] = static_cast<float>(r.y + r.h);
        vars[id + ".cx"] = r.x + r.w * 0.5f;
        vars[id + ".cy"] = r.y + r.h * 0.5f;
    };
    auto make = [&](const Def::NodeDef &d, const ui::UiRect &r, bool parentShown) {
        Node n;
        n.id = d.id;
        n.type = d.type;
        n.label = d.label;
        n.action = d.action;
        n.style = d.style;
        n.visibleIf = d.visibleIf;
        n.activeIf = d.activeIf;
        n.disabledIf = d.disabledIf;
        n.rect = r;
        n.visible = parentShown && EvalCond(d.visibleIf, cond);
        n.active = !d.activeIf.empty() && EvalCond(d.activeIf, cond);
        n.disabled = !d.disabledIf.empty() && EvalCond(d.disabledIf, cond);
        n.fontScale = d.fontScale;
        return n;
    };
    auto rounded = [](float v) {
        return static_cast<int>(std::lround(v));
    };

    for (const Def::NodeDef &d : it->second.nodes)
    {
        ui::UiRect r;
        if (d.hasRect)
        {
            r
                = {rounded(Eval(d.rect[0], vars, out.u, &err)),
                   rounded(Eval(d.rect[1], vars, out.u, &err)),
                   rounded(Eval(d.rect[2], vars, out.u, &err)),
                   rounded(Eval(d.rect[3], vars, out.u, &err))};
        }
        Node n = make(d, r, true);
        publish(d.id, r);
        out.nodes.push_back(n);
        if (d.type != "flow")
            continue;
        // A flow: children one after another along its rect, the hidden ones take no room, the ones that do not fit
        // are left out
        vars["parent.x"] = static_cast<float>(r.x);
        vars["parent.y"] = static_cast<float>(r.y);
        vars["parent.w"] = static_cast<float>(r.w);
        vars["parent.h"] = static_cast<float>(r.h);
        const bool column = d.dir == "column";
        const int gap = rounded(Eval(d.gap, vars, out.u, &err));
        int cursor = column ? r.y : r.x;
        for (const Def::NodeDef &c : d.children)
        {
            const int cw = rounded(Eval(c.size[0].empty() ? "0" : c.size[0], vars, out.u, &err));
            const int ch = rounded(Eval(c.size[1].empty() ? "0" : c.size[1], vars, out.u, &err));
            ui::UiRect cr = column ? ui::UiRect {r.x, cursor, cw > 0 ? cw : r.w, ch}
                                   : ui::UiRect {cursor, r.y, cw, ch > 0 ? ch : r.h};
            Node cn = make(c, cr, n.visible);
            if (cn.visible)
            {
                const int end = column ? cr.y + cr.h : cr.x + cr.w;
                const int limit = column ? r.y + r.h : r.x + r.w;
                if (end > limit)
                    cn.visible = false;
                else
                    cursor = end + gap;
            }
            publish(c.id, cr);
            out.nodes.push_back(cn);
        }
    }
    return out;
}

const Node *Solved::Find(const std::string &id) const
{
    for (const Node &n : nodes)
        if (n.id == id)
            return &n;
    return nullptr;
}

ui::UiRect Solved::Rect(const std::string &id) const
{
    const Node *n = Find(id);
    return n && n->visible ? n->rect : ui::UiRect {};
}

bool Solved::Shown(const std::string &id) const
{
    const Node *n = Find(id);
    return n && n->visible;
}

const Node *Solved::HitButton(int x, int y) const
{
    for (auto it = nodes.rbegin(); it != nodes.rend(); ++it)
        if (it->visible && it->type == "button" && it->rect.Contains(x, y))
            return &*it;
    return nullptr;
}

bool Solved::Covers(int x, int y) const
{
    for (const Node &n : nodes)
        if (n.visible && n.type != "flow" && n.type != "area" && n.rect.w > 0 && n.rect.Contains(x, y))
            return true;
    return false;
}

// ---- the game's layout ----
namespace
{
std::string g_menuProfile = "pc";
}
void SetMenuProfile(const std::string &profile)
{
    g_menuProfile = profile;
}
const std::string &MenuProfile(const Layout &layout, const std::string &screen)
{
    static const std::string pc = "pc";
    return layout.Has(screen, g_menuProfile) ? g_menuProfile : pc;
}

namespace
{
Layout g_layout;
std::string g_text, g_error;
unsigned g_lastPoll = 0;
bool g_loaded = false;
} // namespace

const Layout &Current()
{
    if (!g_loaded)
        Reload();
    return g_layout;
}

bool Reload()
{
    g_loaded = true;
    std::string text;
    if (!Assets::ReadText(LoaderResources::UiLayout(), text))
    {
        g_error = "layout file not found";
        return false;
    }
    g_text = text;
    std::string err;
    Layout next;
    if (!next.Load(text, err))
    {
        g_error = err;
        std::fprintf(
            stderr,
            "[UiLayout] %s: %s (the last good layout stays)\n",
            LoaderResources::UiLayout().c_str(),
            err.c_str());
        return false;
    }
    g_layout = next;
    g_error.clear();
    return true;
}

void Poll(unsigned nowMs)
{
    if (nowMs - g_lastPoll < 1000)
        return;
    g_lastPoll = nowMs;
    std::string text;
    if (!Assets::ReadText(LoaderResources::UiLayout(), text) || text == g_text)
        return;
    Reload();
}

const std::string &LastError()
{
    return g_error;
}

void DrawGeneric(Renderer &r, BitmapFontRenderer &font, const Solved &s, int mx, int my)
{
    for (const Node &n : s.nodes)
    {
        if (!n.visible)
            continue;
        if (n.type == "panel")
            ui::DrawPanel(r, n.rect);
        else if (n.type == "label")
            font.DrawString(
                ui::FitText(font, _(n.label.c_str()), ui::FONT_SMALL_BOLD, n.rect.w),
                n.rect.x,
                n.rect.CenterY(),
                ui::FONT_SMALL_BOLD,
                ui::color::Text,
                false,
                Anchor::LEFT | Anchor::VCENTER);
        else if (n.type == "button")
        {
            ui::ButtonStyle st = n.style == "primary"  ? ui::ButtonStyle::Primary
                                 : n.style == "danger" ? ui::ButtonStyle::Danger
                                 : n.style == "ghost"  ? ui::ButtonStyle::Ghost
                                                       : ui::ButtonStyle::Secondary;
            ui::DrawButton(
                r,
                font,
                n.rect,
                _(n.label.c_str()),
                n.active ? ui::ButtonStyle::Primary : st,
                n.rect.Contains(mx, my),
                !n.disabled,
                ui::ButtonFontForScale(n.fontScale));
        }
    }
}

} // namespace OpenArtOfWar::uilayout
