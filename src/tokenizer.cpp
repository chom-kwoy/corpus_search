#include "tokenizer.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

#include <fmt/core.h>
#include <fmt/ranges.h>
#include <llguidance.h>
#include <nlohmann/json.hpp>
#include <tokenizers_cpp.h>
#include <utf8.h>

namespace corpus_search {

static auto load_json_file(std::string const &path) -> nlohmann::json
{
    // Check if file exists
    if (!std::filesystem::exists(std::filesystem::path(path))) {
        throw std::runtime_error(fmt::format("File does not exist at {}", path));
    }

    // Read file
    auto file = std::ifstream(path);
    if (!file.is_open()) {
        throw std::runtime_error(fmt::format("Error opening file: {}", path));
    }

    return nlohmann::json::parse((std::stringstream{} << file.rdbuf()).str());
}

static auto replace_chars(std::string_view string,
                          std::unordered_map<char, char> const &mapping,
                          bool *was_replaced = nullptr) -> std::string
{
    bool is_replaced = false;
    auto new_string = std::string(string);
    for (int i = 0; i < string.size(); ++i) {
        if (mapping.contains(string[i])) {
            new_string[i] = mapping.at(string[i]);
            is_replaced = true;
        }
    }
    if (was_replaced) {
        *was_replaced = is_replaced;
    }
    return new_string;
}

static auto call_tokenize(const void *user_data,
                          const uint8_t *bytes,
                          size_t bytes_len,
                          uint32_t *output_tokens,
                          size_t output_tokens_len) noexcept -> std::size_t
{
    auto t = static_cast<tokenizer const *>(user_data);

    auto input = std::string(bytes, bytes + bytes_len);
    input = replace_chars(input, t->get_normalize_mapping());
    input = to_unicode(input);
    auto result = t->get_hf_tokenizer()->Encode(input);

    for (int i = 0; i < std::min(result.size(), output_tokens_len); ++i) {
        output_tokens[i] = result[i];
    }

    return result.size();
}

auto tokenizer::load_llg_tokenizer(tokenizers::Tokenizer *tok_tokenizer,
                                   nlohmann::json json) -> LlgTokenizer *
{
    // replace vocab with unnormalized tokens
    auto &vocab = json["model"]["vocab"];
    std::unordered_map<std::string, int> new_vocab;
    for (auto &[tok_str, tok_id] : vocab.items()) {
        bool is_replaced;
        auto new_tok_str = replace_chars(tok_str, inv_normalize_mapping, &is_replaced);
        if (!new_vocab.contains(new_tok_str) || is_replaced) {
            new_vocab[new_tok_str] = tok_id;
        }
    }
    vocab = new_vocab;

    // replace merges
    auto &merges = json["model"]["merges"];
    std::vector<std::vector<std::string>> new_merges = merges;
    for (auto &merge : new_merges) {
        merge.at(0) = replace_chars(merge.at(0), inv_normalize_mapping);
        merge.at(1) = replace_chars(merge.at(1), inv_normalize_mapping);
    }
    merges = new_merges;

    auto const modified_json = json.dump();

    LlgTokenizerInit tok_init = {};
    tok_init.tok_eos = tokenizer::EOS_TOKEN_ID;
    tok_init.use_approximate_greedy_tokenize_fn = false;
    tok_init.tokenize_user_data = this;
    tok_init.tokenize_fn = call_tokenize;
    tok_init.tokenizer_json = modified_json.c_str();

    char error_buf[256];
    LlgTokenizer *ll_tokenizer = llg_new_tokenizer(&tok_init, error_buf, sizeof(error_buf));
    if (ll_tokenizer == nullptr) {
        throw std::runtime_error(fmt::format("Error creating tokenizer: {}", error_buf));
    }

    return ll_tokenizer;
}

auto tokenizer::normalize(std::string_view string) const -> std::string
{
    return replace_chars(string, normalize_mapping);
}

auto tokenizer::unnormalize(std::string_view string) const -> std::string
{
    return replace_chars(string, inv_normalize_mapping);
}

tokenizer::tokenizer(std::string tokenizer_json_path,
                     std::unordered_map<char, char> normalize_mapping,
                     bool verbose)
    : normalize_mapping(std::move(normalize_mapping))
{
    for (auto [from, to] : this->normalize_mapping) {
        inv_normalize_mapping[to] = from;
    }

    auto const json = load_json_file(tokenizer_json_path);

    // Load HF Tokenizers tokenizer
    auto json_for_tok = json;
    json_for_tok["pre_tokenizer"] = nullptr; // remove unicode conversion to allow partial characters
    hf_tokenizer = tokenizers::Tokenizer::FromBlobJSON(json_for_tok.dump());
    if (verbose) {
        const char sample_input[] = "x Z X C kaxnanxho ngixta 國家";
        fmt::println("Loaded hf  tokenizer. \"{}\" -> [{}]",
                     sample_input,
                     fmt::join(tokenize(sample_input), ", "));
    }

    // Load LLG tokenizer
    ll_tokenizer = load_llg_tokenizer(hf_tokenizer.get(), json);
    if (verbose) {
        const char sample_input[] = ". / \\ ` ka.nan.ho ngi.ta 國家";
        fmt::println("Loaded llg tokenizer. \"{}\" -> [{}]",
                     sample_input,
                     fmt::join(llg_tokenize(sample_input), ", "));
    }

    // Do other preprocessing stuff
    auto vocab = json["model"]["vocab"];
    for (auto const &[tok_str, tok_id] : vocab.items()) {
        tid_to_token[tok_id.get<int>()] = to_bytes(tok_str);
    }

    max_token_bytes = 0;
    auto nchars_to_tid = std::unordered_map<int, std::vector<int>>{};
    for (auto const &[tid, token] : tid_to_token) {
        if (tid < 2) { // FIXME: special token detection
            // special token; skip
            continue;
        }
        max_token_bytes = std::max<int>(max_token_bytes, token.size());
        if ((token[0] & 0b1100'0000) == 0b1000'0000) {
            // continuation byte; skip
            continue;
        }
        auto length = utf8::utf8to32(utf8::replace_invalid(token)).size();
        nchars_to_tid[length].push_back(tid);
    }

    if (verbose) {
        fmt::println("Max token length in bytes = {}", max_token_bytes);
    }

    for (int n = 0; n <= max_token_bytes; ++n) {
        auto bitmask = boost::dynamic_bitset<>(vocab_size());
        for (int gt_n = n + 1; gt_n <= max_token_bytes; ++gt_n) {
            for (auto tid : nchars_to_tid.at(gt_n)) {
                bitmask.set(tid);
            }
        }
        gt_n_char_masks[n] = bitmask;
    }
}

tokenizer::~tokenizer()
{
    if (ll_tokenizer) {
        llg_free_tokenizer(ll_tokenizer);
    }
}

auto tokenizer::vocab_size() const -> int
{
    return hf_tokenizer->GetVocabSize();
}

auto tokenizer::max_token_length() const -> int
{
    return max_token_bytes;
}

auto tokenizer::llg_tokenize(std::string_view string) const -> std::vector<uint32_t>
{
    // figure out the size first
    auto num_tokens = llg_tokenize_bytes(ll_tokenizer,
                                         reinterpret_cast<std::uint8_t const *>(string.data()),
                                         string.size(),
                                         nullptr,
                                         0);
    auto tokens = std::vector<std::uint32_t>(num_tokens);
    llg_tokenize_bytes(ll_tokenizer,
                       reinterpret_cast<std::uint8_t const *>(string.data()),
                       string.size(),
                       tokens.data(),
                       tokens.size());
    return tokens;
}

auto tokenizer::tokenize(std::string_view string) const -> std::vector<int>
{
    return hf_tokenizer->Encode(to_unicode(string));
}

auto to_bytes(std::string_view s) -> std::string
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
    std::string result;
    for (auto it = utf8::iterator(s.begin(), s.begin(), s.end());
         it != utf8::iterator(s.end(), s.begin(), s.end());
         ++it) {
        int code_point = *it;
        if (UNICODE_TO_BYTES.contains(code_point)) {
            result.push_back(UNICODE_TO_BYTES.at(code_point));
        } else {
            result.push_back(code_point);
        }
    }
    return result;
}

auto to_unicode(std::string_view s) -> std::string
{
    static const std::unordered_map<int, int> BYTES_TO_UNICODE = {
        {0, 0x100},   {1, 0x101},   {2, 0x102},   {3, 0x103},   {4, 0x104},   {5, 0x105},
        {6, 0x106},   {7, 0x107},   {8, 0x108},   {9, 0x109},   {10, 0x10a},  {11, 0x10b},
        {12, 0x10c},  {13, 0x10d},  {14, 0x10e},  {15, 0x10f},  {16, 0x110},  {17, 0x111},
        {18, 0x112},  {19, 0x113},  {20, 0x114},  {21, 0x115},  {22, 0x116},  {23, 0x117},
        {24, 0x118},  {25, 0x119},  {26, 0x11a},  {27, 0x11b},  {28, 0x11c},  {29, 0x11d},
        {30, 0x11e},  {31, 0x11f},  {32, 0x120},  {33, 0x021},  {34, 0x022},  {35, 0x023},
        {36, 0x024},  {37, 0x025},  {38, 0x026},  {39, 0x027},  {40, 0x028},  {41, 0x029},
        {42, 0x02a},  {43, 0x02b},  {44, 0x02c},  {45, 0x02d},  {46, 0x02e},  {47, 0x02f},
        {48, 0x030},  {49, 0x031},  {50, 0x032},  {51, 0x033},  {52, 0x034},  {53, 0x035},
        {54, 0x036},  {55, 0x037},  {56, 0x038},  {57, 0x039},  {58, 0x03a},  {59, 0x03b},
        {60, 0x03c},  {61, 0x03d},  {62, 0x03e},  {63, 0x03f},  {64, 0x040},  {65, 0x041},
        {66, 0x042},  {67, 0x043},  {68, 0x044},  {69, 0x045},  {70, 0x046},  {71, 0x047},
        {72, 0x048},  {73, 0x049},  {74, 0x04a},  {75, 0x04b},  {76, 0x04c},  {77, 0x04d},
        {78, 0x04e},  {79, 0x04f},  {80, 0x050},  {81, 0x051},  {82, 0x052},  {83, 0x053},
        {84, 0x054},  {85, 0x055},  {86, 0x056},  {87, 0x057},  {88, 0x058},  {89, 0x059},
        {90, 0x05a},  {91, 0x05b},  {92, 0x05c},  {93, 0x05d},  {94, 0x05e},  {95, 0x05f},
        {96, 0x060},  {97, 0x061},  {98, 0x062},  {99, 0x063},  {100, 0x064}, {101, 0x065},
        {102, 0x066}, {103, 0x067}, {104, 0x068}, {105, 0x069}, {106, 0x06a}, {107, 0x06b},
        {108, 0x06c}, {109, 0x06d}, {110, 0x06e}, {111, 0x06f}, {112, 0x070}, {113, 0x071},
        {114, 0x072}, {115, 0x073}, {116, 0x074}, {117, 0x075}, {118, 0x076}, {119, 0x077},
        {120, 0x078}, {121, 0x079}, {122, 0x07a}, {123, 0x07b}, {124, 0x07c}, {125, 0x07d},
        {126, 0x07e}, {127, 0x121}, {128, 0x122}, {129, 0x123}, {130, 0x124}, {131, 0x125},
        {132, 0x126}, {133, 0x127}, {134, 0x128}, {135, 0x129}, {136, 0x12a}, {137, 0x12b},
        {138, 0x12c}, {139, 0x12d}, {140, 0x12e}, {141, 0x12f}, {142, 0x130}, {143, 0x131},
        {144, 0x132}, {145, 0x133}, {146, 0x134}, {147, 0x135}, {148, 0x136}, {149, 0x137},
        {150, 0x138}, {151, 0x139}, {152, 0x13a}, {153, 0x13b}, {154, 0x13c}, {155, 0x13d},
        {156, 0x13e}, {157, 0x13f}, {158, 0x140}, {159, 0x141}, {160, 0x142}, {161, 0x0a1},
        {162, 0x0a2}, {163, 0x0a3}, {164, 0x0a4}, {165, 0x0a5}, {166, 0x0a6}, {167, 0x0a7},
        {168, 0x0a8}, {169, 0x0a9}, {170, 0x0aa}, {171, 0x0ab}, {172, 0x0ac}, {173, 0x143},
        {174, 0x0ae}, {175, 0x0af}, {176, 0x0b0}, {177, 0x0b1}, {178, 0x0b2}, {179, 0x0b3},
        {180, 0x0b4}, {181, 0x0b5}, {182, 0x0b6}, {183, 0x0b7}, {184, 0x0b8}, {185, 0x0b9},
        {186, 0x0ba}, {187, 0x0bb}, {188, 0x0bc}, {189, 0x0bd}, {190, 0x0be}, {191, 0x0bf},
        {192, 0x0c0}, {193, 0x0c1}, {194, 0x0c2}, {195, 0x0c3}, {196, 0x0c4}, {197, 0x0c5},
        {198, 0x0c6}, {199, 0x0c7}, {200, 0x0c8}, {201, 0x0c9}, {202, 0x0ca}, {203, 0x0cb},
        {204, 0x0cc}, {205, 0x0cd}, {206, 0x0ce}, {207, 0x0cf}, {208, 0x0d0}, {209, 0x0d1},
        {210, 0x0d2}, {211, 0x0d3}, {212, 0x0d4}, {213, 0x0d5}, {214, 0x0d6}, {215, 0x0d7},
        {216, 0x0d8}, {217, 0x0d9}, {218, 0x0da}, {219, 0x0db}, {220, 0x0dc}, {221, 0x0dd},
        {222, 0x0de}, {223, 0x0df}, {224, 0x0e0}, {225, 0x0e1}, {226, 0x0e2}, {227, 0x0e3},
        {228, 0x0e4}, {229, 0x0e5}, {230, 0x0e6}, {231, 0x0e7}, {232, 0x0e8}, {233, 0x0e9},
        {234, 0x0ea}, {235, 0x0eb}, {236, 0x0ec}, {237, 0x0ed}, {238, 0x0ee}, {239, 0x0ef},
        {240, 0x0f0}, {241, 0x0f1}, {242, 0x0f2}, {243, 0x0f3}, {244, 0x0f4}, {245, 0x0f5},
        {246, 0x0f6}, {247, 0x0f7}, {248, 0x0f8}, {249, 0x0f9}, {250, 0x0fa}, {251, 0x0fb},
        {252, 0x0fc}, {253, 0x0fd}, {254, 0x0fe}, {255, 0x0ff},
    };
    std::u32string result;
    for (char byte : s) {
        int b = byte & 0xff;
        if (BYTES_TO_UNICODE.contains(b)) {
            result.push_back(BYTES_TO_UNICODE.at(b));
        } else {
            result.push_back(b);
        }
    }
    return utf8::utf32to8(result);
}

} // namespace corpus_search
