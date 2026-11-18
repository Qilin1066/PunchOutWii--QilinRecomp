// `qilin info` output. Developer tool code: std::format is fine here.
#include <format>
#include <string>

#include "dol.hpp"

namespace qilin {

std::string to_json(const Dol& dol) {
    const KnownBuild* build = known_build(dol.sha256);
    std::string j = "{\n";
    j += std::format("  \"sha256\": \"{}\",\n", dol.sha256);
    if (build)
        j += std::format("  \"build\": {{\"disc_id\": \"{}\", \"revision\": {}, \"title\": \"{}\"}},\n",
                         build->disc_id, build->revision, build->title);
    else
        j += "  \"build\": null,\n";
    j += std::format("  \"file_size\": {},\n", dol.file_size);
    j += std::format("  \"entry\": \"0x{:08X}\",\n", dol.entry);
    j += std::format("  \"bss\": {{\"address\": \"0x{:08X}\", \"size\": {}}},\n", dol.bss_address, dol.bss_size);
    j += std::format("  \"image\": {{\"base\": \"0x{:08X}\", \"end\": \"0x{:08X}\"}},\n", dol.base(), dol.image_end());
    uint64_t text = 0, data = 0;
    j += "  \"sections\": [\n";
    for (size_t i = 0; i < dol.sections.size(); ++i) {
        const DolSection& s = dol.sections[i];
        (s.text ? text : data) += s.size;
        j += std::format("    {{\"name\": \"{}\", \"offset\": \"0x{:X}\", \"address\": \"0x{:08X}\", \"size\": {}}}{}\n",
                         s.name(), s.offset, s.address, s.size, i + 1 < dol.sections.size() ? "," : "");
    }
    j += "  ],\n";
    j += std::format("  \"text_bytes\": {},\n  \"data_bytes\": {}\n}}\n", text, data);
    return j;
}

}  // namespace qilin
