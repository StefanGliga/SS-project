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
// TODO: Switch over some types to signed

struct Symbol
{
    string name;
    string section;
    u32 value = 0;
    vector<pair<int, Symbol*>> expr;
    bool is_global = false;
    bool is_extern = false;
    bool resolved = false;
    vector<Symbol*> dependent_symbols;
};
struct Relocation
{
    u32 addend;
    string symbol; // may also be a section name
    u32 offset;
};
struct Section
{
    string name;
    vector<char> data;
    vector<Relocation> rel;
};

unordered_map<string, Symbol> symbols;
unordered_map<string, Section> sections;
Section* active_section = nullptr; // Optional<Section&> the generic version
bool file_end = false;
string err_str;

void ltrim(string_view& str)
{
    str.remove_prefix(min(str.find_first_not_of(" "), str.size()));
}
void rtrim(string_view& str)
{
    str.remove_suffix(str.size() - str.find_last_not_of(" ") - 1);
}

vector<string> parse_list(string_view str)
{
    // revisit this at the end
    vector<string> ret;
    regex re("\\s*,\\s*");
    auto it = cregex_token_iterator(str.begin(), str.end(), re, -1);
    auto end = cregex_token_iterator();
    for(; it != end; it++) {
        string token = it->str();
        token.erase(token.find_last_not_of(" ") + 1);
        token.erase(0, token.find_first_not_of(" "));
        if(token.empty())
            continue;
        ret.push_back(token);
    }
    return ret;
}

u32 parse_value(string_view str, int rel_offset = 0)
{
    u32 val = 0;
    try {
        val = stoull(string(str), 0, 0);
        return val;
    } catch (invalid_argument& e) {
        // not a fail, try to find symbol
    }
    
    // trim whitespace from the right, just in case
    rtrim(str);
    auto symname = string(str);
    auto& sym = symbols[symname];
    if(sym.resolved)
        return sym.value;
    if(sym.name.empty())
        sym.name = symname;
    active_section->rel.push_back(Relocation{0, symname, ((u32)active_section->data.size()) + rel_offset});
    return 0;
}

pair<u64, vector<pair<int, Symbol*>>> parse_expr(string_view str, Symbol& equd)
{
    u64 val = 0;
    vector<pair<int, Symbol*>> expr;
    regex re("\\s*([+-]?)\\s*([a-zA-Z0-9_]+)\\s*");
    auto it = cregex_iterator(str.begin(), str.end(), re);
    auto end = cregex_iterator();
    for (; it != end; ++it) 
    {
        string sign = it->str(1);
        string sym_or_num = it->str(2);
        int mul = (sign == "-") ? -1 : 1;
        u64 num_val = 0;
        try {
            num_val = stoull(sym_or_num, 0, 0);
            val += mul * num_val;
        } catch (invalid_argument& e) {
            auto& sym = symbols[sym_or_num];
            if(sym.resolved)
                val += mul * sym.value;
            else
            {
                if(sym.name.empty())
                    sym.name = sym_or_num;
                expr.push_back({mul, &sym});
                sym.dependent_symbols.push_back(&equd);
            }
        }
    }
    return {val, expr};
}

void propagate_resolution(Symbol& sym)
{
    if(not sym.resolved)
        throw runtime_error("Internal error: propagate_resolution called on unresolved symbol");

    auto it = sym.dependent_symbols.begin();
    for(; it != sym.dependent_symbols.end(); it++)
    {
        auto& dep_sym = **it;
        auto it2 = dep_sym.expr.begin();
        while(it2 != dep_sym.expr.end())
        {
            auto& [mul, sym_ptr] = *it2;
            if(sym_ptr == &sym)
            {
                dep_sym.value += mul * sym.value;

                // if(sym.section != "")
                // {
                //     if(dep_sym.section != "")
                //         throw runtime_error("Internal error: section conflict in symbol resolution");
                //     dep_sym.section = sym.section;
                // }

                it2 = dep_sym.expr.erase(it2);
                if(dep_sym.expr.size() == 0)
                {
                    dep_sym.resolved = true;
                    propagate_resolution(dep_sym);
                }
            }
            else
                it2++;
        }
    }
}

void process_assembly_directive(string_view str)
{
    if(str.starts_with(".global "))
    {
        str.remove_prefix(8);
        auto list = parse_list(str);
        for(auto& symbol_name : list)
        {
            symbols[symbol_name].is_global = true;
            if(symbols[symbol_name].name.empty()) // fresh symbol
                symbols[symbol_name].name = std::move(symbol_name);
        }
    } else if(str.starts_with(".section "))
    {
        str.remove_prefix(9);
        str = str.substr(0, min(str.find_first_of(" "), str.size()));
        string section_name = string(str);
        active_section = &sections[section_name];
        if(active_section->name.empty())
            active_section->name = std::move(section_name);
    } else if(str.starts_with(".extern "))
    {
        str.remove_prefix(8);
        auto list = parse_list(str);
        for(auto& symbol_name : list)
        {
            symbols[symbol_name].is_extern = true;
            symbols[symbol_name].name = std::move(symbol_name); // extern symbols are always fresh
        }
    } else if(str.starts_with(".word "))
    {
        if(not active_section)
            throw runtime_error(".word direktiva pre prve sekcije");

        str.remove_prefix(6);
        auto list = parse_list(str);
        for (auto& word : list)
        {
            size_t end;
            auto value = parse_value(word, 0);
            active_section->data.push_back(value & 0xFF);
            active_section->data.push_back((value >> 8) & 0xFF);
            active_section->data.push_back((value >> 16) & 0xFF);
            active_section->data.push_back((value >> 24) & 0xFF);
        }
            
    } else if(str.starts_with(".skip "))
    {
        if(not active_section)
            throw runtime_error(".skip direktiva pre prve sekcije");

        str.remove_prefix(6);
        size_t size = stoull(string(str), nullptr, 0);
        active_section->data.resize(active_section->data.size() + size, 0);
    } else if(str.starts_with(".equ "))
    {
        str.remove_prefix(5);
        auto tmp = parse_list(str);
        if(tmp.size() != 2)
        {
            err_str = "Neispravan format .equ direktive:" + string(str);
            throw runtime_error(err_str);
        }
        auto& sym = symbols[tmp[0]];
        if (sym.resolved or sym.expr.size() != 0)
        {
            err_str = "Pokusaj redefinicije simbola:" + sym.name;
            throw runtime_error(err_str);
        }
        if(sym.name.empty())
            sym.name = std::move(tmp[0]);
        auto ret = parse_expr(tmp[1], sym);
        sym.value = ret.first;
        if(ret.second.size() == 0)
        {
            sym.resolved = true;
            propagate_resolution(sym);
        }
        else
            sym.expr = std::move(ret.second);
    } else if(str.starts_with(".ascii "))
    {
        if(not active_section)
            throw runtime_error(".ascii direktiva pre prve sekcije");

        str.remove_prefix(7);
        // regex to extract everything between quotes
        regex re("\"([^\"]*)\"");
        cmatch match;
        if(regex_search(str.begin(), str.end(), match, re))
        {
            string ascii_str = match[1].str();
            active_section->data.insert(active_section->data.end(), ascii_str.begin(), ascii_str.end());
            active_section->data.insert(active_section->data.end(), 0);
        }
        else {
            err_str = "Neispravan format .ascii direktive:" + string(str);
            throw runtime_error(err_str);
        }
    }  else if (str.starts_with(".end")) 
    {
        file_end = true;
    }
    else {
        err_str = "Nepoznata direktiva:" + string(str);
        throw runtime_error(err_str);
    }
}

int parse_gpr_opr(string_view str)
{
    if(str == "%sp")
        return 14;
    if(str == "%pc")
        return 15;

    std::regex re("^%r([0-9]|1[0-5])$");
    std::cmatch match;
    if(not std::regex_match(str.begin(), str.end(), match, re))
        return -1;
    auto val = std::stoi(match[1].str());
    return val;
}
int parse_csr_opr(string_view str)
{
    if(str == "%status")
        return 0;
    if(str == "%handler")
        return 1;
    if(str == "%cause")
        return 2;
    return -1;
}
void emit_imm_intolitpool(string opr, char* extra_bytes, int& extra_len, int extra_offset = 0)
{
    i64 val;
    try {
        val = stoll(opr, 0, 0);
    }
    catch (invalid_argument& e) {
        // not a fail, try to find symbol
        auto& sym = symbols[opr];
        if(sym.resolved and sym.section == "")
            val = sym.value;
        else
        {
            if(sym.name.empty())
                sym.name = opr;
            active_section->rel.push_back(Relocation{0, opr, ((u32)active_section->data.size()) + 8 + extra_offset}); // instr is 4 bytes, jump after another 4, literal pool after that
            val = 0;
        }
    }
    extra_bytes[0] = 0x30;
    extra_bytes[1] = 15u << 4;
    extra_bytes[2] = 0x00;
    extra_bytes[3] = 0x04;
    // now pack val into the literal pool
    extra_bytes[4] = val & 0xFF;
    extra_bytes[5] = (val >> 8) & 0xFF;
    extra_bytes[6] = (val >> 16) & 0xFF;
    extra_bytes[7] = (val >> 24) & 0xFF;
    extra_len += 8;
}

void process_instruction(string_view str)
{
    // Format:
    // (label:)? (instr) (operands)?

    // use regex to extract label, instr and operands
    regex re("([a-zA-Z_][a-zA-Z0-9_]*:)?\\s*([a-zA-Z]+)?\\s*(.*)?");
    cmatch match;
    if(not regex_match(str.begin(), str.end(), match, re))
    {
        err_str = "Neispravan format instrukcije:" + string(str);
        throw runtime_error(err_str);
    }

    string label = match[1].str();
    string instr = match[2].str();
    string operands = match[3].str();

    if(not label.empty())
    {
        // remove the colon
        label.pop_back();

        auto& sym = symbols[label];
        if(sym.resolved)
        {
            err_str = "Pokusaj redefinicije simbola:" + label;
            throw runtime_error(err_str);
        }
        if(sym.name.empty())
            sym.name = label;
        sym.value = active_section->data.size();
        sym.section = active_section->name;
        sym.resolved = true;
        if(sym.dependent_symbols.size())
            propagate_resolution(sym);
    }

    if(instr.empty())
        return;

    // remove ALL whitespace from operands
    operands.erase(remove_if(operands.begin(), operands.end(), ::isspace), operands.end());

    char i_bytes[4] = {0,0,0,0};
    char extra_bytes[16];
    int extra_len = 0;
    
    auto opr_list = parse_list(operands);
    if(opr_list.size() > 3)
    {
        err_str = "Previse operanada u instrukciji:" + string(str);
        throw runtime_error(err_str);
    }

    // reg A is packed in the high 4 bits of the 2nd byte, reg B is packed in the low 4 bits
    // reg C in the high 4 bits of the 3rd byte
    // imm is packed in the low 4 bits of the 3rd byte and in the 4th byte

    if(instr == "halt")
    {
        i_bytes[0] = 0x00;
        if(opr_list.size() != 0)
        {
            err_str = "neispravan format instrukcije" + string(str);
            throw runtime_error(err_str);
        }
    } else if (instr == "intr" or instr == "int")
    {
        i_bytes[0] = 0x10;
        if(opr_list.size() != 0)
        {
            err_str = "neispravan format instrukcije " + string(str);
            throw runtime_error(err_str);
        }
    } else if (instr == "call")
    {
        i_bytes[0] = 0x21;
        if(opr_list.size() != 1)
        {
            err_str = "neispravan format instrukcije" + string(str);
            throw runtime_error(err_str);
        }
        auto opr = opr_list[0];
        regex re = regex("[a-zA-Z0-9_]+");
        if(not regex_match(opr.begin(), opr.end(), re))
        {
            err_str = "neispravan format operanda u instrukciji" + string(str);
            throw runtime_error(err_str);
        }
        // we always call trough the literal pool, embedded just after the instruction
        // extra bytes first contain a jump pcrel+4 instruction, then the literal pool
        i_bytes[1] = 15 << 4;
        i_bytes[2] = 0x00;
        i_bytes[3] = 0x04; // after the end of instruction, there is a jump instruction
        emit_imm_intolitpool(opr, extra_bytes, extra_len);
    } else if (instr == "jmp")
    {
        i_bytes[0] = 0x38;
        if(opr_list.size() != 1)
        {
            err_str = "neispravan format instrukcije" + string(str);
            throw runtime_error(err_str);
        }
        auto opr = opr_list[0];
        regex re = regex("[a-zA-Z0-9_]+");
        if(not regex_match(opr.begin(), opr.end(), re))
        {
            err_str = "neispravan format operanda u instrukciji" + string(str);
            throw runtime_error(err_str);
        }
        i_bytes[1] = 15 << 4;
        i_bytes[2] = 0x00;
        i_bytes[3] = 0x04; // after the end of instruction, there is a jump instruction
        emit_imm_intolitpool(opr, extra_bytes, extra_len);
    } else if (instr == "beq")
    {
        i_bytes[0] = 0x39;
        if(opr_list.size() != 3)
        {
            err_str = "neispravan format instrukcije" + string(str);
            throw runtime_error(err_str);
        }
        int opr1 = parse_gpr_opr(opr_list[0]);
        int opr2 = parse_gpr_opr(opr_list[1]);
        if(opr1 == -1 or opr2 == -1)
        {
            err_str = "neispravan format registarskog operanda " + string(str);
            throw runtime_error(err_str);
        }
        // opr 1 and 2 go into B and C, A is always 15
        i_bytes[1] = 15 << 4 | opr1;
        i_bytes[2] = opr2 << 4;
        i_bytes[3] = 0x04; // after the end of instruction, there is a jump instruction
        emit_imm_intolitpool(opr_list[2], extra_bytes, extra_len);
    } else if (instr == "bne")
    {
        i_bytes[0] = 0x3A;
        if(opr_list.size() != 3)
        {
            err_str = "neispravan format instrukcije" + string(str);
            throw runtime_error(err_str);
        }
        int opr1 = parse_gpr_opr(opr_list[0]);
        int opr2 = parse_gpr_opr(opr_list[1]);
        if(opr1 == -1 or opr2 == -1)
        {
            err_str = "neispravan format registarskog operanda " + string(str);
            throw runtime_error(err_str);
        }
        // opr 1 and 2 go into B and C, A is always 15
        i_bytes[1] = 15 << 4 | opr1;
        i_bytes[2] = opr2 << 4;
        i_bytes[3] = 0x04; // after the end of instruction, there is a jump instruction
        emit_imm_intolitpool(opr_list[2], extra_bytes, extra_len);
    } else if (instr == "bgt")
    {
        i_bytes[0] = 0x3B;
        if(opr_list.size() != 3)
        {
            err_str = "neispravan format instrukcije" + string(str);
            throw runtime_error(err_str);
        }
        int opr1 = parse_gpr_opr(opr_list[0]);
        int opr2 = parse_gpr_opr(opr_list[1]);
        if(opr1 == -1 or opr2 == -1)
        {
            err_str = "neispravan format registarskog operanda " + string(str);
            throw runtime_error(err_str);
        }
        // opr 1 and 2 go into B and C, A is always 15
        i_bytes[1] = 15 << 4 | opr1;
        i_bytes[2] = opr2 << 4;
        i_bytes[3] = 0x04; // after the end of instruction, there is a jump instruction
        emit_imm_intolitpool(opr_list[2], extra_bytes, extra_len);
    } else if (instr == "xchg")
    {
        i_bytes[0] = 0x40;
        if(opr_list.size() != 2)
        {
            err_str = "neispravan format instrukcije " + string(str);
            throw runtime_error(err_str);
        }
        int opr1 = parse_gpr_opr(opr_list[0]);
        int opr2 = parse_gpr_opr(opr_list[1]);
        if(opr1 == -1 or opr2 == -1)
        {
            err_str = "neispravan format registarskog operanda " + string(str);
            throw runtime_error(err_str);
        }
        // xchg is weird and uses registers B and C in the instruction
        i_bytes[1] = opr1;
        i_bytes[2] = opr2 << 4;
    } else if (instr == "add")
    {
        i_bytes[0] = 0x50;
        if(opr_list.size() != 2)
        {
            err_str = "neispravan format instrukcije " + string(str);
            throw runtime_error(err_str);
        }
        int opr1 = parse_gpr_opr(opr_list[0]);
        int opr2 = parse_gpr_opr(opr_list[1]);
        if(opr1 == -1 or opr2 == -1)
        {
            err_str = "neispravan format registarskog operanda " + string(str);
            throw runtime_error(err_str);
        }
        i_bytes[1] = opr2 << 4 | opr2;
        i_bytes[2] = opr1 << 4;
    } else if (instr == "sub")
    {
        i_bytes[0] = 0x51;
        if(opr_list.size() != 2)
        {
            err_str = "neispravan format instrukcije " + string(str);
            throw runtime_error(err_str);
        }
        int opr1 = parse_gpr_opr(opr_list[0]);
        int opr2 = parse_gpr_opr(opr_list[1]);
        if(opr1 == -1 or opr2 == -1)
        {
            err_str = "neispravan format registarskog operanda " + string(str);
            throw runtime_error(err_str);
        }
        i_bytes[1] = opr2 << 4 | opr2;
        i_bytes[2] = opr1 << 4;
    } else if (instr == "mul")
    {
        i_bytes[0] = 0x52;
        if(opr_list.size() != 2)
        {
            err_str = "neispravan format instrukcije " + string(str);
            throw runtime_error(err_str);
        }
        int opr1 = parse_gpr_opr(opr_list[0]);
        int opr2 = parse_gpr_opr(opr_list[1]);
        if(opr1 == -1 or opr2 == -1)
        {
            err_str = "neispravan format registarskog operanda " + string(str);
            throw runtime_error(err_str);
        }
        i_bytes[1] = opr2 << 4 | opr2;
        i_bytes[2] = opr1 << 4;
    } else if (instr == "div")
    {
        i_bytes[0] = 0x53;
        if(opr_list.size() != 2)
        {
            err_str = "neispravan format instrukcije " + string(str);
            throw runtime_error(err_str);
        }
        int opr1 = parse_gpr_opr(opr_list[0]);
        int opr2 = parse_gpr_opr(opr_list[1]);
        if(opr1 == -1 or opr2 == -1)
        {
            err_str = "neispravan format registarskog operanda " + string(str);
            throw runtime_error(err_str);
        }
        i_bytes[1] = opr2 << 4 | opr2;
        i_bytes[2] = opr1 << 4;
    } else if (instr == "not")
    {
        i_bytes[0] = 0x60;
        if(opr_list.size() != 1)
        {
            err_str = "neispravan format instrukcije " + string(str);
            throw runtime_error(err_str);
        }
        int opr1 = parse_gpr_opr(opr_list[0]);
        if(opr1 == -1)
        {
            err_str = "neispravan format registarskog operanda " + string(str);
            throw runtime_error(err_str);
        }
        i_bytes[1] = opr1 << 4 | opr1;
    } else if (instr == "and")
    {
        i_bytes[0] = 0x61;
        if(opr_list.size() != 2)
        {
            err_str = "neispravan format instrukcije " + string(str);
            throw runtime_error(err_str);
        }
        int opr1 = parse_gpr_opr(opr_list[0]);
        int opr2 = parse_gpr_opr(opr_list[1]);
        if(opr1 == -1 or opr2 == -1)
        {
            err_str = "neispravan format registarskog operanda " + string(str);
            throw runtime_error(err_str);
        }
        i_bytes[1] = opr2 << 4 | opr2;
        i_bytes[2] = opr1 << 4;
    } else if (instr == "or")
    {
        i_bytes[0] = 0x62;
        if(opr_list.size() != 2)
        {
            err_str = "neispravan format instrukcije " + string(str);
            throw runtime_error(err_str);
        }
        int opr1 = parse_gpr_opr(opr_list[0]);
        int opr2 = parse_gpr_opr(opr_list[1]);
        if(opr1 == -1 or opr2 == -1)
        {
            err_str = "neispravan format registarskog operanda " + string(str);
            throw runtime_error(err_str);
        }
        i_bytes[1] = opr2 << 4 | opr2;
        i_bytes[2] = opr1 << 4;
    } else if (instr == "xor")
    {
        i_bytes[0] = 0x63;
        if(opr_list.size() != 2)
        {
            err_str = "neispravan format instrukcije " + string(str);
            throw runtime_error(err_str);
        }
        int opr1 = parse_gpr_opr(opr_list[0]);
        int opr2 = parse_gpr_opr(opr_list[1]);
        if(opr1 == -1 or opr2 == -1)
        {
            err_str = "neispravan format registarskog operanda " + string(str);
            throw runtime_error(err_str);
        }
        i_bytes[1] = opr2 << 4 | opr2;
        i_bytes[2] = opr1 << 4;
    } else if (instr == "shl")
    {
        i_bytes[0] = 0x70;
        if(opr_list.size() != 2)
        {
            err_str = "neispravan format instrukcije " + string(str);
            throw runtime_error(err_str);
        }
        int opr1 = parse_gpr_opr(opr_list[0]);
        int opr2 = parse_gpr_opr(opr_list[1]);
        if(opr1 == -1 or opr2 == -1)
        {
            err_str = "neispravan format registarskog operanda " + string(str);
            throw runtime_error(err_str);
        }
        i_bytes[1] = opr2 << 4 | opr2;
        i_bytes[2] = opr1 << 4;
    } else if (instr == "shr")
    {
        i_bytes[0] = 0x71;
        if(opr_list.size() != 2)
        {
            err_str = "neispravan format instrukcije " + string(str);
            throw runtime_error(err_str);
        }
        int opr1 = parse_gpr_opr(opr_list[0]);
        int opr2 = parse_gpr_opr(opr_list[1]);
        if(opr1 == -1 or opr2 == -1)
        {
            err_str = "neispravan format registarskog operanda " + string(str);
            throw runtime_error(err_str);
        }
        i_bytes[1] = opr2 << 4 | opr2;
        i_bytes[2] = opr1 << 4;

    } else if (instr == "st")
    {
        i_bytes[0] = 0x80;
        if(opr_list.size() != 2)
        {
            err_str = "neispravan format instrukcije " + string(str);
            throw runtime_error(err_str);
        }
        int opr1 = parse_gpr_opr(opr_list[0]);
        if(opr1 == -1)
        {
            err_str = "neispravan format registarskog operanda " + string(str);
            throw runtime_error(err_str);
        }
        auto opr2 = string_view(opr_list[1]);
        if(opr2[0] == '[')
        {
            std::regex re(R"(\[(%[A-Za-z0-9_]+)\s*([\+\-])?\s*([A-Za-z0-9_]+)?\])");
            std::cmatch match;
            if(not regex_match(opr2.begin(), opr2.end(), match, re))
            {
                err_str = "neispravan format memorijskog operanda " + string(str);
                throw runtime_error(err_str);
            }
            int opr2_real = parse_gpr_opr(match[1].str());
            if(opr2_real == -1)
            {
                err_str = "neispravan format registarskog operanda " + string(str);
                throw runtime_error(err_str);
            }
            // check if all groups matched
            long long D = 0;
            if(match[2].matched)
            {
                int sign = match[2].str() == "+" ? 1 : -1;
                try {
                    D = std::stoll(match[3].str(), 0, 0) * sign;
                } catch (std::invalid_argument& e) {
                    auto it = symbols.find(match[3].str());
                    if(it != symbols.end())
                    {
                        auto& sym = it->second;
                        if(sym.resolved)
                            D = sym.value;
                        else
                            throw runtime_error("Neresen simbol u memorijskom operandu:" + string(str));
                    }
                }
            }
            if(D > 0xFFF or D < -0xFFF)
            {
                err_str = "Prevelika vrednost za D u memorijskom operandu:" + string(str);
                throw runtime_error(err_str);
            }
            i_bytes[1] = opr2_real;
            i_bytes[2] = opr1 << 4 | (D >> 8 & 0xF);
            i_bytes[3] = D & 0xFF;
        }
        else
        {
            i_bytes[0] += 2;
            i_bytes[1] = 15 << 4;
            i_bytes[2] = opr1 << 4;
            i_bytes[3] = 4;
            emit_imm_intolitpool(opr_list[1], extra_bytes, extra_len);
        }
    } else if (instr == "ld")
    {
        i_bytes[0] = 0x90;
        if(opr_list.size() != 2)
        {
            err_str = "neispravan format instrukcije " + string(str);
            throw runtime_error(err_str);
        }
        int opr2 = parse_gpr_opr(opr_list[1]);
        if(opr2 == -1)
        {
            err_str = "neispravan format registarskog operanda " + string(str);
            throw runtime_error(err_str);
        }
        auto opr1 = string_view(opr_list[0]);
        // format is one of:
        // $imm
        // imm
        // %rX
        // [%rX +/- imm]

        if(opr1[0] == '$')
        {
            opr1.remove_prefix(1);
            emit_imm_intolitpool(string(opr1), extra_bytes, extra_len);
            i_bytes[0] += 2;
            i_bytes[1] = opr2 << 4 | 15;
            i_bytes[2] = 0;
            i_bytes[3] = 4;
        } else if(opr1[0] == '%')
        {
            auto opr1_real = parse_gpr_opr(opr1);
            if(opr1_real == -1)
            {
                err_str = "neispravan format registarskog operanda " + string(str);
                throw runtime_error(err_str);
            }
            i_bytes[0] += 1;
            i_bytes[1] = opr2 << 4 | opr1_real;
        } else if(opr1[0] == '[')
        {
            std::regex re(R"(\[(%[A-Za-z0-9_]+)\s*([\+\-])?\s*([A-Za-z0-9_]+)?\])");
            std::cmatch match;
            if(not regex_match(opr1.begin(), opr1.end(), match, re))
            {
                err_str = "neispravan format memorijskog operanda " + string(str);
                throw runtime_error(err_str);
            }
            int opr1_real = parse_gpr_opr(match[1].str());
            if(opr1_real == -1)
            {
                err_str = "neispravan format registarskog operanda " + string(str);
                throw runtime_error(err_str);
            }
            // check if all groups matched
            long long D = 0;
            if(match[2].matched)
            {
                int sign = match[2].str() == "+" ? 1 : -1;
                try {
                    D = std::stoll(match[3].str(), 0, 0) * sign;
                } catch (std::invalid_argument& e) {
                    auto it = symbols.find(match[3].str());
                    if(it != symbols.end())
                    {
                        auto& sym = it->second;
                        if(sym.resolved)
                            D = sym.value;
                        else
                            throw runtime_error("Neresen simbol u memorijskom operandu:" + string(str));
                    }
                }
            }
            if(D > 0xFFF or D < -0xFFF)
            {
                err_str = "Prevelika vrednost za D u memorijskom operandu:" + string(str);
                throw runtime_error(err_str);
            }
            i_bytes[0] += 2;
            i_bytes[1] = opr2 << 4 | opr1_real;
            i_bytes[2] = (D >> 8) & 0xF;
            i_bytes[3] = D & 0xFF;
        }
        else
        {
            // literal
            // the hardest case
            // has to be implemented using 2 loads, one to load the literal as adress into a register,
            // and the other to load the value from that adress
            // first instruction: load into reg opr2, pc+8(+4 to skip the 2nd load, another +4 to skip the jump)
            i_bytes[0] += 2;
            i_bytes[1] = opr2 << 4 | 15;
            i_bytes[2] = 0;
            i_bytes[3] = 8;
            // second instruction: load the value from the adress in opr2, into opr2
            extra_bytes[0] = 0x92;
            extra_bytes[1] = opr2 << 4 | opr2;
            extra_bytes[2] = 0;
            extra_bytes[3] = 0;
            extra_len += 4;
            // then the literal pool
            emit_imm_intolitpool(opr_list[0], extra_bytes+4, extra_len, 4);
        }
    }  else if (instr == "iret")
    {
        // iret is a special case. revisit
        i_bytes[0] = 0x96;
        i_bytes[1] = 14;
        i_bytes[2] = 0;
        i_bytes[3] = 4;
        extra_bytes[0] = 0x93;
        extra_bytes[1] = 15 << 4 | 14;
        extra_bytes[2] = 0;
        extra_bytes[3] = 8;
        extra_len += 4;
    } else if (instr == "ret")
    {
        i_bytes[0] = 0x93;
        if(opr_list.size() != 0)
        {
            err_str = "neispravan format instrukcije " + string(str);
            throw runtime_error(err_str);
        }
        i_bytes[1] = 15 << 4 | 14;
        i_bytes[3] = 4;
    } else if (instr == "push")
    {
        i_bytes[0] = 0x81;
        if(opr_list.size() != 1)
        {
            err_str = "neispravan format instrukcije " + string(str);
            throw runtime_error(err_str);
        }
        int opr1 = parse_gpr_opr(opr_list[0]);
        if(opr1 == -1)
        {
            err_str = "neispravan format registarskog operanda " + string(str);
            throw runtime_error(err_str);
        }
        const auto imm = -4;
        i_bytes[1] = 14 << 4;
        i_bytes[2] = opr1 << 4 | (imm >> 8 & 0xF);
        i_bytes[3] = imm & 0xFF;
    } else if (instr == "pop")
    {
        i_bytes[0] = 0x93;
        if(opr_list.size() != 1)
        {
            err_str = "neispravan format instrukcije " + string(str);
            throw runtime_error(err_str);
        }
        int opr1 = parse_gpr_opr(opr_list[0]);
        if(opr1 == -1)
        {
            err_str = "neispravan format registarskog operanda " + string(str);
            throw runtime_error(err_str);
        }
        i_bytes[1] = opr1 << 4 | 14;
        i_bytes[3] = 4;
    } else if (instr == "csrwr")
    {
        i_bytes[0] = 0x94;
        if(opr_list.size() != 2)
        {
            err_str = "neispravan format instrukcije " + string(str);
            throw runtime_error(err_str);
        }
        int opr1 = parse_gpr_opr(opr_list[0]);
        int opr2 = parse_csr_opr(opr_list[1]);
        if(opr1 == -1 or opr2 == -1)
        {
            err_str = "neispravan format registarskog operanda " + string(str);
            throw runtime_error(err_str);
        }
        i_bytes[1] = opr2 << 4 | opr1;
    } else if (instr == "csrrd")
    {
        i_bytes[0] = 0x90;
        
        if(opr_list.size() != 2)
        {
            err_str = "neispravan format instrukcije " + string(str);
            throw runtime_error(err_str);
        }
        int opr1 = parse_csr_opr(opr_list[0]);
        int opr2 = parse_gpr_opr(opr_list[1]);
        if(opr1 == -1 or opr2 == -1)
        {
            err_str = "neispravan format registarskog operanda " + string(str);
            throw runtime_error(err_str);
        }
        i_bytes[1] = opr2 << 4 | opr1;
    } else {
        err_str = "Nepoznata instrukcija:" + instr;
        throw runtime_error(err_str);
    }

    active_section->data.push_back(i_bytes[0]);
    active_section->data.push_back(i_bytes[1]);
    active_section->data.push_back(i_bytes[2]);
    active_section->data.push_back(i_bytes[3]);
    active_section->data.insert(active_section->data.end(), extra_bytes, extra_bytes + extra_len);
}



void parse_line(string_view str)
{
    ltrim(str);
    rtrim(str);

    // remove comments
    auto comment_pos = str.find_first_of("#");
    if(comment_pos != string::npos)
        str.remove_suffix(str.size() - comment_pos);
    
    if(str.empty())
        return;

    if(str[0] == '.')
        process_assembly_directive(str);
    else
        process_instruction(str);
}

void fix_rel()
{
    // for each section, for each relocation
    // if it is wrt a local symbol, replace it with a section
    // and change the addend to be the offset of the symbol in the section

    for(auto& [name, sec] : sections)
    {
        for(auto& rel : sec.rel)
        {
            auto it = symbols.find(rel.symbol);
            if(it == symbols.end())
            {
                err_str = "Nepostojeci simbol u relokaciji:" + rel.symbol;
                throw runtime_error(err_str);
            }
            auto& sym = it->second;
            if(sym.is_global or sym.is_extern)
                continue;

            rel.symbol = sym.section;
            rel.addend += sym.value;
        }
    }
}

void dump(string_view filename)
{
    // open file, in binary mode
    string txtfilename = string(filename) + ".txt";
    ofstream txtfout(txtfilename);

    txtfout << "Sections:" << sections.size() << endl;

    for(auto& [name, sec] : sections)
        txtfout << name << " " << sec.data.size() << endl;

    for(auto& [name, sec] : sections)
    {
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
    
    for(auto& [name, sec] : sections)
    {
        txtfout << "." << name << ".rel" << endl;
        for(auto& rel : sec.rel)
        {
            txtfout << rel.offset << " " << rel.symbol << " " << rel.addend << endl;
        }
    }
    txtfout << "Symbols:" << symbols.size() << endl;
    for(auto& [name, sym] : symbols)
    {
        /*if(not sym.resolved and not sym.is_extern)
        {
            err_str = "Neresen simbol:" + name;
            if(sym.expr.size())
            {
                err_str += " zavisi od:";
                for(auto& [mul, sym_ptr] : sym.expr)
                    err_str += " " + sym_ptr->name;
            }
            throw runtime_error(err_str);
        }*/
        char type = 'l';
        if(sym.is_global)
            type = 'g';
        if(sym.is_extern)
            type = 'e';
        txtfout << name << " " << sym.value << " " << sym.section << " " << type << endl;
    }
    txtfout.close();

    string binfilename = string(filename);
    ofstream fout(binfilename, ios::binary);

    u32 num_sections = sections.size();
    fout.write((char*)&num_sections, sizeof(num_sections));
    // name length, name, data size, data
    for(auto& [name, sec] : sections)
    {
        u32 name_len = name.size();
        fout.write((char*)&name_len, sizeof(name_len));
        fout.write(name.c_str(), name_len);
        u32 data_size = sec.data.size();
        fout.write((char*)&data_size, sizeof(data_size));
        fout.write(sec.data.data(), data_size);

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
    u32 num_symbols = symbols.size();
    fout.write((char*)&num_symbols, sizeof(num_symbols));
    // name length, name, value, section
    for(auto& [name, sym] : symbols)
    {
        u32 name_len = name.size();
        fout.write((char*)&name_len, sizeof(name_len));
        fout.write(name.c_str(), name_len);
        fout.write((char*)&sym.value, sizeof(sym.value));
        u32 section_len = sym.section.size();
        fout.write((char*)&section_len, sizeof(section_len));
        fout.write(sym.section.c_str(), section_len);
        char type = 'l';
        if(sym.is_global)
            type = 'g';
        if(sym.is_extern)
            type = 'e';
        fout.write(&type, 1);
    }
    fout.close();
}

int main(int argc, char** argv)
{
    string out_filename = "a.out";
    string in_filename = "";
    for(int i = 1; i < argc; i++)
    {
        if(argv[i] == "-o"sv)
        {
            if(not (i+1<argc))
                throw runtime_error("-o naveden bez argumenta");
            
            out_filename = string(argv[i+1]);
            i++; continue;
        }
        in_filename = argv[i];
    }
    if(in_filename.empty())
        throw runtime_error("Nije naveden ulazni fajl");
    
    ifstream fin(in_filename);
    while(fin)
    {
        string line;
        getline(fin, line);
        parse_line(line);
        if(file_end)
            break;
    }
    fix_rel();
    dump(out_filename);
}