#include "core/JsonFile.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>

namespace OpenArtOfWar::JsonFile
{

    namespace fs = std::filesystem;

    nlohmann::json readObject(const std::string &path)
    {
        std::ifstream in(path, std::ios::binary);
        if(!in)
            return nlohmann::json::object();
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        nlohmann::json j = nlohmann::json::parse(text, nullptr, false, true);
        return j.is_object() ? j : nlohmann::json::object();
    }

    bool writeAtomic(const std::string &path, const nlohmann::json &doc)
    {
        std::error_code ec;
        const fs::path target(path);
        if(target.has_parent_path())
            fs::create_directories(target.parent_path(), ec);
        const fs::path tmp = target.string() + ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if(!out)
                return false;
            out << doc.dump(2) << '\n';
            out.flush();
            if(!out.good())
                return false;
        }
        fs::rename(tmp, target, ec); // replaces atomically on POSIX and NTFS
        if(ec)
        {
            fs::remove(tmp, ec);
            return false;
        }
        return true;
    }

} // namespace OpenArtOfWar::JsonFile
