# Zethropol ZCI Komut Ajanı & ZEKANS Entegrasyonu (`zethropol-zci-agent 0.1.0-1`)

Aşama 13 (`13.01`–`13.07`) kapsamında geliştirilen yerel, politika kapılı ve kullanıcı onaylı çok adımlı komut/planlama ajanıdır.

## Mimari ve Güvenlik Sınırları

1. **Güvenli Planlama ve Sıfır Ham Kabuk (`13.01`):**
   - `/usr/lib/zethropol/zethropol-zci-agentd` kullanıcı oturumu D-Bus servisi (`org.zethropol.ZciAgent.v1`, `/org/zethropol/ZciAgent`) doğal dil hedeflerini `org.zethropol.ZciRuntime.v1` üzerinden salt-veri (`WrapUntrustedData`, `executable=false`, `policy_authority="none"`) yapılandırılmış adımlara dönüştürür.
   - Hiçbir adım `sh -c`, `bash -c`, `eval` veya ham kabuk dizgesi olarak çalıştırılmaz; yalnızca derleme zamanı izin listeli (`allowlist`) yapılandırılmış eylemler (`read_file`, `project_analysis`, `system_diagnostics`, `recovery_readiness`, `write_workspace_file`, `create_workspace_file`) kabul edilir.
2. **Politika Geçidi & Kullanıcı Onayı (`13.02`):**
   - Her yürütülebilir adım `org.zethropol.ZciDaemon.v1.ExecuteAction` ve `org.zethropol.Security.Policy.v1.EvaluateAction` üzerinden doğrulanır.
   - Yazma/oluşturma adımları (`ApprovalRequired`) kullanıcı `ApproveStep` ile açıkça onaylamadan fail-closed (`unapproved-steps-pending`) engellenir; onay sonrasında dahi yalnızca `zci-daemon` `userspace_preflight` (`delegated-userspace-approved`, `UserSpaceSafe`) ve izinli çalışma alanı sınırları (`~/Zethropol-OS`, `~/Projects`, `/tmp/zekans-work`) içinde atomik, sembolik-bağlantı reddeden yazma yapılır.
   - `/boot`, `/etc/zethropol`, `/etc/polkit-1`, `/etc/dbus-1`, `/usr`, `/sys`, `/proc`, `/dev`, `/root`, `AGENTS.md` ve `services/recovery/src/recovery_service.rs` hedefleri fail-closed reddedilir.
3. **Snapper Ön-Kontrol & Kurtarma Kilitleri (`13.03`):**
   - Plan önerisinde ve her onaylı yazma yürütmesi öncesinde salt-okunur `org.zethropol.Recovery.v1.GetState` ön-kontrolü yapılır; `rollback_execution_enabled=false` ve `repair_execution_enabled=false` kilitleri kalıcı olarak korunur.
4. **D-Bus Arayüzü (`13.04` — `org.zethropol.ZciAgent.v1`):**
   - **Metotlar:** `ProposePlan`, `ApproveStep`, `ExecuteApprovedPlan`, `CancelPlan`, `GetPlanStatus`, `GetAgentStatus` (ek olarak `GetState`, `ListPlans`, `GetAuditTrail`).
   - **Sinyaller:** `PlanProposed`, `StepApprovalRequired`, `StepCompleted`, `PlanFinished`.
5. **Masaüstü Qt6/QML + CLI & `zci-code` Hizalaması (`13.05`):**
   - `/usr/bin/zci-agent` (koyu cam `#0b111d` Qt6/QML onay penceresi, `/usr/share/applications/zci-agent.desktop`) ve headless terminal kipi (`--status`, `--plan`, `--approve`, `--execute`, `--cancel`, `--plan-status`, `--audit`).
   - `zci-code agent ...` çağrıları `services/local-inference/zci-code-gemma4-shared` üzerinden doğrudan `zci-agent` istemcisine yönlendirilir; `Qt6::Network` ve `QProcess` bağlantısı yoktur.
6. **Değiştirilemez Hash-Zincirli Denetim Günlüğü (`13.06`):**
   - Tüm planlar, onay/red kararları, güvenlik engelleri ve yürütme sonuçları `~/.local/share/zethropol/zci-agent/audit-trail.jsonl` (`0700`/`0600`, sembolik bağlantı reddeden) içine SHA-256 hash-zinciriyle yazılır ve her sorguda zincir bütünlüğü doğrulanır.
