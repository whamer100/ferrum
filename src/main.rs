use std::path::{PathBuf};
use serde_json::{json, Value};
use std::collections::{VecDeque};
use std::env;
use std::io::{Read, Write};
use std::time::{Instant, SystemTime};
use fomat_macros::{fomat};
use lazy_static::lazy_static;
use tokio_stream::StreamExt;
use const_format::{formatcp};
use reqwest::header::{HeaderMap, USER_AGENT};
use tokio::io::{AsyncWriteExt};

const VERSION: &'static str = env!("CARGO_PKG_VERSION");
const DL_ANIM_RATE: f64 = 1. / 10.;  // update every 0.1 seconds
const FERRUM_USER_AGENT: &'static str = formatcp!("Ferrum/{} (https://github.com/whamer100/ferrum)", VERSION);

lazy_static! {
    static ref CLIENT: reqwest::Client = reqwest::Client::new();
    static ref DOWNLOAD_ANIM: Vec<char> = vec!['|', '/', '-', '\\'];
    static ref EMULATOR_INFO_TABLE: Value = json!({
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
});
}

#[derive(Default, Debug)]
struct ProgramState {
    frm_path: PathBuf,
    emulator: String,
    rom_id: String,
    platform_id: String,
    platform_roms_folder: String,
    rom_json_ctx: Value,
    json_file: String,
    download_queue: VecDeque<String>
}

#[derive(Debug)]
struct ExtractPair {
    src: String,
    dst: String
}

impl ExtractPair {
    fn new(src: String, dst: String) -> Self {
        Self { src, dst }
    }
}

fn populate_queue(state: &mut ProgramState) {
    if let Some(rom_ctx) = state.rom_json_ctx.get(&state.rom_id) {
        state.download_queue.push_front(state.rom_id.to_string());
        if let Some(rroms) = rom_ctx.get("require") {
            if let Some(required_roms) = rroms.as_array() {
                for rrom in required_roms {
                    state.download_queue.push_back(rrom.as_str().unwrap().to_string());
                }
            }
        }
        // cursed edge case moment
        else if let Some(rroms) = rom_ctx.get("required") {
            if let Some(required_roms) = rroms.as_array() {
                for rrom in required_roms {
                    state.download_queue.push_back(rrom.as_str().unwrap().to_string());
                }
            }
        }
    } else {
        println!("   - Rom [{}] not found for [{}].", state.rom_id, state.json_file);
    }
}

async fn check_zip(zip_file: &PathBuf) {
    if zip_file.ends_with(".zip") {
        if tokio::fs::try_exists(&zip_file).await.unwrap() {
            let mut valid = true;
            {
                let file = std::fs::File::open(zip_file).unwrap();
                let reader = std::io::BufReader::new(file);
                let mut archive = zip::ZipArchive::new(reader).unwrap();
                for i in 0..archive.len() {
                    let file = archive.by_index(i).unwrap();
                    let _outpath = match file.enclosed_name() {
                        Some(path) => path,
                        None => {
                            println!("   - Error reading zip file [{}] (deleting).", zip_file.display());
                            valid = false;
                            break;
                        }
                    };
                }
            }
            if !valid {
                std::fs::remove_file(zip_file).unwrap();
            }
        }
    }
}

async fn download_file(url: &str, source_file: &str, output_file: &PathBuf) {
    let mut headers = HeaderMap::new();
    headers.insert(USER_AGENT, FERRUM_USER_AGENT.parse().unwrap());
    // check if the file even exists before attempting to download the file
    let r = CLIENT.head(url).headers(headers.to_owned()).send().await.unwrap();
    if r.status() != 200 {
        println!("  Error: File failed to download.");
        return
    }
    // actually download the file lmao
    let r = CLIENT.get(url).headers(headers).send().await.unwrap();
    let file_sz = r.content_length().unwrap();
    let mut file = tokio::fs::File::create(output_file).await.unwrap();
    let mut file_progress: u64 = 0;
    let mut stream = r.bytes_stream();
    let mut anim_frame = 0;
    let mut timer = Instant::now();
    let mut elapsed: f64 = 0.;
    while let Some(chunk_res) = stream.next().await {
        let chunk = chunk_res.unwrap();
        file.write(&chunk).await.unwrap();
        file_progress += chunk.len() as u64;
        let now = Instant::now();
        elapsed += now.duration_since(timer).as_secs_f64();
        timer = Instant::now();
        if elapsed > DL_ANIM_RATE {
            elapsed -= DL_ANIM_RATE;
            let download_percentage: f64 = (file_progress as f64 / file_sz as f64) * 100f64;
            print!("  {} Downloading {source_file}: {download_percentage:>8.4}%...      \r", DOWNLOAD_ANIM[anim_frame]);
            std::io::stdout().flush().unwrap();
            anim_frame = (anim_frame + 1) % DOWNLOAD_ANIM.len();
        }
        // let download_percentage: f64 = (file_progress as f64 / file_sz as f64) * 100f64;
        // print!("  {} Downloading {source_file}: {download_percentage:>8.4}%... [{elapsed}]      \r", DOWNLOAD_ANIM[anim_frame]);
        // anim_frame = (anim_frame + 1) % DOWNLOAD_ANIM.len();
    }

}

async fn download_queue(state: &mut ProgramState) {
    while !state.download_queue.is_empty() {
        let rom = state.download_queue.pop_front().unwrap();
        let rom_info = &state.rom_json_ctx[rom];

        let download_url = rom_info["download"].as_str().unwrap();
        let decoded_url = urlencoding::decode(download_url).unwrap();

        let mut mark_position = decoded_url.rfind('=');
        if mark_position.is_none() {
            mark_position = decoded_url.rfind('/');
        }

        let source_file = &decoded_url[(mark_position.unwrap()+1)..];
        let output_path = state.frm_path.join(&state.platform_roms_folder);
        let output_file = output_path.join(
            if let Some(copy_to) = rom_info.get("copy_to") {
                copy_to.as_str().unwrap()
            } else {
                source_file
            }
        );
        let mut extract_list = Vec::<ExtractPair>::new();
        if let Some(extract_to) = rom_info.get("extract_to") {
            for it in extract_to.as_array().unwrap() {
                let dst = it["dst"].as_str().unwrap().to_string();
                let inner_file = output_path.join(&dst);
                if !tokio::fs::try_exists(inner_file).await.unwrap() {
                    let e = ExtractPair::new(
                        it["src"].as_str().unwrap().to_string(),
                        dst,
                    );
                    extract_list.push(e);
                }
                if extract_list.is_empty() {
                    println!("  Files already exist.");
                    continue;
                }
            }
        } else {
            check_zip(&output_file).await;
            if tokio::fs::try_exists(&output_file).await.unwrap() {
                println!("  File {} already exists.", output_file.display());
                continue
            }
        }

        let output_folder = output_file.parent().unwrap();
        tokio::fs::create_dir_all(output_folder).await.unwrap();
        let start = SystemTime::now();
        download_file(&download_url, &source_file, &output_file).await;
        let end = SystemTime::now();
        match end.duration_since(start) {
            Ok(elapsed) => {
                println!("  * File {source_file} downloaded in {:.2}s.       ", elapsed.as_secs_f64());
            }
            Err(e) => {
                println!("  - Error [{e:?}] when using SystemTime for some reason!");
            }
        }

        if let Some(_extract_to) = rom_info.get("extract_to") {
            {  // zip file context
                let file = std::fs::File::open(&output_file).unwrap();
                let reader = std::io::BufReader::new(file);
                let mut archive = zip::ZipArchive::new(reader).unwrap();

                for pair in &extract_list {
                    let index_opt = archive.index_for_name(&pair.src.as_str());
                    if index_opt.is_none() {
                        println!("  - Error: File {} not found in zip [{}]", pair.src, output_file.display());
                        // warning for the end user to go complain to the JSON repo devs to fix
                        // ill try and make some kind of fix later that guesses for the correct file name
                        println!("    - ExtractList in the JSON data is likely malformed. This is not a fault of this downloader.");
                        continue
                    }
                    let index = index_opt.unwrap();
                    // println!("{index} -> {}", pair.src);

                    let inner_output_file = output_path.join(&pair.dst);
                    let inner_output_folder = inner_output_file.parent().unwrap();
                    if !tokio::fs::try_exists(inner_output_folder).await.unwrap() {
                        tokio::fs::create_dir_all(inner_output_folder).await.unwrap();
                    }
                    println!("  - Extracting {} to {}...", &pair.src, inner_output_file.display());
                    let mut zip_file = archive.by_index(index).unwrap();
                    let mut out_file = std::fs::File::create(inner_output_file).unwrap();
                    let res = std::io::copy(&mut zip_file, &mut out_file);
                    if res.is_err() {
                        println!("  - Error: File {} failed to extract from zip [{}]", &pair.src, output_file.display());
                    }
                }
            }
            // out of zip context, safe to delete file now
            std::fs::remove_file(output_file).unwrap()
        }


        // println!("{extract_list:?}");
        // println!("{source_file:?} {output_path:?} {output_file:?}");
        // println!("{:?} {source_file} {output_path:?}", rom_info);
    }
}

async fn fetch_rom(state: &mut ProgramState) {
    let emulator_info_opt = EMULATOR_INFO_TABLE.get(&state.emulator);
    if emulator_info_opt.is_none() {
        println!("  Error: unknown emulator [{}]", &state.emulator);
        return;
    }
    let emulator_info = emulator_info_opt.unwrap();
    state.platform_roms_folder = emulator_info["roms_folder"].as_str().unwrap().to_string();
    if let Some(platforms) = emulator_info.get("platforms") {
        if let Some(index) = state.rom_id.find('_') {
            state.platform_id = state.rom_id[0..index].to_string();
            state.rom_id = state.rom_id[(index+1)..].to_string();
            if let Some(platform) = platforms.get(&state.platform_id) {
                state.platform_roms_folder = platform.as_str().unwrap().to_string();
            } else {
                println!("  Error: unknown platform [{}] for emulator [{}]", &state.platform_id, &state.emulator);
                return;
            }
        }
    }

    if let Some(_) = emulator_info.get("dont_add_prefix_to_json_file") {
        if let Some(prefix) = emulator_info.get("prefix") {
            let prefix_str = prefix.as_str().unwrap();
            if state.rom_id.starts_with(prefix_str) {
                state.rom_id = state.rom_id[prefix_str.len()..].to_string();
            }
        }
    }

    state.json_file = fomat!(
        (&state.emulator)
        if (!state.platform_id.is_empty()) { '_' (&state.platform_id) }
        "_roms.json"
    );

    let check = tokio::fs::try_exists(&state.json_file).await;
    if !check.unwrap_or(false) {
        println!("  Error: Missing file [{}] (Missing FC2 JSON Pack?)", state.json_file);
        return;
    }

    let json_str = tokio::fs::read_to_string(&state.json_file).await;
    if json_str.is_ok() {
        state.rom_json_ctx = serde_json::from_str(json_str.unwrap().as_str()).unwrap();

    } else {
        println!("  Error: Failed to open file [{}]", state.json_file);
        return;
    }

    println!("  Searching for required roms...");
    populate_queue(state);
    download_queue(state).await;
    // println!("{:?}", state);
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<_> = env::args().collect();
    let mut state = ProgramState::default();

    println!("\nFerrum - Fightcade Rom Manager v.{VERSION}");
    if args.len() < 2 {
        println!("  Error: missing arguments. Syntax: frm <emulator> <rom_id>");
        return Ok(());
    }


    state.frm_path = std::env::current_exe()?.parent().unwrap().to_path_buf();
    state.emulator = args[1].to_owned();
    state.rom_id = args[2].to_owned();

    // let client = reqwest::Client::new();
    //
    // let resp = client.get("https://httpbin.org/ip").send().await?;
    // let test: Value = resp.json().await?;
    // println!("{}", test["origin"]);

    fetch_rom(&mut state).await;

    {  // block for letting the end user read the logs (at least until i get it to log to disk)
        let mut stdin = std::io::stdin();
        let mut stdout = std::io::stdout();

        write!(stdout, "Press any key to continue...").unwrap();
        stdout.flush().unwrap();

        let _ = stdin.read(&mut [0u8]).unwrap();
    }

    Ok(())
}
