use crate::profiles::RuntimeConfig;
use serde::Serialize;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

const SOFTWARE_GPU_MARKERS: &[&str] = &["llvmpipe", "lavapipe", "swiftshader", "softpipe"];
const HARDWARE_DEVICE_TYPES: &[&str] = &[
    "PHYSICAL_DEVICE_TYPE_DISCRETE_GPU",
    "PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU",
    "PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU",
];

pub const ANTICHEAT_EULA_NOTICE: &str =
    "Uyarı: Çekirdek seviyesi anti-cheat atlatma, bellek/ikili müdahalesi veya oyun EULA/kullanım koşulları ihlali kesinlikle yasaktır. Yalnızca Linux/Proton üzerinde resmi olarak desteklenen oyun ve anti-cheat çalışma zamanları kullanılmalıdır.";

#[derive(Debug, Clone, Serialize)]
pub struct VulkanDeviceInfo {
    pub name: String,
    pub device_type: String,
    pub api_version: String,
    pub driver_name: String,
    pub driver_info: String,
    pub vendor_id: String,
    pub device_id: String,
    pub is_hardware: bool,
    pub meets_vulkan_13: bool,
}

#[derive(Debug, Clone, Serialize)]
pub struct VulkanSummary {
    pub available: bool,
    pub hardware_vulkan_13: bool,
    pub devices: Vec<VulkanDeviceInfo>,
    pub reason: String,
}

#[derive(Debug, Clone, Serialize)]
pub struct GpuCapabilityMatrix {
    pub matched_profile: String,
    pub tier: String,
    pub tier_label: String,
    pub gpu_name: String,
    pub driver_name: String,
    pub driver_info: String,
    pub vulkan_api_version: String,
    pub hardware_vulkan_13: bool,
    pub dxvk_supported: bool,
    pub vkd3d_supported: bool,
    pub vram_total_gb: Option<f64>,
    pub limitations: Vec<String>,
    pub recommendations: Vec<String>,
}

#[derive(Debug, Clone, Serialize)]
pub struct GamingReadinessReport {
    pub gpu_matrix: GpuCapabilityMatrix,
    pub vulkan_available: bool,
    pub dxvk_available: bool,
    pub dxvk_source: String,
    pub vkd3d_available: bool,
    pub vkd3d_source: String,
    pub steam_available: bool,
    pub proton_available: bool,
    pub proton_source: String,
    pub gamescope_available: bool,
    pub mangohud_available: bool,
    pub gamemode_available: bool,
    pub wine_available: bool,
    pub missing_components: Vec<String>,
    pub anticheat_eula_notice: String,
    pub kernel_anticheat_bypass_forbidden: bool,
}

pub fn parse_semver_pair(version: &str) -> (u32, u32) {
    let mut parts = version.trim().split('.');
    let major = parts
        .next()
        .and_then(|p| p.chars().take_while(|c| c.is_ascii_digit()).collect::<String>().parse::<u32>().ok())
        .unwrap_or(0);
    let minor = parts
        .next()
        .and_then(|p| p.chars().take_while(|c| c.is_ascii_digit()).collect::<String>().parse::<u32>().ok())
        .unwrap_or(0);
    (major, minor)
}

pub fn parse_vulkaninfo_summary(summary_text: &str) -> VulkanSummary {
    let mut devices = Vec::new();
    let mut in_devices = false;
    let mut current: Option<std::collections::HashMap<String, String>> = None;

    let flush = |cur: &mut Option<std::collections::HashMap<String, String>>,
                 out: &mut Vec<VulkanDeviceInfo>| {
        let Some(map) = cur.take() else {
            return;
        };
        let name = map.get("deviceName").cloned().unwrap_or_else(|| "Unknown".to_string());
        let device_type = map.get("deviceType").cloned().unwrap_or_else(|| "unknown".to_string());
        let api_version = map.get("apiVersion").cloned().unwrap_or_else(|| "0.0.0".to_string());
        let driver_name = map.get("driverName").cloned().unwrap_or_else(|| "unknown".to_string());
        let driver_info = map.get("driverInfo").cloned().unwrap_or_default();
        let vendor_id = map.get("vendorID").cloned().unwrap_or_default();
        let device_id = map.get("deviceID").cloned().unwrap_or_default();

        let lower = format!("{name} {driver_name}").to_ascii_lowercase();
        let is_software = device_type == "PHYSICAL_DEVICE_TYPE_CPU"
            || SOFTWARE_GPU_MARKERS.iter().any(|m| lower.contains(m));
        let is_hardware = !is_software && HARDWARE_DEVICE_TYPES.iter().any(|t| *t == device_type);
        let (major, minor) = parse_semver_pair(&api_version);
        let meets_vulkan_13 = (major, minor) >= (1, 3);

        out.push(VulkanDeviceInfo {
            name,
            device_type,
            api_version,
            driver_name,
            driver_info,
            vendor_id,
            device_id,
            is_hardware,
            meets_vulkan_13,
        });
    };

    for raw in summary_text.lines() {
        let line = raw.trim();
        if line == "Devices:" {
            in_devices = true;
            continue;
        }
        if !in_devices {
            continue;
        }
        if line.starts_with("GPU") && line.ends_with(':') && line[3..line.len() - 1].chars().all(|c| c.is_ascii_digit()) {
            flush(&mut current, &mut devices);
            current = Some(std::collections::HashMap::new());
            continue;
        }
        if let Some(map) = current.as_mut() {
            if !line.starts_with('=') {
                if let Some((k, v)) = line.split_once('=') {
                    let key = k.trim();
                    if !key.is_empty() {
                        map.insert(key.to_string(), v.trim().to_string());
                    }
                }
            }
        }
    }
    flush(&mut current, &mut devices);

    if devices.is_empty() {
        return VulkanSummary {
            available: false,
            hardware_vulkan_13: false,
            devices,
            reason: "vulkaninfo çıktısında Vulkan GPU aygıtı bulunamadı.".to_string(),
        };
    }

    if let Some(best) = devices.iter().find(|d| d.is_hardware && d.meets_vulkan_13) {
        return VulkanSummary {
            available: true,
            hardware_vulkan_13: true,
            reason: format!(
                "Donanım Vulkan {} algılandı: {} ({}).",
                best.api_version, best.name, best.driver_name
            ),
            devices,
        };
    }

    if let Some(hw) = devices.iter().find(|d| d.is_hardware) {
        return VulkanSummary {
            available: true,
            hardware_vulkan_13: false,
            reason: format!(
                "Donanım GPU '{}' yalnızca Vulkan {} sunuyor (DXVK 2.x için Vulkan 1.3+ gerekir).",
                hw.name, hw.api_version
            ),
            devices,
        };
    }

    VulkanSummary {
        available: true,
        hardware_vulkan_13: false,
        devices,
        reason: "Yalnızca yazılım Vulkan aygıtı (llvmpipe/CPU) algılandı; donanım Vulkan yok.".to_string(),
    }
}

pub fn probe_vulkan(vulkaninfo_bin: &Path) -> VulkanSummary {
    if !vulkaninfo_bin.is_file() {
        return VulkanSummary {
            available: false,
            hardware_vulkan_13: false,
            devices: Vec::new(),
            reason: "vulkaninfo aracı kurulu değil (vulkan-tools paketi gerekli).".to_string(),
        };
    }
    let output = Command::new(vulkaninfo_bin).arg("--summary").output();
    match output {
        Ok(res) => {
            let stdout = String::from_utf8_lossy(&res.stdout);
            if !res.status.success() && !stdout.contains("Devices:") {
                VulkanSummary {
                    available: false,
                    hardware_vulkan_13: false,
                    devices: Vec::new(),
                    reason: format!("vulkaninfo hata koduyla sonlandı: {:?}", res.status.code()),
                }
            } else {
                parse_vulkaninfo_summary(&stdout)
            }
        }
        Err(err) => VulkanSummary {
            available: false,
            hardware_vulkan_13: false,
            devices: Vec::new(),
            reason: format!("vulkaninfo çalıştırılamadı: {err}"),
        },
    }
}

pub fn classify_gpu_matrix(
    vulkan: &VulkanSummary,
    vram_total_gb: Option<f64>,
) -> GpuCapabilityMatrix {
    let primary = vulkan
        .devices
        .iter()
        .find(|d| d.is_hardware && d.meets_vulkan_13)
        .or_else(|| vulkan.devices.iter().find(|d| d.is_hardware))
        .or_else(|| vulkan.devices.first());

    let Some(dev) = primary else {
        return GpuCapabilityMatrix {
            matched_profile: "unavailable".to_string(),
            tier: "vulkan_unavailable".to_string(),
            tier_label: "Vulkan Kullanılamıyor".to_string(),
            gpu_name: "Algılanmadı".to_string(),
            driver_name: "none".to_string(),
            driver_info: String::new(),
            vulkan_api_version: "N/A".to_string(),
            hardware_vulkan_13: false,
            dxvk_supported: false,
            vkd3d_supported: false,
            vram_total_gb,
            limitations: vec![vulkan.reason.clone()],
            recommendations: vec![
                "Vulkan sürücüsü ve 'vulkan-tools' kurulumunu doğrulayın; o zamana kadar WineD3D yedek motoru kullanılır.".to_string(),
            ],
        };
    };

    let lower = format!(
        "{} {} {} {} {}",
        dev.name, dev.driver_name, dev.driver_info, dev.vendor_id, dev.device_id
    )
    .to_ascii_lowercase();

    // 1. Software Vulkan (llvmpipe / lavapipe)
    if !dev.is_hardware {
        return GpuCapabilityMatrix {
            matched_profile: "software-vulkan".to_string(),
            tier: "software_only".to_string(),
            tier_label: "Yalnızca Yazılım Vulkan (CPU)".to_string(),
            gpu_name: dev.name.clone(),
            driver_name: dev.driver_name.clone(),
            driver_info: dev.driver_info.clone(),
            vulkan_api_version: dev.api_version.clone(),
            hardware_vulkan_13: false,
            dxvk_supported: false,
            vkd3d_supported: false,
            vram_total_gb,
            limitations: vec![
                "Donanım GPU hızlandırması yok; llvmpipe/yazılım Vulkan oyun için uygun değildir.".to_string(),
            ],
            recommendations: vec![
                "Uygun Mesa/Vulkan sürücüsünü (vulkan-radeon veya vulkan-intel) doğrulayın; şimdilik WineD3D kullanın.".to_string(),
            ],
        };
    }

    // 2. AMD RX 560 / RX 560D (Polaris11 / 0x67ef / RADV)
    let is_polaris11 = lower.contains("polaris11")
        || lower.contains("rx 560")
        || lower.contains("0x67ef")
        || lower.contains("67ef");
    if is_polaris11 {
        let mut limitations = vec![
            "AMD Polaris11 (GCN 4 / RADV): Donanım ışın izleme ve mesh-shader desteği yoktur; VKD3D-Proton (D3D12) yalnızca sınırlı/hafif başlıklarda uygundur.".to_string(),
            "RX 560/560D termal sınırı (~82 °C) ve 4 GB sınıfı VRAM bütçesi nedeniyle yüksek doku paketlerinde VRAM taşması ve takılma görülebilir.".to_string(),
        ];
        if let Some(vram) = vram_total_gb {
            if vram <= 4.2 {
                limitations.push(format!(
                    "Ölçülen VRAM ({vram:.2} GB): Modern D3D12 oyunlarda düşük/orta doku ayarı ve 1080p altı ölçekleme önerilir."
                ));
            }
        }
        return GpuCapabilityMatrix {
            matched_profile: "amd-rx560-polaris11".to_string(),
            tier: "dxvk_ready_vkd3d_limited".to_string(),
            tier_label: "DXVK Hazır · VKD3D Sınırlı (Polaris11)".to_string(),
            gpu_name: dev.name.clone(),
            driver_name: dev.driver_name.clone(),
            driver_info: dev.driver_info.clone(),
            vulkan_api_version: dev.api_version.clone(),
            hardware_vulkan_13: dev.meets_vulkan_13,
            dxvk_supported: dev.meets_vulkan_13,
            vkd3d_supported: false,
            vram_total_gb,
            limitations,
            recommendations: vec![
                "D3D9/10/11 oyunları için RADV + DXVK tercih edin.".to_string(),
                "82 °C üstü GPU sıcaklığında termal koruma Gaming profilini engeller; kasa hava akışı ve orta ayarları koruyun.".to_string(),
            ],
        };
    }

    // 3. Intel Broadwell HD 5500 (Gen8 / hasvk / 0x1616)
    let is_broadwell = lower.contains("hd graphics 5500")
        || lower.contains("broadwell")
        || lower.contains("bdw")
        || lower.contains("hasvk")
        || lower.contains("0x1616");
    if is_broadwell {
        return GpuCapabilityMatrix {
            matched_profile: "intel-broadwell-hd5500".to_string(),
            tier: "legacy_vulkan_12_wined3d".to_string(),
            tier_label: "Eski Vulkan 1.2 · WineD3D Önerilir (Broadwell)".to_string(),
            gpu_name: dev.name.clone(),
            driver_name: dev.driver_name.clone(),
            driver_info: dev.driver_info.clone(),
            vulkan_api_version: dev.api_version.clone(),
            hardware_vulkan_13: dev.meets_vulkan_13,
            dxvk_supported: false,
            vkd3d_supported: false,
            vram_total_gb,
            limitations: vec![
                format!(
                    "Intel Broadwell HD 5500 (Mesa intel_hasvk) Vulkan {} ile sınırlıdır; DXVK 2.x (Vulkan 1.3+) ve VKD3D-Proton desteklenmez.",
                    dev.api_version
                ),
                "Paylaşımlı sistem belleği ve sınırlı yürütme birimi nedeniyle yalnızca hafif/klasik 2D ve eski 3D oyunlar uygundur.".to_string(),
            ],
            recommendations: vec![
                "Wine uyumluluk profillerinde WineD3D (OpenGL) veya yerel Linux oyunlarını tercih edin.".to_string(),
            ],
        };
    }

    // 4. AMD Radeon Vega 3 APU (Raven/Picasso / 0x15d8 / 0x15dd)
    let is_vega3 = lower.contains("vega 3")
        || lower.contains("vega3")
        || lower.contains("raven2")
        || lower.contains("0x15d8")
        || lower.contains("0x15dd");
    if is_vega3 {
        return GpuCapabilityMatrix {
            matched_profile: "amd-vega3-apu".to_string(),
            tier: "apu_dxvk_light".to_string(),
            tier_label: "Hafif Oyunlar İçin DXVK Uygun (Vega 3 APU)".to_string(),
            gpu_name: dev.name.clone(),
            driver_name: dev.driver_name.clone(),
            driver_info: dev.driver_info.clone(),
            vulkan_api_version: dev.api_version.clone(),
            hardware_vulkan_13: dev.meets_vulkan_13,
            dxvk_supported: dev.meets_vulkan_13,
            vkd3d_supported: false,
            vram_total_gb,
            limitations: vec![
                "AMD Vega 3 tümleşik APU (3 CU) paylaşımlı sistem RAM/UMA bellek bant genişliği ve düşük termal zarfla sınırlıdır.".to_string(),
                "VKD3D-Proton (D3D12) ağır modern oyunlarda yetersiz performans ve bellek baskısı oluşturabilir.".to_string(),
            ],
            recommendations: vec![
                "720p/düşük ayarlarda hafif D3D9-11 ve bağımsız (indie) oyunlar için RADV + DXVK kullanın.".to_string(),
                "Çift kanal sistem RAM ve dengeli termal profil (General/Gaming) koruyun.".to_string(),
            ],
        };
    }

    // 5. Generic Hardware GPU
    if dev.meets_vulkan_13 {
        GpuCapabilityMatrix {
            matched_profile: "generic-vulkan-13".to_string(),
            tier: "dxvk_vkd3d_ready".to_string(),
            tier_label: "Vulkan 1.3+ · DXVK/VKD3D Uygun".to_string(),
            gpu_name: dev.name.clone(),
            driver_name: dev.driver_name.clone(),
            driver_info: dev.driver_info.clone(),
            vulkan_api_version: dev.api_version.clone(),
            hardware_vulkan_13: true,
            dxvk_supported: true,
            vkd3d_supported: true,
            vram_total_gb,
            limitations: Vec::new(),
            recommendations: vec![
                "DXVK ve VKD3D-Proton için donanım Vulkan 1.3+ desteği doğrulandı.".to_string(),
            ],
        }
    } else {
        GpuCapabilityMatrix {
            matched_profile: "generic-legacy-vulkan".to_string(),
            tier: "legacy_vulkan_wined3d".to_string(),
            tier_label: "Eski Vulkan (< 1.3) · WineD3D Yedek".to_string(),
            gpu_name: dev.name.clone(),
            driver_name: dev.driver_name.clone(),
            driver_info: dev.driver_info.clone(),
            vulkan_api_version: dev.api_version.clone(),
            hardware_vulkan_13: false,
            dxvk_supported: false,
            vkd3d_supported: false,
            vram_total_gb,
            limitations: vec![format!(
                "GPU '{}' Vulkan {} sunuyor (< 1.3); DXVK 2.x ve VKD3D-Proton kullanılamaz.",
                dev.name, dev.api_version
            )],
            recommendations: vec!["WineD3D yedek motorunu kullanın.".to_string()],
        }
    }
}

fn find_tool(dirs: &[PathBuf], binary: &str) -> bool {
    dirs.iter().any(|dir| dir.join(binary).is_file())
}

fn find_dll_source(dirs: &[PathBuf], dlls: &[&str]) -> (bool, String) {
    for dir in dirs {
        if dlls.iter().any(|dll| dir.join(dll).is_file()) {
            return (true, format!("system:{}", dir.display()));
        }
    }
    (false, "none".to_string())
}

fn find_proton_source(dirs: &[PathBuf]) -> (bool, String) {
    for dir in dirs {
        let Ok(entries) = fs::read_dir(dir) else {
            continue;
        };
        for entry in entries.flatten() {
            let name = entry.file_name().to_string_lossy().to_ascii_lowercase();
            if name.contains("proton") && entry.path().is_dir() {
                return (true, entry.path().display().to_string());
            }
        }
    }
    (false, "none".to_string())
}

pub fn probe_gaming_readiness(
    cfg: &RuntimeConfig,
    vram_total_gb: Option<f64>,
) -> GamingReadinessReport {
    let vulkan = probe_vulkan(&cfg.vulkaninfo_bin);
    let gpu_matrix = classify_gpu_matrix(&vulkan, vram_total_gb);

    let (dxvk_available, dxvk_source) = find_dll_source(&cfg.dxvk_dirs, &["d3d11.dll"]);
    let (vkd3d_available, vkd3d_source) =
        find_dll_source(&cfg.vkd3d_dirs, &["d3d12.dll", "d3d12core.dll"]);
    let steam_available = find_tool(&cfg.tool_search_dirs, "steam");
    let (proton_available, proton_source) = find_proton_source(&cfg.proton_dirs);
    let gamescope_available = find_tool(&cfg.tool_search_dirs, "gamescope");
    let mangohud_available = find_tool(&cfg.tool_search_dirs, "mangohud");
    let gamemode_available = find_tool(&cfg.tool_search_dirs, "gamemoded");
    let wine_available = find_tool(&cfg.tool_search_dirs, "wine");

    let mut missing = Vec::new();
    if !vulkan.available {
        missing.push("vulkan-tools / Vulkan ICD sürücüsü".to_string());
    }
    if gpu_matrix.dxvk_supported && !dxvk_available {
        missing.push("dxvk-mingw-git (D3D9/10/11 -> Vulkan çeviri katmanı)".to_string());
    }
    if gpu_matrix.vkd3d_supported && !vkd3d_available {
        missing.push("vkd3d-proton-mingw-git (D3D12 -> Vulkan çeviri katmanı)".to_string());
    }
    if !steam_available {
        missing.push("steam (isteğe bağlı oyun istemcisi)".to_string());
    }
    if !gamescope_available {
        missing.push("gamescope (isteğe bağlı mikro-kompozitör)".to_string());
    }
    if !mangohud_available {
        missing.push("mangohud (isteğe bağlı FPS/sıcaklık katmanı)".to_string());
    }
    if !gamemode_available {
        missing.push("gamemode (isteğe bağlı oturum zamanlayıcı yardımcısı)".to_string());
    }
    if !wine_available {
        missing.push("wine (Windows uyumluluk çalışma zamanı)".to_string());
    }

    GamingReadinessReport {
        gpu_matrix,
        vulkan_available: vulkan.available,
        dxvk_available,
        dxvk_source,
        vkd3d_available,
        vkd3d_source,
        steam_available,
        proton_available,
        proton_source,
        gamescope_available,
        mangohud_available,
        gamemode_available,
        wine_available,
        missing_components: missing,
        anticheat_eula_notice: ANTICHEAT_EULA_NOTICE.to_string(),
        kernel_anticheat_bypass_forbidden: true,
    }
}
