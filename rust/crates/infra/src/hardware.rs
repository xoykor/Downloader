//! Detecção de aceleração por hardware.
//!
//! A lista de `-hwaccels` do FFmpeg descreve o que o binário conhece, mas não
//! prova que há um dispositivo utilizável. Este módulo separa as duas coisas
//! e executa uma sondagem curta antes de a aplicação oferecer um backend.

use crate::process::{ProcessRunner, ProcessSpec};
use std::ffi::OsString;
use std::time::Duration;
use tokio_util::sync::CancellationToken;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum HardwareBackend {
    Vulkan,
    Vaapi,
    Amf,
    Cuda,
    Qsv,
    Vdpau,
    Drm,
    Opencl,
}

impl HardwareBackend {
    pub const ALL: [Self; 8] = [
        Self::Vulkan,
        Self::Vaapi,
        Self::Amf,
        Self::Cuda,
        Self::Qsv,
        Self::Vdpau,
        Self::Drm,
        Self::Opencl,
    ];

    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Vulkan => "vulkan",
            Self::Vaapi => "vaapi",
            Self::Amf => "amf",
            Self::Cuda => "cuda",
            Self::Qsv => "qsv",
            Self::Vdpau => "vdpau",
            Self::Drm => "drm",
            Self::Opencl => "opencl",
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct HardwareProbe {
    pub backend: HardwareBackend,
    /// Backend listado pelo FFmpeg, mesmo que não tenha passado na sondagem.
    pub advertised: bool,
    /// Dispositivo/driver respondeu à sondagem curta.
    pub usable: bool,
    pub detail: Option<String>,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct HardwareCapabilities {
    pub advertised: Vec<HardwareBackend>,
    pub usable: Vec<HardwareBackend>,
    pub probes: Vec<HardwareProbe>,
}

impl HardwareCapabilities {
    pub fn supports(&self, backend: HardwareBackend) -> bool {
        self.usable.contains(&backend)
    }
}

/// Converte a saída de `ffmpeg -hwaccels` em backends conhecidos.
pub fn parse_hwaccels(output: &str) -> Vec<HardwareBackend> {
    let mut found = Vec::new();
    for line in output.lines().map(str::trim) {
        let Some(backend) = HardwareBackend::ALL
            .into_iter()
            .find(|candidate| candidate.as_str() == line)
        else {
            continue;
        };
        if !found.contains(&backend) {
            found.push(backend);
        }
    }
    found
}

/// Sonda um backend com uma operação mínima do FFmpeg.
///
/// A saída não é publicada nem usa arquivos do usuário. Para Vulkan, por
/// exemplo, `vulkan=list` falha com `VK_ERROR_INITIALIZATION_FAILED` quando o
/// processo não enxerga nenhum dispositivo, que é exatamente o caso a ser
/// refletido na UI.
pub async fn probe_backend<R: ProcessRunner + ?Sized>(
    runner: &R,
    ffmpeg: OsString,
    backend: HardwareBackend,
) -> HardwareProbe {
    let spec = ProcessSpec {
        executable: ffmpeg,
        args: vec![
            OsString::from("-hide_banner"),
            OsString::from("-init_hw_device"),
            OsString::from(format!("{}=list", backend.as_str())),
            OsString::from("-f"),
            OsString::from("lavfi"),
            OsString::from("-i"),
            OsString::from("nullsrc"),
            OsString::from("-frames:v"),
            OsString::from("1"),
            OsString::from("-f"),
            OsString::from("null"),
            OsString::from("-"),
        ],
        timeout: Some(Duration::from_secs(3)),
        ..Default::default()
    };
    match runner.run(spec, CancellationToken::new()).await {
        Ok(result) => {
            let detail = if result.stderr.is_empty() {
                None
            } else {
                Some(
                    String::from_utf8_lossy(&result.stderr)
                        .lines()
                        .last()
                        .unwrap_or_default()
                        .trim()
                        .to_owned(),
                )
            };
            HardwareProbe {
                backend,
                advertised: true,
                usable: result.code == Some(0) && !result.timed_out && !result.cancelled,
                detail,
            }
        }
        Err(error) => HardwareProbe {
            backend,
            advertised: true,
            usable: false,
            detail: Some(error.to_string()),
        },
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::process::TokioProcessRunner;

    #[test]
    fn parses_only_known_acceleration_backends() {
        let parsed =
            parse_hwaccels("Hardware acceleration methods:\nvulkan\nvaapi\nunknown\nvulkan\n");
        assert_eq!(
            parsed,
            vec![HardwareBackend::Vulkan, HardwareBackend::Vaapi]
        );
    }

    #[tokio::test]
    async fn probe_reports_missing_vulkan_device_without_claiming_support() {
        let probe = probe_backend(
            &TokioProcessRunner,
            OsString::from("ffmpeg"),
            HardwareBackend::Vulkan,
        )
        .await;
        // Em ambientes sem /dev/dri/Vulkan, a sondagem deve falhar de forma
        // explícita; nunca transformar o backend anunciado em "usável".
        if probe.detail.as_deref().is_some_and(|detail| {
            detail.contains("No devices")
                || detail.contains("No such device")
                || detail.contains("INITIALIZATION_FAILED")
        }) {
            assert!(!probe.usable);
        }
    }
}
