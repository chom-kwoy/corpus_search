#include "utils.h"

#include "searcher.h"

#include "utf8.h"
#include <fmt/core.h>
#include <fstream>
#include <iostream>

auto tokenize(LlgTokenizer *tokenizer, std::string const &string) -> std::vector<std::uint32_t>
{
    auto sample_input_bytes = std::vector<std::uint8_t>(std::begin(string), std::end(string));
    auto tokens = std::vector<std::uint32_t>(1024);
    auto num_tokens = llg_tokenize_bytes(tokenizer,
                                         sample_input_bytes.data(),
                                         sample_input_bytes.size(),
                                         tokens.data(),
                                         tokens.size());
    tokens.resize(num_tokens);
    return tokens;
}

auto make_index(std::unordered_map<int, std::vector<int>> sentences)
    -> std::unordered_map<int, std::vector<IndexEntry>>
{
    auto index = std::unordered_map<int, std::vector<IndexEntry>>{};
    for (auto const &[sent_id, sentence] : sentences) {
        int pos = 0;
        for (int token : sentence) {
            index[token].push_back(
                {static_cast<unsigned int>(sent_id), static_cast<unsigned int>(pos)});
            pos += 1;
        }
    }
    return index;
}

auto to_bytes(std::string s) -> std::string
{
    static const std::unordered_map<int, int> UNICODE_TO_BYTES = {
        {0x100, 0},   {0x101, 1},   {0x102, 2},   {0x103, 3},   {0x104, 4},   {0x105, 5},
        {0x106, 6},   {0x107, 7},   {0x108, 8},   {0x109, 9},   {0x10a, 10},  {0x10b, 11},
        {0x10c, 12},  {0x10d, 13},  {0x10e, 14},  {0x10f, 15},  {0x110, 16},  {0x111, 17},
        {0x112, 18},  {0x113, 19},  {0x114, 20},  {0x115, 21},  {0x116, 22},  {0x117, 23},
        {0x118, 24},  {0x119, 25},  {0x11a, 26},  {0x11b, 27},  {0x11c, 28},  {0x11d, 29},
        {0x11e, 30},  {0x11f, 31},  {0x120, 32},  {0x021, 33},  {0x022, 34},  {0x023, 35},
        {0x024, 36},  {0x025, 37},  {0x026, 38},  {0x027, 39},  {0x028, 40},  {0x029, 41},
        {0x02a, 42},  {0x02b, 43},  {0x02c, 44},  {0x02d, 45},  {0x02e, 46},  {0x02f, 47},
        {0x030, 48},  {0x031, 49},  {0x032, 50},  {0x033, 51},  {0x034, 52},  {0x035, 53},
        {0x036, 54},  {0x037, 55},  {0x038, 56},  {0x039, 57},  {0x03a, 58},  {0x03b, 59},
        {0x03c, 60},  {0x03d, 61},  {0x03e, 62},  {0x03f, 63},  {0x040, 64},  {0x041, 65},
        {0x042, 66},  {0x043, 67},  {0x044, 68},  {0x045, 69},  {0x046, 70},  {0x047, 71},
        {0x048, 72},  {0x049, 73},  {0x04a, 74},  {0x04b, 75},  {0x04c, 76},  {0x04d, 77},
        {0x04e, 78},  {0x04f, 79},  {0x050, 80},  {0x051, 81},  {0x052, 82},  {0x053, 83},
        {0x054, 84},  {0x055, 85},  {0x056, 86},  {0x057, 87},  {0x058, 88},  {0x059, 89},
        {0x05a, 90},  {0x05b, 91},  {0x05c, 92},  {0x05d, 93},  {0x05e, 94},  {0x05f, 95},
        {0x060, 96},  {0x061, 97},  {0x062, 98},  {0x063, 99},  {0x064, 100}, {0x065, 101},
        {0x066, 102}, {0x067, 103}, {0x068, 104}, {0x069, 105}, {0x06a, 106}, {0x06b, 107},
        {0x06c, 108}, {0x06d, 109}, {0x06e, 110}, {0x06f, 111}, {0x070, 112}, {0x071, 113},
        {0x072, 114}, {0x073, 115}, {0x074, 116}, {0x075, 117}, {0x076, 118}, {0x077, 119},
        {0x078, 120}, {0x079, 121}, {0x07a, 122}, {0x07b, 123}, {0x07c, 124}, {0x07d, 125},
        {0x07e, 126}, {0x121, 127}, {0x122, 128}, {0x123, 129}, {0x124, 130}, {0x125, 131},
        {0x126, 132}, {0x127, 133}, {0x128, 134}, {0x129, 135}, {0x12a, 136}, {0x12b, 137},
        {0x12c, 138}, {0x12d, 139}, {0x12e, 140}, {0x12f, 141}, {0x130, 142}, {0x131, 143},
        {0x132, 144}, {0x133, 145}, {0x134, 146}, {0x135, 147}, {0x136, 148}, {0x137, 149},
        {0x138, 150}, {0x139, 151}, {0x13a, 152}, {0x13b, 153}, {0x13c, 154}, {0x13d, 155},
        {0x13e, 156}, {0x13f, 157}, {0x140, 158}, {0x141, 159}, {0x142, 160}, {0x0a1, 161},
        {0x0a2, 162}, {0x0a3, 163}, {0x0a4, 164}, {0x0a5, 165}, {0x0a6, 166}, {0x0a7, 167},
        {0x0a8, 168}, {0x0a9, 169}, {0x0aa, 170}, {0x0ab, 171}, {0x0ac, 172}, {0x143, 173},
        {0x0ae, 174}, {0x0af, 175}, {0x0b0, 176}, {0x0b1, 177}, {0x0b2, 178}, {0x0b3, 179},
        {0x0b4, 180}, {0x0b5, 181}, {0x0b6, 182}, {0x0b7, 183}, {0x0b8, 184}, {0x0b9, 185},
        {0x0ba, 186}, {0x0bb, 187}, {0x0bc, 188}, {0x0bd, 189}, {0x0be, 190}, {0x0bf, 191},
        {0x0c0, 192}, {0x0c1, 193}, {0x0c2, 194}, {0x0c3, 195}, {0x0c4, 196}, {0x0c5, 197},
        {0x0c6, 198}, {0x0c7, 199}, {0x0c8, 200}, {0x0c9, 201}, {0x0ca, 202}, {0x0cb, 203},
        {0x0cc, 204}, {0x0cd, 205}, {0x0ce, 206}, {0x0cf, 207}, {0x0d0, 208}, {0x0d1, 209},
        {0x0d2, 210}, {0x0d3, 211}, {0x0d4, 212}, {0x0d5, 213}, {0x0d6, 214}, {0x0d7, 215},
        {0x0d8, 216}, {0x0d9, 217}, {0x0da, 218}, {0x0db, 219}, {0x0dc, 220}, {0x0dd, 221},
        {0x0de, 222}, {0x0df, 223}, {0x0e0, 224}, {0x0e1, 225}, {0x0e2, 226}, {0x0e3, 227},
        {0x0e4, 228}, {0x0e5, 229}, {0x0e6, 230}, {0x0e7, 231}, {0x0e8, 232}, {0x0e9, 233},
        {0x0ea, 234}, {0x0eb, 235}, {0x0ec, 236}, {0x0ed, 237}, {0x0ee, 238}, {0x0ef, 239},
        {0x0f0, 240}, {0x0f1, 241}, {0x0f2, 242}, {0x0f3, 243}, {0x0f4, 244}, {0x0f5, 245},
        {0x0f6, 246}, {0x0f7, 247}, {0x0f8, 248}, {0x0f9, 249}, {0x0fa, 250}, {0x0fb, 251},
        {0x0fc, 252}, {0x0fd, 253}, {0x0fe, 254}, {0x0ff, 255},
    };
    std::vector<int> result;
    for (auto it = utf8::iterator(s.begin(), s.begin(), s.end());
         it != utf8::iterator(s.end(), s.begin(), s.end());
         ++it) {
        int code_point = *it;
        result.push_back(UNICODE_TO_BYTES.at(code_point));
    }
    return std::string(result.begin(), result.end());
}
