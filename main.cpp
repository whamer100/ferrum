#include <iostream>
#include <nlohmann/json.hpp>
#include <utility>
#include <cpr/cpr.h>
#include "vendor/miniz.h"  // https://github.com/richgel999/miniz
#include "utils.h"

#define VERSION "0.0.1a"
// likely not going to need any more than this
#define SET_CACHE_RESERVE 4

using json = nlohmann::json;
static cpr::CurlHolder holder { };

char download_anim[4] = { '|', '/', '-', '\\' };
int anim_frame = 0;

typedef struct program_state_s {
    std::filesystem::path frm_path {};
    std::string emulator {};
    std::string rom_id {};
    std::string platform_id {};
    std::string platform_roms_folder {};
    json rom_json_ctx {};
    std::string json_file {};
    std::queue<std::string> download_queue {};
    std::unordered_set<std::string> download_cache {};  // to make sure duplicate entries are not queued
    Utils::DualStreamBuf dual_stream_buf;

    explicit program_state_s(Utils::DualStreamBuf dual_stream_buf): dual_stream_buf(std::move(dual_stream_buf)) {}
} program_state;

typedef struct extract_pair_s {
    std::string src;
    std::string dst;
} extract_pair;

// slightly different from frm.py, reorganized fbneo.platforms structure to be a bit more sensical
const auto emulator_info_table = R"({
    "fbneo": {
        "roms_folder": "fbneo/ROMs",
        "platforms": {
            "md": "fbneo/ROMs/megadrive",
            "gg": "fbneo/ROMs/gamegear",
            "cv": "fbneo/ROMs/coleco",
            "msx": "fbneo/ROMs/msx",
            "sms": "fbneo/ROMs/sms",
            "nes": "fbneo/ROMs/nes",
            "pce": "fbneo/ROMs/pce",
            "sg1k": "fbneo/ROMs/sg1000",
            "tg": "fbneo/ROMs/tg16"
        }
    },
    "nulldc": {
        "roms_folder": "nulldc/nulldc-1-0-4-en-win"
    },
    "fc1": {
        "roms_folder": "ggpofba/ROMs",
        "prefix": "fc1_",
        "dont_add_prefix_to_json_file": true
    },
    "flycast": {
        "roms_folder": "flycast/ROMs"
    },
    "duckstation": {
        "roms_folder": "duckstation/ROMs"
    },
    "snes9x": {
        "roms_folder": "snes9x/ROMs"
    }
})"_json;

/*
 *  - recursively check for all required roms
 *  - dont check if roms already exist (handle that in the download queue stage)
 *  - if no roms or invalid state, leave queue empty (empty queue is error state and will be shown accordingly)
 */
void populate_queue(program_state& state, const std::string& rom) {
    if (state.rom_json_ctx.contains(rom)) {
        if (state.download_cache.contains(rom))
            return;  // we can ignore this iteration, rom already found
        state.download_cache.emplace(rom);
        state.download_queue.push(rom);
        const auto rom_ctx = state.rom_json_ctx[rom];
        if (rom_ctx.contains("require")) {
            const std::vector<std::string> required_roms = rom_ctx["require"];
            for (const auto& rrom: required_roms) {
                populate_queue(state, rrom);
            }
        }
        if (rom_ctx.contains("required")) {
            const std::vector<std::string> required_roms = rom_ctx["required"];
            for (const auto& rrom: required_roms) {
                populate_queue(state, rrom);
            }
        }
    }
    else
        std::cout << "   - Rom [" << rom << "] not found for [" << state.json_file <<"]." << std::endl;
}

mz_bool open_zip(mz_zip_archive *pZip, const std::filesystem::path& zip_path, _Out_ FILE* zip_fd, const mz_uint flags = 0) {
    auto zip_path_wc = std::filesystem::weakly_canonical(zip_path);
    const auto zip_path_normalized = zip_path_wc.make_preferred().string();
    zip_fd = fopen(zip_path_normalized.c_str(), "rb");
    fseek(zip_fd, 0, SEEK_END);
    const auto zip_sz = ftell(zip_fd);
    fseek(zip_fd, 0, SEEK_SET);
    return mz_zip_reader_init_cfile(pZip, zip_fd, zip_sz, flags);
}

mz_bool close_zip(mz_zip_archive *pZip, FILE* zip_fd) {
    const auto res = mz_zip_reader_end(pZip);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    fclose(zip_fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return res;
}

void check_zip(const std::filesystem::path& zip_file) {
    auto zip_path_wc = std::filesystem::weakly_canonical(zip_file);
    const auto zip_path = zip_path_wc.make_preferred().string();
    if (zip_path.ends_with(".zip")) {
        if (exists(zip_file)) {
            FILE* zip_fd = nullptr;
            mz_zip_archive zip_archive = { 0 };
            if (!open_zip(&zip_archive, zip_file, zip_fd)) {
                std::cout << "   - Error reading zip file [" << zip_path << "] (deleting)." << std::endl;
                close_zip(&zip_archive, zip_fd);
                remove(zip_file);
            }
            // if (!mz_zip_reader_init_cfile(&zip_archive, zip_fd, zip_sz, 0)) {
            //     std::cout << "   - Error reading zip file [" << zip_path << "] (deleting)." << std::endl;
            //     mz_zip_reader_end(&zip_archive);
            //     fclose(zip_fd);
            //     remove(zip_file);
            //     return;
            // }

            if (!mz_zip_validate_archive(&zip_archive, 0)) {
                std::cout << "   - Error validating zip file [" << zip_path << "] (deleting)." << std::endl;
                // mz_zip_reader_end(&zip_archive);
                // fclose(zip_fd);
                close_zip(&zip_archive, zip_fd);
                remove(zip_file);
                return;
            }

            // all checks passed, safely close zip
            close_zip(&zip_archive, zip_fd);
        }
    }
}

void download_file(const std::string& url, const std::string& src_file, const std::filesystem::path& dst_file) {
    if (const auto r = cpr::Head(cpr::Url{url}); r.status_code != 200) {
        std::cout << "  Error: File failed to download." << std::endl;
        return;
    }
    std::ofstream output_file(dst_file, std::ios::binary);
    const auto r = cpr::Download(output_file,
        cpr::Url{url},
        cpr::ProgressCallback([&](
            const cpr::cpr_off_t downloadTotal, const cpr::cpr_off_t downloadNow,
            cpr::cpr_off_t /*uploadTotal*/, cpr::cpr_off_t /*uploadNow*/, intptr_t /*userdata*/) -> bool
            {
            const double downloadPercentage = static_cast<double>(downloadNow) / static_cast<double>(downloadTotal) * 100;
            std::cout.precision(4);
            std::cout.fill(' ');
            std::cout << "  " << download_anim[anim_frame++ % sizeof(download_anim)] << " Downloading " << src_file << ": " << std::setw(6) << downloadPercentage << "%...      \r";
            std::cout.flush();
            return true;
        })
        );
    check_zip(dst_file);
}

void download_queue(program_state& state) {
    while (!state.download_queue.empty()) {
        const auto rom = state.download_queue.front();
        state.download_queue.pop();
        const auto rom_info = state.rom_json_ctx[rom];

        const std::string download_url = rom_info["download"];
        const auto decoded_url = holder.urlDecode(download_url);

        auto mark_position = decoded_url.find_last_of('=');
        if (mark_position == std::string::npos) {
            mark_position = decoded_url.find_last_of('/');
        }

        const std::string source_file = decoded_url.substr(mark_position + 1);
        const auto output_path = std::filesystem::path(state.frm_path) / state.platform_roms_folder;
        const auto output_file = output_path / (
            rom_info.contains("copy_to")
                ? static_cast<std::string>(rom_info["copy_to"]) : source_file
            );

        std::vector<extract_pair> extract_list;
        if (rom_info.contains("extract_to")) {
            for (const auto& it : rom_info["extract_to"]) {
                const auto inner_file = output_path / it["dst"];
                if (!std::filesystem::exists(inner_file))
                    extract_list.emplace_back(it["src"], it["dst"]);
            }
            if (extract_list.empty()) {
                std::cout << "  Files already exist." << std::endl;
                continue;
            }
        }
        else {
            check_zip(output_file);
            if (exists(output_file)) {
                std::cout << "  File " << output_file << " already exists." << std::endl;
                continue;
            }
        }
        const auto output_folder = output_file.parent_path();
        std::filesystem::create_directories(output_folder);

        state.dual_stream_buf.set_state(false);
        auto start = std::chrono::high_resolution_clock::now();
        download_file(download_url, source_file, output_file);
        auto end = std::chrono::high_resolution_clock::now();
        state.dual_stream_buf.set_state(true);
        std::chrono::duration<double> elapsed = end - start;
        std::cout << "  * File " << source_file << " downloaded in " << elapsed << ".       " << std::endl;

        if (rom_info.contains("extract_to")) {
            // miniz_cpp::zip_file zip_file(output_file.string());
            FILE* zip_fd = nullptr;
            mz_zip_archive zip_archive = { 0 };
            if (!open_zip(&zip_archive, output_file, zip_fd)) {
                std::cout << "   - Error reading zip file [" << output_file << "]." << std::endl;
                close_zip(&zip_archive, zip_fd);
                continue;
            }
            // if (!mz_zip_reader_init_file(&zip_archive, output_file.string().c_str(), 0)) {
            //     std::cout << "   - Error reading zip file [" << output_file << "]." << std::endl;
            //     mz_zip_reader_end(&zip_archive);
            //     continue;
            // }

            for (const auto& [src, dst] : extract_list) {
                auto file_index = mz_zip_reader_locate_file(&zip_archive, src.c_str(), nullptr, 0);
                if (file_index < 0) {
                    std::cout << "  - Error: File " << src << " not found in zip [" << output_file << "]" << std::endl;
                    continue;
                }

                // todo: clean this up a bit
                const auto inner_output_file = output_path / dst;
                const auto inner_output_folder = inner_output_file.parent_path();
                if (!exists(inner_output_folder))
                    std::filesystem::create_directories(inner_output_folder);
                std::string iofs = inner_output_file.string();
                std::ranges::replace(iofs, '\\', '/');
                std::cout << "  - Extracting " << src << " to " << iofs << "..." << std::endl;
                const std::string normalized_dst_path = weakly_canonical(inner_output_file).make_preferred().string();
                // std::cout << "    - " << src << ", " << normalized_dst_path << std::endl;
                // zip_file.extract(src, normalized_dst_path);
                if (!mz_zip_reader_extract_to_file(&zip_archive, file_index, normalized_dst_path.c_str(), 0)) {
                    std::cout << "  - Error: File " << src << " failed to extract from zip [" << output_file << "]" << std::endl;
                }

            }
            close_zip(&zip_archive, zip_fd);
            remove(output_file);
        }

        // std::cout << "   * " << rom << " -> " << decoded_url << std::endl;
        // std::cout << "   * " << rom << " -> " << output_path << " | " << output_file << std::endl;
        // if (rom_info.contains("extract_to"))
        //     std::cout << "    -  " << rom_info["extract_to"] << std::endl;
    }
}

int fetch_rom(program_state &state) {
    if (!emulator_info_table.contains(state.emulator)) {
        std::cout << "  Error: unknown emulator [" << state.emulator << "]" << std::endl;
        return 1;
    }
    const auto &emulator_info = emulator_info_table[state.emulator];
    state.platform_roms_folder = emulator_info["roms_folder"];
    if (emulator_info.contains("platforms")) {
        const auto &platforms = emulator_info["platforms"];
        if (state.rom_id.find_first_of('_') != std::string::npos) {
            state.platform_id = Utils::scan_to(state.rom_id, '_');
            state.rom_id = Utils::split_to(state.rom_id, '_');
            if (platforms.contains(state.platform_id))
                state.platform_roms_folder = platforms[state.platform_id];
            else {
                std::cout << "  Error: unknown platform [" << state.platform_id << "] for emulator [" << state.emulator << "]" << std::endl;
                return 1;
            }
        }
    }

    if (emulator_info.contains("dont_add_prefix_to_json_file") && emulator_info.contains("prefix")) {
        const std::string prefix = emulator_info["prefix"];
        const auto prefix_length = prefix.length();
        if (state.rom_id.starts_with(prefix))
            state.rom_id.erase(0, prefix_length); // remove prefix
    }

    const std::string target_json = state.emulator + (state.platform_id.empty() ? "" : '_' + state.platform_id) + "_roms.json";
    state.json_file = target_json;

    if (!std::filesystem::exists(target_json)) {
        std::cout << "  Error: Missing file [" << target_json << "] (Missing FC2 JSON Pack?)" << std::endl;
        return 1;
    }

    std::ifstream ifs(target_json);
    if (!ifs.is_open()) {
        std::cout << "  Error: Failed to open file [" << target_json << "]" << std::endl;
        return 1;
    }
    state.rom_json_ctx = json::parse(ifs);

    std::cout << "  Searching for required roms..." << std::endl;
    populate_queue(state, state.rom_id);

    if (state.download_queue.empty()) {
        std::cout << "  Error: No roms found with id [" << state.rom_id << "]" << std::endl;
        return 1;
    }
    std::cout << "   - Roms queued for downloading: " << state.download_queue.size() << std::endl;
    download_queue(state);

    return 0;
}

int main(const int argc, char** argv) {
    const std::ofstream out("ferrum.log");
    auto state = program_state(Utils::DualStreamBuf(std::cout.rdbuf(), out.rdbuf()));
    std::cout.rdbuf(&state.dual_stream_buf); // redirect std::cout to dual buffer

    std::cout << "\nFerrum - Fightcade Rom Manager v." << VERSION << std::endl;
    if (argc < 2) {
        std::cout << "  Error: missing arguments. Syntax: frm <emulator> <rom_id>" << std::endl;
        return 1;
    }

    state.frm_path = std::filesystem::path(argv[0]).parent_path();
    state.emulator = argv[1];
    state.rom_id = argv[2];
    state.download_cache.reserve(SET_CACHE_RESERVE);

    /* delete this later
    std::cout << std::setw(4) << json::meta() << std::endl;
    const std::string test_json_endpoint = "https://jsonplaceholder.typicode.com/todos/1";
    cpr::Response r = cpr::Get(
        cpr::Url{test_json_endpoint}
    );
    std::cout << r.status_code << '\n' << r.header["content-type"] << std::endl;
    const auto res = json::parse(r.text);
    std::cout << std::setw(4) << res << std::endl;
    */
    int res = fetch_rom(state);

    // restore original buffer
    std::cout.rdbuf(std::cout.rdbuf());

    return res;
}
