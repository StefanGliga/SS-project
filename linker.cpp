#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <string_view>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <regex>
#include <iomanip>
#include <format>

using namespace std;
using u32 = uint32_t;
using u64 = uint64_t;
using i32 = int32_t;
using i64 = int64_t;

struct Placement
{
    string name;
    u32 start;
};
struct Symbol
{
    string name;
    string section;
    u32 value = 0;
    char type;
    bool resolved = false;
};
struct Relocation
{
    u32 addend;
    string symbol;
    u32 offset;
};
struct Section
{
    string name;
    vector<char> data;
    vector<Relocation> rel;
};
struct ObjectFile
{
    unordered_map<string, Section> sections;
    unordered_map<string, Symbol> symbols;
};

vector<ObjectFile> object_files;
unordered_map<string, Section> combined_sections;
unordered_map<string, Symbol> combined_symbols;
string err_str;

ObjectFile& read_object(const string& filename)
{
    ifstream file(filename, ios::binary);
    if(not file)
    {
        err_str = "Ne mogu otvoriti fajl " + filename;
        throw runtime_error(err_str);
    }
    
    ObjectFile obj;

    u32 num_sections;
    file.read((char*)&num_sections, sizeof(num_sections));
    for(u32 i = 0; i < num_sections; i++)
    {
        u32 name_len;
        file.read((char*)&name_len, sizeof(name_len));
        string name(name_len, '\0');
        file.read(name.data(), name_len);
        u32 data_size;
        file.read((char*)&data_size, sizeof(data_size));
        vector<char> data(data_size);
        file.read(data.data(), data_size);
        Section sec;
        sec.name = name;
        sec.data = data;

        u32 num_relocations;
        file.read((char*)&num_relocations, sizeof(num_relocations));
        for(u32 i = 0; i < num_relocations; i++)
        {
            Relocation rel;
            file.read((char*)&rel.offset, sizeof(rel.offset));
            file.read((char*)&rel.addend, sizeof(rel.addend));
            u32 symbol_len;
            file.read((char*)&symbol_len, sizeof(symbol_len));
            rel.symbol.resize(symbol_len);
            file.read(rel.symbol.data(), symbol_len);
            sec.rel.push_back(std::move(rel));
        }
        obj.sections[name] = std::move(sec);
    }

    u32 num_symbols;
    file.read((char*)&num_symbols, sizeof(num_symbols));
    for(u32 i = 0; i < num_symbols; i++)
    {
        u32 name_len;
        file.read((char*)&name_len, sizeof(name_len));
        string name(name_len, '\0');
        file.read(name.data(), name_len);
        u32 value;
        file.read((char*)&value, sizeof(value));
        u32 section_len;
        file.read((char*)&section_len, sizeof(section_len));
        string section(section_len, '\0');
        file.read(section.data(), section_len);
        char type;
        file.read(&type, 1);
        Symbol sym;
        sym.name = name;
        sym.section = std::move(section);
        sym.value = value;
        sym.type = type;
        obj.symbols[name] = std::move(sym);
    }
    object_files.push_back(std::move(obj));

    file.close();
    return object_files.back();
}

void process_object(ObjectFile& obj)
{
    // first, global and local symbols with a section get the (running) lenght of that section from the combined_sections added to the value of the symbol
    for(auto& [name, sym] : obj.symbols)
    {
        if(sym.section.empty())
            continue;
        if(sym.type == 'e')
            continue;
        
        sym.value += combined_sections[sym.section].data.size();
        if(combined_sections[sym.section].name.empty())
            combined_sections[sym.section].name = sym.section; // fresh section
    }

    // then, non local symbols get added to the combined_symbols
    // globals as automatically resolved, error if multiple definitions
    // extern as unresolved, accepts multiple definitions
    for(auto& [name, sym] : obj.symbols)
    {
        if(sym.type == 'l')
            continue;
        
        if(sym.type == 'g')
        {
            if(combined_symbols.contains(name))
            {
                if(combined_symbols[name].resolved)
                {
                    if(not combined_symbols[name].value == 0 or not combined_symbols[name].section.empty()) // workaround
                    {
                        err_str = "Simbol " + name + " je vec definisan";
                        throw runtime_error(err_str);
                    }
                }
                auto &csym = combined_symbols[name];
                csym.value = sym.value;
                csym.section = sym.section;
                csym.resolved = true;
            }
            else
            {
                combined_symbols[name] = sym;
                combined_symbols[name].resolved = true;
            }
        }
        else
        {
            if(combined_symbols[name].name.empty()) // extern symbol not yet defined
                combined_symbols[name] = sym;
        }
    }
    
    // then, all sections are added to the combined_sections
    for(auto& [name, sec] : obj.sections)
    {
        Section& combined_sec = combined_sections[name];

        if(combined_sec.name.empty())
            combined_sec.name = name;
        // todo: revisit this
        for(auto& rel : sec.rel)
        {
            rel.offset += combined_sec.data.size();
            if(combined_sections.find(rel.symbol) != combined_sections.end())
            {
                rel.addend += combined_sections[rel.symbol].data.size();
            }
            combined_sec.rel.push_back(std::move(rel));
        }
    }

    for(auto& [name, sec] : obj.sections)
    {
        Section& combined_sec = combined_sections[name];
        combined_sec.data.insert(combined_sec.data.end(), sec.data.begin(), sec.data.end());
    }
}

void dump_relocatable(const string& file_name)
{
    string txtfilename = file_name + ".txt";
    ofstream txtfout(txtfilename);

    txtfout << "Sections:" << combined_sections.size() << endl;
    for(auto section : combined_sections)
    {
        auto& [name, sec] = section;
        txtfout << name << " " << sec.data.size() << endl;
    }
    for(auto section : combined_sections)
    {
        auto& [name, sec] = section;
        txtfout << "." << name << endl;
        int i = 0;
        
        for(char c : sec.data)
        {
            if(i % 16 == 0)
                txtfout << endl;
            
            txtfout << format("{:02X} ", c);
            
            i++;
        }
    }
    txtfout << endl;
    txtfout << dec;

    for(auto section : combined_sections)
    {
        auto& [name, sec] = section;
        txtfout << "." << name << ".rel" << endl;
        for(auto& rel : sec.rel)
        {
            txtfout << rel.offset << " " << rel.symbol << " " << rel.addend << endl;
        }
    }
    txtfout << "Symbols:" << combined_symbols.size() << endl;
    for(auto symbol : combined_symbols)
    {
        auto& [name, sym] = symbol;
        char type = 'g';
        if(not sym.resolved)
            type = 'e';
        txtfout << name << " " << sym.value << " " << sym.section << " " << type << endl;
    }
    txtfout.close();

    ofstream fout(file_name, ios::binary);
    u32 num_sections = combined_sections.size();
    fout.write((char*)&num_sections, sizeof(num_sections));
    // name length, name, data size, data
    for(auto section : combined_sections)
    {
        auto& [name, sec] = section;
        u32 name_len = name.size();
        fout.write((char*)&name_len, sizeof(name_len));
        fout.write(name.c_str(), name_len);
        u32 data_size = sec.data.size();
        fout.write((char*)&data_size, sizeof(data_size));
        fout.write(sec.data.data(), data_size);
    }
    for(auto section : combined_sections)
    {
        auto& [name, sec] = section;
        u32 num_relocations = sec.rel.size();
        fout.write((char*)&num_relocations, sizeof(num_relocations));
        for(auto& rel : sec.rel)
        {
            u32 offset = rel.offset;
            fout.write((char*)&offset, sizeof(offset));
            u32 addend = rel.addend;
            fout.write((char*)&addend, sizeof(addend));
            u32 symbol_len = rel.symbol.size();
            fout.write((char*)&symbol_len, sizeof(symbol_len));
            fout.write(rel.symbol.c_str(), symbol_len);
        }
    }

    u32 num_symbols = combined_symbols.size();
    fout.write((char*)&num_symbols, sizeof(num_symbols));
    // name length, name, value, section
    for(auto symbol : combined_symbols)
    {
        auto& [name, sym] = symbol;
        u32 name_len = name.size();
        fout.write((char*)&name_len, sizeof(name_len));
        fout.write(name.c_str(), name_len);
        fout.write((char*)&sym.value, sizeof(sym.value));
        u32 section_len = sym.section.size();
        fout.write((char*)&section_len, sizeof(section_len));
        fout.write(sym.section.c_str(), section_len);
        char type = 'g';
        if(not sym.resolved)
            type = 'e';
        fout.write(&type, 1);
    }
    fout.close();
}

void dump_hex(const string& file_name, const vector<Placement>& placements)
{
    unordered_map<string, u32> section_offsets;
    unordered_set<string> placed_sections;
    // preallocate 4Gb
    vector<char> data(4ull*1024*1024*1024);

    // first place sections in placements
    // keep track of the last section end
    u64 current_pos = 0;
    string last_placed_section;
    for(auto& placement : placements)
    {
        if(current_pos > placement.start)
        {
            err_str = "Sekcije " + last_placed_section + " i " + placement.name + " se preklapaju zbog -place direktiva";
            throw runtime_error(err_str);
        }


        section_offsets[placement.name] = placement.start;
        current_pos = placement.start;
        // write the section data
        auto& sec = combined_sections[placement.name];
        copy(sec.data.begin(), sec.data.end(), data.begin() + current_pos);
        current_pos += sec.data.size();
        last_placed_section = placement.name;
        placed_sections.insert(placement.name);
    }
    // then place the rest of the sections
    for(auto& section : combined_sections)
    {
        auto& [name, sec] = section;
        if(placed_sections.contains(name))
            continue;
        
        section_offsets[name] = current_pos;
        copy(sec.data.begin(), sec.data.end(), data.begin() + current_pos);
        current_pos += sec.data.size();
    }
    
    // then resolve relocations
    for(auto& section : combined_sections)
    {
        // TODO: double check this
        auto& [name, sec] = section;
        for(auto& rel : sec.rel)
        {
            // a relocation
            // in a section
            // means that at section location + offset
            // you write symbol value + addend
            // where the real symbol value is symbol value + symbol_section offset(0 if not in a section)

            if(combined_sections.find(rel.symbol) != combined_sections.end())
            {
                u32 value = section_offsets[rel.symbol] + rel.addend;
                u32 offset = section_offsets[name] + rel.offset;
                *(u32*)(data.data() + offset) = value;
            }
            else
            {
                Symbol& symbol = combined_symbols[rel.symbol];
                if(not symbol.resolved and not rel.symbol.empty())
                {
                    err_str = "Simbol " + rel.symbol + " nije definisan";
                    throw runtime_error(err_str);
                }
                u32 sec_offset = section_offsets[symbol.section];
                u32 value = symbol.value + rel.addend + sec_offset;
                u32 offset = section_offsets[name] + rel.offset;
                *(u32*)(data.data() + offset) = value;
            }
        }
    }

    
    ofstream txtfout(file_name + ".txt");
    // When dumping the text representation, dump only memory that is a part of a section
    // format: Section: name start: start length: length then hex data

    // first, sort all section offsets
    vector<pair<string, u32>> sorted_offsets(section_offsets.begin(), section_offsets.end());
    sort(sorted_offsets.begin(), sorted_offsets.end(), [](const pair<string, u32>& a, const pair<string, u32>& b) {
        return a.second < b.second;
    });

    for(auto& [name, start] : sorted_offsets)
    {
        auto& sec = combined_sections[name];
        txtfout << "Section: " << name << " start: " << start << " length: " << sec.data.size() << endl;
        int i = 0;
        const auto offf = section_offsets[name];
        for(int i = 0; i < sec.data.size(); i++)
        {
            if(i % 16 == 0)
                txtfout << endl;
            
            txtfout << format("{:02X} ", data[i + offf]);
        }
        txtfout << endl;
    }

    txtfout.close();

    ofstream fout(file_name, ios::binary);
    // for the hex file dump all 4Gb of data
    fout.write(data.data(), data.size());

    fout.close();
}

int main(int argc, char** argv)
{
    string out_filename = "a.hex";
    vector<string> object_files;
    vector<Placement> placements;
    bool hex = false;
    bool relocatable = false;
    for(int i = 1; i < argc; i++)
    {
        if(argv[i] == "-o"sv)
        {
            if(not (i+1<argc))
                throw runtime_error("-o naveden bez argumenta");
            
            out_filename = string(argv[i+1]);
            i++; continue;
        }
        if(argv[i] == "-hex"sv)
        {
            hex = true;
            continue;
        }
        if(argv[i] == "-relocatable"sv)
        {
            relocatable = true;
            continue;
        }
        if (std::string_view(argv[i]).starts_with("-place")) {
            // -place=name@start
            std::string_view place = argv[i] + 7;
            auto at = place.find('@');
            if (at == std::string_view::npos) {
                throw std::runtime_error("Nevalidan -place argument");
            }
            std::string_view name = place.substr(0, at);
            std::string_view start = place.substr(at + 1);
            placements.push_back(Placement{std::string(name), (u32)stoul(string(start), 0, 0)});
            continue;
        }

        object_files.push_back(argv[i]);
    }

    if(object_files.empty())
    {
        cout << "Nema ulaznih fajlova" << endl;
        return 1;
    }
    if(hex and relocatable)
    {
        cout << "Ne moze se koristiti -hex i -relocatable zajedno" << endl;
        return 1;
    }
    if((not hex) and (not relocatable))
    {
        cout << "Mora se koristiti -hex ili -relocatable" << endl;
        return 1;
    }
    if(relocatable and placements.size() > 0)
    {
        cout << "-place direktive navedene u -relocatable modu" << endl;
        return 1;
    }

    if (placements.size() > 0) {
        std::sort(placements.begin(), placements.end(), [](const Placement& a, const Placement& b) {
            return a.start < b.start;
        });
    }

    for(auto& file : object_files)
    {
        auto& obj = read_object(file);
        process_object(obj);
    }
    if(relocatable)
    {
        dump_relocatable(out_filename);
    }
    else
    {
        dump_hex(out_filename, placements);
    }
    
    
}