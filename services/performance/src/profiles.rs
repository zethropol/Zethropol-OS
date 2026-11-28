use serde::{Deserialize, Serialize};
use std::fs;
use std::path::{Path, PathBuf};

pub const DEFAULT_CPU_THERMAL_LIMIT_C: f64 = 85.0;
pub const DEFAULT_GPU_THERMAL_LIMIT_C: f64 = 82.0;
pub const POLKIT_ACTION_MODIFY: &str = "org.zethropol.performance.modify";

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum PerformanceProfile {
    General,
    Gaming,
    Developer,
    Creator,
}

impl PerformanceProfile {
    pub const ALL: [PerformanceProfile; 4] = [
        PerformanceProfile::General,
        PerformanceProfile::Gaming,
        PerformanceProfile::Developer,
        PerformanceProfile::Creator,
    ];

    pub fn parse(raw: &str) -> Option<Self> {
        match raw.trim().to_ascii_lowercase().as_str() {
            "general" => Some(Self::General),
            "gaming" => Some(Self::Gaming),
            "developer" => Some(Self::Developer),
            "creator" => Some(Self::Creator),
            _ => None,
        }
    }

    pub fn as_str(self) -> &'static str {
        match self {
            Self::General => "general",
            Self::Gaming => "gaming",
            Self::Developer => "developer",
            Self::Creator => "creator",
        }
    }

    pub fn display_name(self) -> &'static str {
        match self {
            Self::General => "General",
            Self::Gaming => "Gaming",
            Self::Developer => "Developer",
            Self::Creator => "Creator",
        }
    }

    pub fn description(self) -> &'static str {
        match self {
            Self::General => {
                "Varsayılan dengeli profil; günlük kullanım için verimli CPU/GPU saatleri ve düşük gürültü."
            }
            Self::Gaming => {
                "Düşük gecikme ve kararlı kare süresi için yüksek öncelikli CPU governor ve GPU DPM optimizasyonu (termal sınır korumalı)."
            }
            Self::Developer => {
                "Derleme, test ve paralel iş yükleri için CPU öncelikli yüksek akış; GPU otomatik/serin modda korunur."
            }
            Self::Creator => {
                "Video, ses ve medya render iş yükleri için uzun süreli kararlı frekans ve dengeli termal süreklilik."
            }
        }
    }

    pub fn preferred_governors(self) -> &'static [&'static str] {
        match self {
            Self::General => &["schedutil", "ondemand", "powersave"],
            Self::Gaming => &["performance", "schedutil"],
            Self::Developer => &["performance", "schedutil", "ondemand"],
            Self::Creator => &["schedutil", "performance", "ondemand"],
        }
    }

    pub fn preferred_gpu_dpm(self) -> &'static str {
        match self {
            Self::General => "auto",
            Self::Gaming => "high",
            Self::Developer => "auto",
            Self::Creator => "auto",
        }
    }

    pub fn thermal_guard_blocks(self) -> bool {
        matches!(self, Self::Gaming)
    }

    pub fn thermal_soft_fallback(self) -> bool {
        matches!(self, Self::Developer)
    }
}

#[derive(Debug, Clone)]
pub struct RuntimeConfig {
    pub cpufreq_dir: PathBuf,
    pub drm_dir: PathBuf,
    pub proc_dir: PathBuf,
    pub state_file: PathBuf,
    pub pkcheck_bin: PathBuf,
    pub vulkaninfo_bin: PathBuf,
    pub tool_search_dirs: Vec<PathBuf>,
    pub dxvk_dirs: Vec<PathBuf>,
    pub vkd3d_dirs: Vec<PathBuf>,
    pub proton_dirs: Vec<PathBuf>,
    pub cpu_thermal_limit_c: f64,
    pub gpu_thermal_limit_c: f64,
}

impl Default for RuntimeConfig {
    fn default() -> Self {
        Self {
            cpufreq_dir: PathBuf::from("/sys/devices/system/cpu/cpufreq"),
            drm_dir: PathBuf::from("/sys/class/drm"),
            proc_dir: PathBuf::from("/proc"),
            state_file: PathBuf::from("/var/lib/zethropol-performance/profile.json"),
            pkcheck_bin: PathBuf::from("/usr/bin/pkcheck"),
            vulkaninfo_bin: PathBuf::from("/usr/bin/vulkaninfo"),
            tool_search_dirs: vec![PathBuf::from("/usr/bin"), PathBuf::from("/usr/local/bin")],
            dxvk_dirs: vec![
                PathBuf::from("/usr/share/dxvk/x64"),
                PathBuf::from("/usr/lib/dxvk/x64"),
                PathBuf::from("/usr/share/dxvk/x32"),
            ],
            vkd3d_dirs: vec![
                PathBuf::from("/usr/share/vkd3d-proton/x64"),
                PathBuf::from("/usr/lib/vkd3d-proton/x64"),
            ],
            proton_dirs: vec![
                PathBuf::from("/usr/share/steam/compatibilitytools.d"),
                PathBuf::from("/usr/local/share/steam/compatibilitytools.d"),
            ],
            cpu_thermal_limit_c: DEFAULT_CPU_THERMAL_LIMIT_C,
            gpu_thermal_limit_c: DEFAULT_GPU_THERMAL_LIMIT_C,
        }
    }
}

impl RuntimeConfig {
    pub fn from_env() -> Self {
        let mut cfg = Self::default();
        let path_env = |key: &str| -> Option<PathBuf> {
            std::env::var(key)
                .ok()
                .map(|v| v.trim().to_string())
                .filter(|v| !v.is_empty())
                .map(PathBuf::from)
        };
        let list_env = |key: &str| -> Option<Vec<PathBuf>> {
            std::env::var(key).ok().map(|raw| {
                raw.split(':')
                    .map(str::trim)
                    .filter(|s| !s.is_empty())
                    .map(PathBuf::from)
                    .collect()
            })
        };

        if let Some(p) = path_env("ZETHROPOL_PERFORMANCE_CPUFREQ_DIR") {
            cfg.cpufreq_dir = p;
        }
        if let Some(p) = path_env("ZETHROPOL_PERFORMANCE_DRM_DIR") {
            cfg.drm_dir = p;
        }
        if let Some(p) = path_env("ZETHROPOL_PERFORMANCE_PROC_DIR") {
            cfg.proc_dir = p;
        }
        if let Some(p) = path_env("ZETHROPOL_PERFORMANCE_STATE_FILE") {
            cfg.state_file = p;
        }
        if let Some(p) = path_env("ZETHROPOL_PERFORMANCE_PKCHECK_BIN") {
            cfg.pkcheck_bin = p;
        }
        if let Some(p) = path_env("ZETHROPOL_PERFORMANCE_VULKANINFO_BIN") {
            cfg.vulkaninfo_bin = p;
        }
        if let Some(list) = list_env("ZETHROPOL_PERFORMANCE_TOOL_DIRS") {
            cfg.tool_search_dirs = list;
        }
        if let Some(list) = list_env("ZETHROPOL_PERFORMANCE_DXVK_DIRS") {
            cfg.dxvk_dirs = list;
        }
        if let Some(list) = list_env("ZETHROPOL_PERFORMANCE_VKD3D_DIRS") {
            cfg.vkd3d_dirs = list;
        }
        if let Some(list) = list_env("ZETHROPOL_PERFORMANCE_PROTON_DIRS") {
            cfg.proton_dirs = list;
        }
        if let Ok(v) = std::env::var("ZETHROPOL_PERFORMANCE_CPU_THERMAL_LIMIT_C") {
            if let Ok(limit) = v.trim().parse::<f64>() {
                if limit.is_finite() && (50.0..=105.0).contains(&limit) {
                    cfg.cpu_thermal_limit_c = limit;
                }
            }
        }
        if let Ok(v) = std::env::var("ZETHROPOL_PERFORMANCE_GPU_THERMAL_LIMIT_C") {
            if let Ok(limit) = v.trim().parse::<f64>() {
                if limit.is_finite() && (50.0..=105.0).contains(&limit) {
                    cfg.gpu_thermal_limit_c = limit;
                }
            }
        }
        cfg
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SavedProfileState {
    pub active_profile: PerformanceProfile,
}

pub fn load_saved_profile(state_file: &Path) -> PerformanceProfile {
    let Ok(raw) = fs::read_to_string(state_file) else {
        return PerformanceProfile::General;
    };
    serde_json::from_str::<SavedProfileState>(&raw)
        .map(|s| s.active_profile)
        .unwrap_or(PerformanceProfile::General)
}

pub fn save_profile(state_file: &Path, profile: PerformanceProfile) -> Result<(), String> {
    if let Some(parent) = state_file.parent() {
        fs::create_dir_all(parent).map_err(|e| format!("failed to create state dir: {e}"))?;
    }
    let payload = serde_json::to_string_pretty(&SavedProfileState {
        active_profile: profile,
    })
    .map_err(|e| e.to_string())?;
    let tmp = state_file.with_extension("json.tmp");
    fs::write(&tmp, format!("{payload}\n"))
        .map_err(|e| format!("failed to write profile state: {e}"))?;
    fs::rename(&tmp, state_file)
        .map_err(|e| format!("failed to persist profile state: {e}"))?;
    Ok(())
}

#[derive(Debug, Clone, Serialize)]
pub struct ThermalAssessment {
    pub guard_active: bool,
    pub cpu_temp_c: Option<f64>,
    pub gpu_temp_c: Option<f64>,
    pub cpu_limit_c: f64,
    pub gpu_limit_c: f64,
    pub reason: String,
}

pub fn evaluate_thermal_guard(
    cpu_temp_c: Option<f64>,
    gpu_temp_c: Option<f64>,
    cpu_limit_c: f64,
    gpu_limit_c: f64,
) -> ThermalAssessment {
    let mut reasons = Vec::new();
    if let Some(cpu) = cpu_temp_c {
        if cpu.is_finite() && cpu >= cpu_limit_c {
            reasons.push(format!(
                "CPU sıcaklığı ({cpu:.1} °C) güvenli sınırı ({cpu_limit_c:.1} °C) aştı"
            ));
        }
    }
    if let Some(gpu) = gpu_temp_c {
        if gpu.is_finite() && gpu >= gpu_limit_c {
            reasons.push(format!(
                "GPU sıcaklığı ({gpu:.1} °C) güvenli sınırı ({gpu_limit_c:.1} °C) aştı"
            ));
        }
    }
    let guard_active = !reasons.is_empty();
    let reason = if guard_active {
        format!(
            "Termal koruma etkin: {}. Yüksek güç/oyun profili geçici olarak engellendi.",
            reasons.join("; ")
        )
    } else {
        "Termal sınırlar güvenli aralıkta.".to_string()
    };

    ThermalAssessment {
        guard_active,
        cpu_temp_c,
        gpu_temp_c,
        cpu_limit_c,
        gpu_limit_c,
        reason,
    }
}

pub fn validate_governor_name(governor: &str) -> bool {
    !governor.is_empty()
        && governor.len() <= 32
        && governor
            .chars()
            .all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '_' || c == '-')
}

pub fn validate_gpu_dpm_level(level: &str) -> bool {
    matches!(level, "auto" | "high" | "low")
}

#[derive(Debug, Clone, Serialize)]
pub struct CpuGovernorInfo {
    pub current: Option<String>,
    pub available: Vec<String>,
}

pub fn read_cpu_governor_info(cpufreq_dir: &Path) -> CpuGovernorInfo {
    let mut policies = collect_cpufreq_policies(cpufreq_dir);
    policies.sort();
    let mut current = None;
    let mut available = Vec::new();

    for policy in policies {
        if current.is_none() {
            if let Ok(raw) = fs::read_to_string(policy.join("scaling_governor")) {
                let trimmed = raw.trim();
                if validate_governor_name(trimmed) {
                    current = Some(trimmed.to_string());
                }
            }
        }
        if available.is_empty() {
            if let Ok(raw) = fs::read_to_string(policy.join("scaling_available_governors")) {
                for tok in raw.split_whitespace() {
                    if validate_governor_name(tok) && !available.iter().any(|x| x == tok) {
                        available.push(tok.to_string());
                    }
                }
            }
        }
        if current.is_some() && !available.is_empty() {
            break;
        }
    }

    CpuGovernorInfo { current, available }
}

fn collect_cpufreq_policies(cpufreq_dir: &Path) -> Vec<PathBuf> {
    let Ok(entries) = fs::read_dir(cpufreq_dir) else {
        return Vec::new();
    };
    entries
        .flatten()
        .map(|e| e.path())
        .filter(|p| {
            p.file_name()
                .and_then(|n| n.to_str())
                .is_some_and(|n| {
                    n.strip_prefix("policy")
                        .is_some_and(|rest| !rest.is_empty() && rest.chars().all(|c| c.is_ascii_digit()))
                })
        })
        .collect()
}

pub fn apply_cpu_governor(cpufreq_dir: &Path, preferred: &[&str]) -> Result<Option<String>, String> {
    let mut policies = collect_cpufreq_policies(cpufreq_dir);
    policies.sort();
    if policies.is_empty() {
        return Ok(None);
    }

    let info = read_cpu_governor_info(cpufreq_dir);
    let Some(chosen) = preferred
        .iter()
        .copied()
        .find(|cand| validate_governor_name(cand) && info.available.iter().any(|a| a == cand))
    else {
        return Ok(None);
    };

    let mut applied_any = false;
    for policy in policies {
        let avail = fs::read_to_string(policy.join("scaling_available_governors")).unwrap_or_default();
        if !avail.split_whitespace().any(|tok| tok == chosen) {
            continue;
        }
        fs::write(policy.join("scaling_governor"), chosen)
            .map_err(|e| format!("CPU governor yazımı başarısız ({}): {e}", policy.display()))?;
        applied_any = true;
    }

    Ok(applied_any.then(|| chosen.to_string()))
}

fn collect_drm_dpm_paths(drm_dir: &Path) -> Vec<PathBuf> {
    let Ok(entries) = fs::read_dir(drm_dir) else {
        return Vec::new();
    };
    let mut cards: Vec<(u32, PathBuf)> = Vec::new();
    for entry in entries.flatten() {
        let name = entry.file_name().to_string_lossy().into_owned();
        let Some(num) = name
            .strip_prefix("card")
            .filter(|s| !s.is_empty() && s.chars().all(|c| c.is_ascii_digit()))
            .and_then(|s| s.parse::<u32>().ok())
        else {
            continue;
        };
        let dpm = entry.path().join("device").join("power_dpm_force_performance_level");
        if dpm.is_file() {
            cards.push((num, dpm));
        }
    }
    cards.sort_by_key(|(num, _)| *num);
    cards.into_iter().map(|(_, p)| p).collect()
}

pub fn read_amd_gpu_dpm(drm_dir: &Path) -> Option<String> {
    for path in collect_drm_dpm_paths(drm_dir) {
        if let Ok(raw) = fs::read_to_string(&path) {
            let trimmed = raw.trim();
            if !trimmed.is_empty() && trimmed.len() <= 32 {
                return Some(trimmed.to_string());
            }
        }
    }
    None
}

pub fn apply_amd_gpu_dpm(drm_dir: &Path, level: &str) -> Result<Option<String>, String> {
    if !validate_gpu_dpm_level(level) {
        return Err(format!("Geçersiz GPU DPM düzeyi: {level}"));
    }
    let paths = collect_drm_dpm_paths(drm_dir);
    if paths.is_empty() {
        return Ok(None);
    }
    for path in &paths {
        fs::write(path, level)
            .map_err(|e| format!("GPU DPM yazımı başarısız ({}): {e}", path.display()))?;
    }
    Ok(Some(level.to_string()))
}

#[derive(Debug, Clone, Serialize)]
pub struct GamingSessionStatus {
    pub active: bool,
    pub detected_processes: Vec<String>,
    pub recommended_profile: String,
    pub silent_auto_switch_blocked: bool,
    pub advisory: String,
}

const GAMING_COMM_EXACT: &[&str] = &[
    "gamescope",
    "wine64-preloader",
    "wine-preloader",
    "hl2_linux",
    "cs2",
    "dota2",
];

const GAMING_CMDLINE_MARKERS: &[&str] = &[
    "steamlaunch",
    "proton",
    "gamescope",
    "mangohud",
    "steamapps/common/",
    "zethropol-wine-run",
];

const EXCLUDED_COMM_MARKERS: &[&str] = &[
    "control_center",
    "zethropol-perf",
    "zethropol-hard",
    "zethropol-moni",
    "zethropol-reco",
    "cargo",
    "rustc",
];

pub fn detect_gaming_session(proc_dir: &Path, thermal_guard_active: bool) -> GamingSessionStatus {
    let mut detected = Vec::new();
    if let Ok(entries) = fs::read_dir(proc_dir) {
        let mut pids: Vec<u32> = entries
            .flatten()
            .filter_map(|e| e.file_name().to_str()?.parse::<u32>().ok())
            .collect();
        pids.sort_unstable();

        for pid in pids.into_iter().take(2048) {
            let pid_dir = proc_dir.join(pid.to_string());
            let comm = fs::read_to_string(pid_dir.join("comm"))
                .unwrap_or_default()
                .trim()
                .to_string();
            if comm.is_empty() {
                continue;
            }
            let comm_lower = comm.to_ascii_lowercase();
            if EXCLUDED_COMM_MARKERS
                .iter()
                .any(|excl| comm_lower.starts_with(excl))
            {
                continue;
            }

            let mut matched = GAMING_COMM_EXACT.iter().any(|m| comm_lower == *m);
            if !matched {
                if let Ok(raw_cmd) = fs::read(pid_dir.join("cmdline")) {
                    let cmd_text = String::from_utf8_lossy(&raw_cmd)
                        .replace('\0', " ")
                        .to_ascii_lowercase();
                    // Avoid matching the background Steam client itself; require launch wrapper or game path
                    if cmd_text.contains("steamlaunch")
                        || cmd_text.contains("steamapps/common/")
                        || GAMING_CMDLINE_MARKERS.iter().any(|m| {
                            *m != "steamlaunch" && *m != "steamapps/common/" && cmd_text.contains(m)
                        })
                    {
                        matched = true;
                    }
                }
            }

            if matched && !detected.iter().any(|d| d == &comm) {
                detected.push(comm);
                if detected.len() >= 8 {
                    break;
                }
            }
        }
    }

    let active = !detected.is_empty();
    let (recommended_profile, advisory) = if active && thermal_guard_active {
        (
            "general".to_string(),
            "Aktif oyun oturumu algılandı ancak termal koruma devrede; soğuma sağlanana kadar General profili önerilir. Sessiz profil geçişi yapılmaz.".to_string(),
        )
    } else if active {
        (
            "gaming".to_string(),
            "Aktif oyun oturumu algılandı. İsterseniz Gaming profilini açık onayınızla etkinleştirebilirsiniz; sessiz profil dayatması kapalıdır.".to_string(),
        )
    } else {
        (
            "general".to_string(),
            "Aktif oyun oturumu algılanmadı. Sessiz profil dayatması kapalıdır.".to_string(),
        )
    };

    GamingSessionStatus {
        active,
        detected_processes: detected,
        recommended_profile,
        silent_auto_switch_blocked: true,
        advisory,
    }
}

#[derive(Debug, Clone, Serialize)]
pub struct ProfileDescriptor {
    pub id: String,
    pub title: String,
    pub description: String,
    pub target_governor: String,
    pub target_gpu_dpm: String,
    pub thermal_guard_applies: bool,
    pub allowed_now: bool,
    pub blocked_reason: String,
}

pub fn build_profile_descriptors(
    gov_info: &CpuGovernorInfo,
    thermal: &ThermalAssessment,
) -> Vec<ProfileDescriptor> {
    PerformanceProfile::ALL
        .iter()
        .map(|&p| {
            let target_governor = p
                .preferred_governors()
                .iter()
                .copied()
                .find(|g| gov_info.available.iter().any(|a| a == g))
                .unwrap_or(p.preferred_governors()[0])
                .to_string();
            let blocked = p.thermal_guard_blocks() && thermal.guard_active;
            ProfileDescriptor {
                id: p.as_str().to_string(),
                title: p.display_name().to_string(),
                description: p.description().to_string(),
                target_governor,
                target_gpu_dpm: p.preferred_gpu_dpm().to_string(),
                thermal_guard_applies: p.thermal_guard_blocks(),
                allowed_now: !blocked,
                blocked_reason: if blocked {
                    thermal.reason.clone()
                } else {
                    String::new()
                },
            }
        })
        .collect()
}
