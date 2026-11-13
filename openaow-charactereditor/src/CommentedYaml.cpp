#include "CommentedYaml.hpp"
#include "ConfigMerge.hpp"

#include <fstream>
#include <sstream>

namespace CharEditor
{

namespace
{

std::string Trim(std::string_view s)
{
    std::size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string_view::npos)
        return {};
    std::size_t b = s.find_last_not_of(" \t\r");
    return std::string(s.substr(a, b - a + 1));
}

// Start of a trailing " # comment" outside quotes, npos when there is none
std::size_t TrailingComment(std::string_view s)
{
    char quote = 0;
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        const char c = s[i];
        if (quote)
        {
            if (c == '\\' && quote == '"')
                ++i;
            else if (c == quote)
                quote = 0;
        }
        else if (c == '"' || c == '\'')
        {
            quote = c;
        }
        else if (c == '#' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t'))
        {
            return i;
        }
    }
    return std::string::npos;
}

} // namespace

std::string CommentedYaml::text() const
{
    std::string out;
    for (const auto &l : m_lines)
    {
        out += l;
        out += '\n';
    }
    return out;
}

bool CommentedYaml::load(const std::filesystem::path &path, std::string &error)
{
    m_loaded = false;
    std::ifstream in(path);
    if (!in)
    {
        error = "Could not open " + path.string();
        return false;
    }
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(line);
    }
    try
    {
        std::ostringstream joined;
        for (const auto &l : lines)
            joined << l << '\n';
        m_node = YAML::Load(joined.str());
    }
    catch (const std::exception &e)
    {
        error = e.what();
        return false;
    }
    m_path = path;
    m_lines = std::move(lines);
    m_edits.clear();
    m_dirty = false;
    m_loaded = true;
    Index();
    return true;
}

bool CommentedYaml::Save(std::string &error)
{
    if (!m_loaded)
        return false;
    // The file as it is now, with the editor's edits applied again: whatever was added there since loading
    // stays. An edit whose key is gone from the file is dropped: the file's contents are never overwritten
    // with an older copy.
    CommentedYaml fresh;
    std::string loadError;
    const std::vector<std::pair<std::string, std::string>> edits = m_edits;
    if (fresh.load(m_path, loadError))
    {
        for (const auto &[path, value] : edits)
            fresh.Set(path, value);
        m_lines = fresh.m_lines;
        m_node = fresh.m_node;
        Index();
    }
    if (!WriteFileAtomic(m_path, text(), error))
        return false;
    m_edits.clear();
    m_dirty = false;
    return true;
}

void CommentedYaml::Index()
{
    m_lineOf.clear();
    m_comments.clear();
    struct Level
    {
        int indent;
        std::string path;
    };
    std::vector<Level> stack;
    std::vector<std::string> pending;
    for (std::size_t i = 0; i < m_lines.size(); ++i)
    {
        const std::string &l = m_lines[i];
        const std::string t = Trim(l);
        if (t.empty())
        {
            pending.clear();
            continue;
        }
        if (t[0] == '#')
        {
            pending.push_back(Trim(t.substr(1)));
            continue;
        }
        const std::size_t indent = l.find_first_not_of(' ');
        const std::size_t colon = t.find(':');
        if (t[0] == '-' || colon == std::string::npos || colon == 0)
        {
            pending.clear();
            continue;
        }
        std::string key = t.substr(0, colon);
        if (key.size() >= 2 && (key.front() == '"' || key.front() == '\''))
            key = key.substr(1, key.size() - 2);
        while (!stack.empty() && stack.back().indent >= static_cast<int>(indent))
            stack.pop_back();
        const std::string path = stack.empty() ? key : stack.back().path + "." + key;
        stack.push_back({static_cast<int>(indent), path});
        m_lineOf[path] = static_cast<int>(i);
        std::string comment;
        for (const auto &c : pending)
        {
            if (!comment.empty())
                comment += ' ';
            comment += c;
        }
        if (!comment.empty())
            m_comments[path] = comment;
        pending.clear();
    }
}

YAML::Node CommentedYaml::At(std::string_view path) const
{
    YAML::Node n = m_node; // a handle onto the same data; read only through const lookups below
    std::size_t start = 0;
    while (start <= path.size() && n.IsDefined())
    {
        const std::size_t dot = path.find('.', start);
        const std::string key(path.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start));
        if (!key.empty())
        {
            const YAML::Node &cn = n;
            if (!cn.IsMap())
                return YAML::Node();
            const YAML::Node child = cn[key];
            if (!child.IsDefined())
                return YAML::Node();
            n.reset(child); // rebind the handle (operator= would overwrite the data)
        }
        if (dot == std::string::npos)
            break;
        start = dot + 1;
    }
    return n;
}

std::string CommentedYaml::Comment(std::string_view path) const
{
    auto it = m_comments.find(path);
    return it == m_comments.end() ? std::string() : it->second;
}

bool CommentedYaml::Set(std::string_view path, std::string_view valueText)
{
    auto it = m_lineOf.find(path);
    if (it == m_lineOf.end())
        return false;
    const std::string old = m_lines[it->second];
    const std::size_t colon = old.find(':');
    if (colon == std::string::npos)
        return false;
    // Everything after the value that must stay: " # comment"
    const std::string rest = old.substr(colon + 1);
    const std::size_t c = TrailingComment(rest);
    std::string tail;
    if (c != std::string::npos)
    {
        std::size_t gap = c;
        while (gap > 0 && rest[gap - 1] == ' ')
            --gap;
        tail = "  " + rest.substr(c);
        (void) gap;
    }
    std::string newLine = old.substr(0, colon + 1);
    newLine += ' ';
    newLine += valueText;
    newLine += tail;
    m_lines[it->second] = std::move(newLine);
    try
    {
        m_node = YAML::Load(text());
    }
    catch (...)
    {
        m_lines[it->second] = old;
        return false;
    }
    m_edits.emplace_back(std::string(path), std::string(valueText));
    m_dirty = true;
    return true;
}

} // namespace CharEditor
