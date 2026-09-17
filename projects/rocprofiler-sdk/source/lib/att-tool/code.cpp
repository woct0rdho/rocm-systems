// MIT License
//
// Copyright (c) 2024-2025 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "code.hpp"
#include <nlohmann/json.hpp>
#include "lib/output/csv.hpp"
#include "outputfile.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_set>

namespace rocprofiler
{
namespace att_wrapper
{
using csv_encoder = rocprofiler::tool::csv::csv_encoder<8>;

std::string
demangle(std::string_view line);

// Builds a json filetree by recursively inserting "path" into the json object.
void
navigate(nlohmann::json& json, std::vector<std::string>& path, const std::string& filename)
{
    if(path.size() == 1) json[path.at(0)] = filename;

    if(path.size() <= 1) return;

    auto& j = json[path.at(0)];
    path.erase(path.begin());
    navigate(j, path, filename);
}

CodeFile::CodeFile(Fspath _dir, std::shared_ptr<AddressTable> _table)
: dir(std::move(_dir))
, table(std::move(_table))
{}

void
CodeFile::addCodeobj(uint64_t id)
{
    if(std::find(codeobj_ids.begin(), codeobj_ids.end(), id) == codeobj_ids.end())
        codeobj_ids.emplace_back(id);
}

void
CodeFile::addFallbackPC(pcinfo_t pc)
{
    if(pc.code_object_id == 0 && pc.address == 0) return;
    fallback_pcs.emplace_back(pc);
}

void
CodeFile::addZeroHitDisassembly()
{
    if(!isa_map.empty() || !table) return;

    auto emit_symbol = [this](uint64_t codeobj_id, const auto& symbol) {
        if(symbol.mem_size == 0) return false;

        bool emitted = false;
        auto symbol_pc = pcinfo_t{.address = symbol.vaddr, .code_object_id = codeobj_id};
        if(kernel_names.find(symbol_pc) == kernel_names.end())
        {
            auto name = KernelName{symbol.name, demangle(symbol.name)};
            kernel_names.emplace(symbol_pc, std::move(name));
        }

        for(auto addr = symbol.vaddr; addr < symbol.vaddr + symbol.mem_size;)
        {
            auto info = pcinfo_t{.address = addr, .code_object_id = codeobj_id};
            if(isa_map.find(info) != isa_map.end()) break;

            auto code_line = table->get(codeobj_id, addr);
            if(!code_line || code_line->size == 0) break;

            auto& cline = *(isa_map.emplace(info, std::make_unique<CodeLine>()).first->second);

            cline.line_number  = isa_map.size() + kernel_names.size() - 1;
            cline.code_line    = std::move(code_line);
            line_numbers[info] = cline.line_number;
            emitted            = true;

            addr += cline.code_line->size;
        }

        return emitted;
    };

    auto emit_for_pc = [this, &emit_symbol](pcinfo_t pc) {
        if(pc.code_object_id == 0) return false;

        const auto& symbols = table->getSymbolMap(pc.code_object_id);
        if(auto itr = symbols.find(pc.address); itr != symbols.end())
            return emit_symbol(pc.code_object_id, itr->second);

        for(const auto& [_, symbol] : symbols)
        {
            if(pc.address >= symbol.vaddr && pc.address < symbol.vaddr + symbol.mem_size)
                return emit_symbol(pc.code_object_id, symbol);
        }

        return false;
    };

    bool emitted_any = false;
    for(auto pc : fallback_pcs)
    {
        try
        {
            emitted_any |= emit_for_pc(pc);
        } catch(std::exception& e)
        {
            ROCP_INFO << "Unable to emit zero-hit ATT disassembly for occupancy PC "
                      << pc.code_object_id << ":" << pc.address << ": " << e.what();
        }
    }

    for(auto codeobj_id : codeobj_ids)
    {
        if(emitted_any) break;

        try
        {
            for(auto& [vaddr, symbol] : table->getSymbolMap(codeobj_id))
            {
                emitted_any |= emit_symbol(codeobj_id, symbol);
            }
        } catch(std::exception& e)
        {
            ROCP_INFO << "Unable to emit zero-hit ATT disassembly for code object " << codeobj_id
                      << ": " << e.what();
        }
    }

    if(emitted_any)
    {
        ROCP_WARNING << "ATT decode produced no target CU/SIMD wave instruction records; "
                     << "emitting zero-hit disassembly only. For instruction timing data, rerun "
                     << "with a larger workload or a different --att-target-cu/--att-simd-select.";
    }
}

CodeFile::~CodeFile()
{
    addZeroHitDisassembly();

    std::vector<std::pair<pcinfo_t, std::unique_ptr<CodeLine>>> vec;
    vec.reserve(isa_map.size());

    for(auto& [pc, isa] : isa_map)
        if(isa && isa->code_line) vec.emplace_back(pc, std::move(isa));

    isa_map.clear();
    line_numbers.clear();

    if(GlobalDefs::get().has_format("csv"))
    {
        // Write CSV, ordered by id + vaddr
        std::sort(vec.begin(),
                  vec.end(),
                  [](const std::pair<pcinfo_t, std::unique_ptr<CodeLine>>& a,
                     const std::pair<pcinfo_t, std::unique_ptr<CodeLine>>& b) {
                      return a.first < b.first;
                  });

        std::stringstream ofs;
        csv_encoder::write_row(ofs,
                               "CodeObj",
                               "Vaddr",
                               "Instruction",
                               "Hitcount",
                               "Latency",
                               "Stall",
                               "Idle",
                               "Source");

        for(auto& [pc, line] : vec)
        {
            if(kernel_names.find(pc) != kernel_names.end())
            {
                csv_encoder::write_row(ofs,
                                       pc.code_object_id,
                                       pc.address,
                                       "; " + kernel_names.at(pc).name,
                                       0,
                                       0,
                                       0,
                                       0,
                                       kernel_names.at(pc).demangled);
            }
            csv_encoder::write_row(ofs,
                                   pc.code_object_id,
                                   pc.address,
                                   line->code_line->inst,
                                   line->hitcount,
                                   line->latency,
                                   line->stall,
                                   line->idle,
                                   line->code_line->comment);
        }

        OutputFile file(dir.parent_path() / ("stats_" + dir.filename().string() + ".csv"));
        file << ofs.str();
    }

    if(!GlobalDefs::get().has_format("json")) return;

    // Write JSON, ordered by exec line number
    std::sort(vec.begin(),
              vec.end(),
              [](const std::pair<pcinfo_t, std::unique_ptr<CodeLine>>& a,
                 const std::pair<pcinfo_t, std::unique_ptr<CodeLine>>& b) {
                  return a.second->line_number < b.second->line_number;
              });

    nlohmann::json jcode = nlohmann::json::array();

    std::unordered_set<std::string> snapshots{};

    for(auto& line : vec)
    {
        auto& isa = *line.second;

        if(kernel_names.find(line.first) != kernel_names.end())
        {
            std::stringstream code;
            code << "[\"; " << kernel_names.at(line.first).name << "\",0," << (isa.line_number - 1)
                 << ",\"" << kernel_names.at(line.first).demangled << "\","
                 << line.first.code_object_id << "," << line.first.address << ",0,0,0,0]";
            jcode.push_back(nlohmann::json::parse(code.str()));
        }

        std::stringstream code;
        code << "[\"" << isa.code_line->inst << "\",0," << isa.line_number << ",\""
             << isa.code_line->comment << "\"," << line.first.code_object_id << ","
             << line.first.address << "," << isa.hitcount << "," << isa.latency << "," << isa.stall
             << "," << isa.idle << "]";

        jcode.push_back(nlohmann::json::parse(code.str()));

        auto&  comment  = isa.code_line->comment;
        size_t lineref  = comment.find(':');
        size_t previous = 0;

        // size() + 2 because we need at least ':' and one number after
        while(lineref != std::string::npos && lineref < comment.size() + 2)
        {
            auto source_ref = comment.substr(previous, lineref - previous);

            if(!source_ref.empty() && snapshots.find(source_ref) == snapshots.end())
                snapshots.insert(std::move(source_ref));

            previous = comment.find(CodeLine::Instruction::separator, lineref);
            if(previous == std::string::npos) break;

            previous += CodeLine::Instruction::separator.size();
            lineref = comment.find(':', previous);
        }
    }

    nlohmann::json json;
    json["code"]    = jcode;
    json["version"] = TOOL_VERSION;
    json["header"]  = "ISA, _, LineNumber, Source, Codeobj, Vaddr, Hit, Latency, Stall, Idle";

    nlohmann::json jfuncmap = nlohmann::json::array();
    for(auto cid : codeobj_ids)
    {
        auto fmap = table->getFuncmap(cid);
        if(!fmap) continue;

        for(const auto& entry_ptr : fmap->entries)
        {
            if(!entry_ptr || entry_ptr->name.empty()) continue;

            const char* kind_str = nullptr;
            switch(entry_ptr->kind)
            {
                case rocprofiler::sdk::codeobj::funcmap::FuncmapEntryKind::Function:
                    kind_str = "F";
                    break;
                case rocprofiler::sdk::codeobj::funcmap::FuncmapEntryKind::UserScope:
                    kind_str = "U";
                    break;
                case rocprofiler::sdk::codeobj::funcmap::FuncmapEntryKind::Point:
                    kind_str = "P";
                    break;
                case rocprofiler::sdk::codeobj::funcmap::FuncmapEntryKind::Kernel:
                    kind_str = "K";
                    break;
            }

            jfuncmap.push_back({cid,
                                entry_ptr->id,
                                kind_str,
                                entry_ptr->name,
                                entry_ptr->source_loc,
                                entry_ptr->vaddr});
        }
    }
    json["sqtt_funcmap"] = std::move(jfuncmap);

    OutputFile(dir / "code.json") << json;

    nlohmann::json jsnapfiletree;
    size_t         num_snap = 0;

    for(const auto& source_ref : snapshots)
    {
        if(rocprofiler::common::filesystem::exists(source_ref))
        {
            Fspath            filepath(source_ref);
            std::stringstream newfile;
            newfile << "source_" << (num_snap++) << '_' << filepath.filename().string();

            std::vector<std::string> path_elements(filepath.begin(), filepath.end());
            navigate(jsnapfiletree, path_elements, newfile.str());

            constexpr auto opt = rocprofiler::common::filesystem::copy_options::overwrite_existing;
            try
            {
                rocprofiler::common::filesystem::copy(filepath, dir / newfile.str(), opt);
            } catch(std::exception& e)
            {
                ROCP_WARNING << "Missing source file " << filepath << ": " << e.what();
                ROCP_CI_LOG(ERROR) << "Unable to copy source files: " << (dir / newfile.str());
            }
        }
    }

    if(num_snap != 0) OutputFile(dir / "snapshots.json") << jsnapfiletree;
}

}  // namespace att_wrapper
}  // namespace rocprofiler
