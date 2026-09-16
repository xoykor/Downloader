//! Local identities are trusted only while the corresponding media still exists.
use crate::EngineConfig;
use downloader_infra::{ProcessRunner, ProcessSpec, TokioProcessRunner};
use serde_json::{json, Value};
use std::{
    collections::BTreeMap,
    path::{Path, PathBuf},
    time::{Duration, UNIX_EPOCH},
};
use tokio_util::sync::CancellationToken;

const INDEX: &str = ".downloader-library.json";

fn fingerprint(path: &Path) -> Option<Value> {
    let m = path.metadata().ok()?;
    if !m.is_file() || m.len() == 0 {
        return None;
    }
    Some(json!([
        m.len(),
        m.modified()
            .ok()?
            .duration_since(UNIX_EPOCH)
            .ok()?
            .as_nanos()
            .to_string()
    ]))
}
fn read_index(dir: &Path) -> Value {
    std::fs::read(dir.join(INDEX))
        .ok()
        .and_then(|b| serde_json::from_slice(&b).ok())
        .filter(Value::is_object)
        .unwrap_or_else(|| json!({}))
}
pub(crate) fn remember(dir: &Path, path: &Path, info: &Value) -> std::io::Result<()> {
    let Some(identity) = identity(info) else {
        return Ok(());
    };
    let Some(name) = path.file_name().and_then(|n| n.to_str()) else {
        return Ok(());
    };
    let mut index = read_index(dir);
    index[name] = json!({"identity":identity, "fingerprint":fingerprint(path)});
    let temporary = dir.join(".downloader-library.json.tmp");
    std::fs::write(&temporary, serde_json::to_vec(&index)?)?;
    std::fs::rename(temporary, dir.join(INDEX))
}
pub(crate) fn matching_published(
    dir: &Path,
    info: &Value,
    extension: &std::ffi::OsStr,
) -> Option<PathBuf> {
    let id = identity(info)?;
    let index = read_index(dir);
    for (name, entry) in index.as_object()? {
        if Path::new(name).file_name()?.to_str()? != name {
            continue;
        }
        let path = dir.join(name);
        if path.extension() == Some(extension)
            && entry["identity"].as_str() == Some(&id)
            && fingerprint(&path).is_some_and(|mark| mark == entry["fingerprint"])
        {
            return Some(path);
        }
    }
    None
}
fn identity(info: &Value) -> Option<String> {
    let id = info["id"].as_str()?;
    let extractor = info["extractor_key"]
        .as_str()
        .or_else(|| info["extractor"].as_str())?;
    if id.is_empty()
        || !id
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || "_-".contains(c))
        || !extractor
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || "_-:".contains(c))
    {
        return None;
    }
    Some(format!("{} {id}", extractor.to_ascii_lowercase()))
}
fn youtube_id(url: &str) -> Option<String> {
    let rest = url
        .strip_prefix("https://")
        .or_else(|| url.strip_prefix("http://"))?;
    let (host, path) = rest.split_once('/')?;
    let id = match host {
        "youtu.be" => path.split(['?', '#']).next()?,
        "youtube.com" | "www.youtube.com" | "music.youtube.com" | "m.youtube.com" => {
            if let Some(id) = path
                .strip_prefix("shorts/")
                .or_else(|| path.strip_prefix("embed/"))
            {
                id.split(['?', '#', '/']).next()?
            } else {
                path.split_once('?')?
                    .1
                    .split('&')
                    .find_map(|p| p.strip_prefix("v="))?
                    .split('#')
                    .next()?
            }
        }
        _ => return None,
    };
    (id.len() == 11
        && id
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || "_-".contains(c)))
    .then(|| format!("youtube {id}"))
}

pub(crate) async fn existing(
    dir: &Path,
    format: &str,
    mode: &str,
    config: &EngineConfig,
    cancel: CancellationToken,
) -> BTreeMap<String, PathBuf> {
    let index = read_index(dir);
    let mut found = BTreeMap::new();
    let Ok(entries) = std::fs::read_dir(dir) else {
        return found;
    };
    for entry in entries.flatten() {
        if cancel.is_cancelled() {
            break;
        }
        let path = entry.path();
        let ext = path
            .extension()
            .and_then(|v| v.to_str())
            .unwrap_or("")
            .to_ascii_lowercase();
        if !matches!(
            ext.as_str(),
            "opus" | "mp3" | "m4a" | "flac" | "wav" | "ogg" | "mp4" | "mkv" | "webm"
        ) || (format != "auto" && format != ext)
        {
            continue;
        }
        let Some(mark) = fingerprint(&path) else {
            continue;
        };
        let name = entry.file_name().to_string_lossy().into_owned();
        // Probe streams as well as file size: partial and damaged files must not suppress downloads.
        let Ok(probe) = TokioProcessRunner
            .run(
                ProcessSpec {
                    executable: config.ffprobe.clone(),
                    args: vec![
                        "-v".into(),
                        "error".into(),
                        "-show_streams".into(),
                        "-show_format".into(),
                        "-of".into(),
                        "json".into(),
                        path.as_os_str().into(),
                    ],
                    timeout: Some(Duration::from_secs(5)),
                    current_dir: None,
                },
                cancel.clone(),
            )
            .await
        else {
            continue;
        };
        if probe.code != Some(0) {
            continue;
        }
        let Ok(data) = serde_json::from_slice::<Value>(&probe.stdout) else {
            continue;
        };
        let Some(streams) = data["streams"].as_array() else {
            continue;
        };
        let audio = streams.iter().any(|s| s["codec_type"] == "audio");
        let video = streams
            .iter()
            .any(|s| s["codec_type"] == "video" && s["disposition"]["attached_pic"] != 1);
        let audio_output =
            matches!(format, "opus" | "mp3" | "m4a" | "flac" | "wav" | "ogg") || mode == "audio";
        if (audio_output && !audio)
            || (!audio_output && !video)
            || (!audio_output && mode == "video_audio" && !audio)
            || (format == "opus" && !streams.iter().any(|s| s["codec_name"] == "opus"))
        {
            continue;
        }
        let cached = &index[&name];
        let id = if cached["fingerprint"] == mark {
            cached["identity"].as_str().map(str::to_owned)
        } else {
            None
        };
        let id = id.or_else(|| {
            streams
                .iter()
                .map(|s| &s["tags"])
                .chain(std::iter::once(&data["format"]["tags"]))
                .find_map(|tags| {
                    tags.as_object()?
                        .iter()
                        .filter(|(k, _)| {
                            matches!(
                                k.to_ascii_lowercase().as_str(),
                                "purl" | "comment" | "webpage_url"
                            )
                        })
                        .find_map(|(_, v)| youtube_id(v.as_str()?))
                })
        });
        if let Some(id) = id {
            found.insert(id, path);
        }
    }
    found
}

#[cfg(test)]
mod tests {
    use super::*;
    #[tokio::test]
    async fn scan_respects_format_deleted_files_corruption_and_legacy_metadata() {
        let dir = tempfile::tempdir().unwrap();
        let audio = dir.path().join("Song.opus");
        assert!(std::process::Command::new("ffmpeg")
            .args([
                "-v",
                "error",
                "-f",
                "lavfi",
                "-i",
                "sine=duration=0.2",
                "-metadata",
                "purl=https://www.youtube.com/watch?v=eZuDklxsaRg",
                "-c:a",
                "libopus"
            ])
            .arg(&audio)
            .status()
            .unwrap()
            .success());
        let config = EngineConfig::default();
        let scan = existing(
            dir.path(),
            "opus",
            "audio",
            &config,
            CancellationToken::new(),
        )
        .await;
        assert_eq!(scan.get("youtube eZuDklxsaRg"), Some(&audio));
        assert!(existing(
            dir.path(),
            "mp3",
            "audio",
            &config,
            CancellationToken::new()
        )
        .await
        .is_empty());
        let info = json!({"id":"example", "extractor_key":"Generic"});
        remember(dir.path(), &audio, &info).unwrap();
        assert_eq!(
            matching_published(dir.path(), &info, std::ffi::OsStr::new("opus")),
            Some(audio.clone())
        );
        assert!(existing(
            dir.path(),
            "opus",
            "audio",
            &config,
            CancellationToken::new()
        )
        .await
        .contains_key("generic example"));
        std::fs::write(&audio, b"invalid audio").unwrap();
        assert!(existing(
            dir.path(),
            "opus",
            "audio",
            &config,
            CancellationToken::new()
        )
        .await
        .is_empty());
        std::fs::remove_file(&audio).unwrap();
        assert!(existing(
            dir.path(),
            "opus",
            "audio",
            &config,
            CancellationToken::new()
        )
        .await
        .is_empty());
        assert!(matching_published(dir.path(), &info, std::ffi::OsStr::new("opus")).is_none());
    }

    #[test]
    fn identity_uses_exact_site_and_video_id() {
        assert_eq!(
            youtube_id("https://www.youtube.com/watch?v=eZuDklxsaRg&list=abc"),
            Some("youtube eZuDklxsaRg".into())
        );
        assert_eq!(
            youtube_id("https://youtube.com.evil.test/watch?v=eZuDklxsaRg"),
            None
        );
        assert_eq!(
            youtube_id("https://youtu.be/eZuDklxsaRg"),
            Some("youtube eZuDklxsaRg".into())
        );
    }
}
