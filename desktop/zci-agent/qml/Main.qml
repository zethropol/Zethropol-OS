import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root
    objectName: "zciAgentWindow"
    width: 1140
    height: 760
    minimumWidth: 900
    minimumHeight: 620
    visible: true
    title: "Zethropol ZCI Komut Ajanı & ZEKANS"
    color: "#0b111d"

    readonly property color bgSurface: "#0b111d"
    readonly property color glassPanel: "#131c2e"
    readonly property color glassCard: "#182338"
    readonly property color glassBorder: "#273652"
    readonly property color accentCyan: "#38bdf8"
    readonly property color accentTeal: "#2dd4bf"
    readonly property color accentAmber: "#f59e0b"
    readonly property color accentRed: "#f87171"
    readonly property color accentGreen: "#34d399"
    readonly property color textPrimary: "#f1f5f9"
    readonly property color textSecondary: "#94a3b8"

    function riskColor(risk) {
        if (risk === "ApprovalRequired") return accentAmber
        if (risk === "UserSpaceSafe") return accentTeal
        return accentCyan
    }

    function statusColor(st) {
        if (st === "completed" || st === "approved" || st === "auto-readonly") return accentGreen
        if (st === "denied" || st === "failed" || st === "rejected" || st === "cancelled") return accentRed
        if (st === "pending" || st === "awaiting-approval") return accentAmber
        return accentCyan
    }

    header: Rectangle {
        height: 76
        color: "#0f172a"
        border.color: glassBorder
        border.width: 1

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 18
            anchors.rightMargin: 18
            spacing: 12

            ColumnLayout {
                spacing: 2
                Text {
                    text: "ZCI Komut Ajanı & ZEKANS"
                    color: textPrimary
                    font.pixelSize: 18
                    font.bold: true
                }
                Text {
                    text: agentClient ? agentClient.statusMessage : ""
                    color: textSecondary
                    font.pixelSize: 11
                    elide: Text.ElideRight
                    Layout.maximumWidth: 420
                }
            }

            Item { Layout.fillWidth: true }

            // Live Standby / Active + VRAM badge
            Rectangle {
                objectName: "runtimeStateBadge"
                radius: 8
                color: "#16243a"
                border.color: (agentClient && agentClient.runtimeState === "Active") ? accentAmber : accentTeal
                border.width: 1
                implicitHeight: 32
                implicitWidth: rtRow.implicitWidth + 18
                RowLayout {
                    id: rtRow
                    anchors.centerIn: parent
                    spacing: 6
                    Rectangle {
                        width: 8
                        height: 8
                        radius: 4
                        color: (agentClient && agentClient.runtimeState === "Active") ? accentAmber : accentTeal
                    }
                    Text {
                        text: agentClient
                              ? (agentClient.runtimeState + " · VRAM " + agentClient.activeVramMb + " MB")
                              : "Standby"
                        color: textPrimary
                        font.pixelSize: 11
                        font.bold: true
                    }
                }
            }

            // Hardware profile badge
            Rectangle {
                objectName: "hardwareProfileBadge"
                radius: 8
                color: "#16243a"
                border.color: glassBorder
                border.width: 1
                implicitHeight: 32
                implicitWidth: hwLabel.implicitWidth + 16
                Text {
                    id: hwLabel
                    anchors.centerIn: parent
                    text: agentClient ? agentClient.hardwareProfile : "dev-baseline"
                    color: accentCyan
                    font.pixelSize: 11
                    font.bold: true
                }
            }

            // Profile selector: zekans-code (zci-code) vs zci-command
            RowLayout {
                spacing: 6
                Button {
                    objectName: "profileZekansBtn"
                    text: "zci-code"
                    onClicked: if (agentClient) agentClient.activeProfile = "zekans-code"
                    background: Rectangle {
                        radius: 8
                        color: (agentClient && agentClient.activeProfile === "zekans-code") ? "#1d3b5c" : "#131c2e"
                        border.color: (agentClient && agentClient.activeProfile === "zekans-code") ? accentCyan : glassBorder
                    }
                    contentItem: Text {
                        text: parent.text
                        color: textPrimary
                        font.pixelSize: 11
                        font.bold: true
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        leftPadding: 10
                        rightPadding: 10
                    }
                }
                Button {
                    objectName: "profileCommandBtn"
                    text: "zci-command"
                    onClicked: if (agentClient) agentClient.activeProfile = "zci-command"
                    background: Rectangle {
                        radius: 8
                        color: (agentClient && agentClient.activeProfile === "zci-command") ? "#1d3b5c" : "#131c2e"
                        border.color: (agentClient && agentClient.activeProfile === "zci-command") ? accentCyan : glassBorder
                    }
                    contentItem: Text {
                        text: parent.text
                        color: textPrimary
                        font.pixelSize: 11
                        font.bold: true
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        leftPadding: 10
                        rightPadding: 10
                    }
                }
            }

            Button {
                objectName: "refreshStatusBtn"
                text: "Yenile"
                onClicked: if (agentClient) agentClient.refreshStatus()
                background: Rectangle {
                    radius: 8
                    color: "#1b2942"
                    border.color: glassBorder
                }
                contentItem: Text {
                    text: parent.text
                    color: textPrimary
                    font.pixelSize: 11
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    leftPadding: 10
                    rightPadding: 10
                }
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 10

        // Security posture badge bar
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Repeater {
                model: [
                    { idName: "securityBadge_noShell", label: "Ham Kabuk Kapalı (No sh -c)", ok: agentClient ? !agentClient.rawShellEnabled : true },
                    { idName: "securityBadge_loopback", label: "Loopback-Only · Sıfır Dış Ağ", ok: agentClient ? agentClient.loopbackOnly : true },
                    { idName: "securityBadge_recoveryLock", label: "Kurtarma Kilitleri (rollback/repair=false)", ok: agentClient ? (agentClient.rollbackLocked && agentClient.repairLocked) : true },
                    { idName: "securityBadge_auditChain", label: agentClient ? ("Hash-Zincirli 0600 Denetim #" + agentClient.auditEntryCount + " (" + agentClient.auditHeadShort + ")") : "Hash-Zincirli 0600", ok: agentClient ? (agentClient.auditChainValid && agentClient.auditPermissionsStrict) : true }
                ]
                delegate: Rectangle {
                    required property var modelData
                    objectName: modelData.idName
                    radius: 7
                    color: modelData.ok ? "#102430" : "#31151d"
                    border.color: modelData.ok ? "#1f4d5e" : accentRed
                    border.width: 1
                    implicitHeight: 28
                    implicitWidth: badgeTxt.implicitWidth + 16
                    Text {
                        id: badgeTxt
                        anchors.centerIn: parent
                        text: modelData.label
                        color: modelData.ok ? accentTeal : accentRed
                        font.pixelSize: 11
                        font.bold: true
                    }
                }
            }
            Item { Layout.fillWidth: true }
        }

        // Error banner
        Rectangle {
            objectName: "errorBannerBox"
            visible: agentClient && agentClient.errorBanner.length > 0
            Layout.fillWidth: true
            implicitHeight: errTxt.implicitHeight + 18
            radius: 10
            color: "#33161e"
            border.color: accentRed
            border.width: 1
            RowLayout {
                anchors.fill: parent
                anchors.margins: 9
                Text {
                    id: errTxt
                    Layout.fillWidth: true
                    text: agentClient ? agentClient.errorBanner : ""
                    color: "#fecaca"
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                }
                Button {
                    text: "Kapat"
                    onClicked: if (agentClient) agentClient.clearError()
                    background: Rectangle { radius: 6; color: "#4c1d28" }
                    contentItem: Text { text: parent.text; color: textPrimary; font.pixelSize: 11; leftPadding: 8; rightPadding: 8 }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 12

            // Left sidebar: Plans + Immutable Audit Trail
            Rectangle {
                Layout.preferredWidth: 310
                Layout.fillHeight: true
                radius: 12
                color: glassPanel
                border.color: glassBorder
                border.width: 1

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 8

                    Text {
                        text: "Yerel Planlar"
                        color: textPrimary
                        font.pixelSize: 13
                        font.bold: true
                    }

                    ListView {
                        objectName: "plansListView"
                        Layout.fillWidth: true
                        Layout.preferredHeight: 210
                        clip: true
                        spacing: 6
                        model: agentClient ? agentClient.plans : []
                        delegate: Rectangle {
                            required property var modelData
                            width: ListView.view.width
                            height: 56
                            radius: 8
                            color: (agentClient && agentClient.activePlanId === modelData.plan_id) ? "#1f3250" : glassCard
                            border.color: (agentClient && agentClient.activePlanId === modelData.plan_id) ? accentCyan : glassBorder
                            border.width: 1

                            MouseArea {
                                anchors.fill: parent
                                onClicked: if (agentClient) agentClient.selectPlan(modelData.plan_id)
                            }

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 8
                                spacing: 2
                                Text {
                                    text: modelData.goal || modelData.summary || modelData.plan_id
                                    color: textPrimary
                                    font.pixelSize: 11
                                    font.bold: true
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Text {
                                        text: (modelData.agent_profile || "") + " · " + (modelData.step_count || 0) + " adım"
                                        color: textSecondary
                                        font.pixelSize: 10
                                    }
                                    Item { Layout.fillWidth: true }
                                    Text {
                                        text: modelData.status || ""
                                        color: statusColor(modelData.status || "")
                                        font.pixelSize: 10
                                        font.bold: true
                                    }
                                }
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: glassBorder
                    }

                    Text {
                        text: "Değiştirilemez Denetim İzleri (Hash-Zinciri)"
                        color: textPrimary
                        font.pixelSize: 12
                        font.bold: true
                    }

                    ListView {
                        objectName: "auditTrailList"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        spacing: 5
                        model: agentClient ? agentClient.auditEntries : []
                        delegate: Rectangle {
                            required property var modelData
                            width: ListView.view.width
                            implicitHeight: audCol.implicitHeight + 10
                            radius: 7
                            color: "#111a2b"
                            border.color: "#1f2e47"
                            border.width: 1

                            ColumnLayout {
                                id: audCol
                                anchors.fill: parent
                                anchors.margins: 6
                                spacing: 2
                                RowLayout {
                                    Layout.fillWidth: true
                                    Text {
                                        text: "#" + (modelData.seq || 0) + " " + (modelData.event_type || "")
                                        color: accentCyan
                                        font.pixelSize: 10
                                        font.bold: true
                                    }
                                    Item { Layout.fillWidth: true }
                                    Text {
                                        text: (modelData.entry_sha256 || "").substring(0, 10)
                                        color: textSecondary
                                        font.pixelSize: 9
                                        font.family: "monospace"
                                    }
                                }
                                Text {
                                    text: (modelData.outcome || "") + " · " + (modelData.detail || "")
                                    color: textSecondary
                                    font.pixelSize: 10
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                            }
                        }
                    }
                }
            }

            // Right main panel: Planner Composer + Snapper Preflight + Step Approval Cards
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                radius: 12
                color: glassPanel
                border.color: glassBorder
                border.width: 1

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 10

                    // Goal composer
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        TextField {
                            id: workspaceField
                            objectName: "workspaceInput"
                            Layout.preferredWidth: 230
                            placeholderText: "Çalışma alanı dizini..."
                            text: agentClient ? agentClient.workspaceDir : ""
                            color: textPrimary
                            placeholderTextColor: textSecondary
                            onTextEdited: if (agentClient) agentClient.workspaceDir = text
                            background: Rectangle {
                                radius: 8
                                color: "#0f172a"
                                border.color: glassBorder
                            }
                        }
                        TextField {
                            id: goalField
                            objectName: "goalInput"
                            Layout.fillWidth: true
                            placeholderText: "Doğal dil görevi yazın (örn: Çalışma alanını analiz et veya onaylı rapor oluştur)..."
                            color: textPrimary
                            placeholderTextColor: textSecondary
                            onAccepted: {
                                if (agentClient) {
                                    agentClient.proposePlan(text)
                                    text = ""
                                }
                            }
                            background: Rectangle {
                                radius: 8
                                color: "#0f172a"
                                border.color: glassBorder
                            }
                        }
                        Button {
                            objectName: "proposePlanBtn"
                            text: (agentClient && agentClient.busy) ? "Planlanıyor..." : "Plan Öner"
                            enabled: !(agentClient && agentClient.busy)
                            onClicked: {
                                if (agentClient) {
                                    agentClient.proposePlan(goalField.text)
                                    goalField.text = ""
                                }
                            }
                            background: Rectangle {
                                radius: 8
                                color: "#0284c7"
                            }
                            contentItem: Text {
                                text: parent.text
                                color: "#ffffff"
                                font.pixelSize: 12
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                                leftPadding: 14
                                rightPadding: 14
                            }
                        }
                    }

                    // Snapper & Recovery preflight banner (13.03)
                    Rectangle {
                        objectName: "snapperBanner"
                        visible: agentClient && agentClient.snapperSummary.length > 0
                        Layout.fillWidth: true
                        implicitHeight: snapTxt.implicitHeight + 14
                        radius: 8
                        color: "#10272a"
                        border.color: "#1e5b5c"
                        border.width: 1
                        Text {
                            id: snapTxt
                            anchors.fill: parent
                            anchors.margins: 7
                            text: "Snapper / Kurtarma Ön-Kontrol: " + (agentClient ? agentClient.snapperSummary : "")
                            color: accentTeal
                            font.pixelSize: 11
                            wrapMode: Text.Wrap
                        }
                    }

                    // Active Plan header & action buttons
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Text {
                                text: (agentClient && agentClient.activePlanSummary.length > 0)
                                      ? agentClient.activePlanSummary
                                      : "Henüz seçili bir eylem planı yok."
                                color: textPrimary
                                font.pixelSize: 14
                                font.bold: true
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                            Text {
                                text: (agentClient && agentClient.activePlanId.length > 0)
                                      ? ("Plan: " + agentClient.activePlanId + " · Durum: " + agentClient.activePlanStatus + " · executable=false · authority=none")
                                      : "Tüm önerilen adımlar salt-veri zarfında değerlendirilir; yazma adımları kullanıcı onayı olmadan yürütülmez."
                                color: textSecondary
                                font.pixelSize: 11
                            }
                        }
                        Button {
                            objectName: "executePlanBtn"
                            text: "Onaylı Planı Yürüt"
                            enabled: agentClient && agentClient.activePlanId.length > 0 && !agentClient.busy
                            onClicked: if (agentClient) agentClient.executeApprovedPlan()
                            background: Rectangle {
                                radius: 8
                                color: parent.enabled ? "#0d9488" : "#1e293b"
                            }
                            contentItem: Text {
                                text: parent.text
                                color: "#ffffff"
                                font.pixelSize: 12
                                font.bold: true
                                leftPadding: 12
                                rightPadding: 12
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                        Button {
                            objectName: "cancelPlanBtn"
                            text: "İptal Et"
                            enabled: agentClient && agentClient.activePlanId.length > 0 && !agentClient.busy
                            onClicked: if (agentClient) agentClient.cancelActivePlan("Kullanıcı iptal etti")
                            background: Rectangle {
                                radius: 8
                                color: parent.enabled ? "#7f1d1d" : "#1e293b"
                            }
                            contentItem: Text {
                                text: parent.text
                                color: "#ffffff"
                                font.pixelSize: 12
                                leftPadding: 10
                                rightPadding: 10
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                    }

                    // Step cards list
                    ListView {
                        objectName: "stepsListView"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        spacing: 10
                        model: agentClient ? agentClient.steps : []
                        delegate: Rectangle {
                            required property var modelData
                            width: ListView.view.width
                            implicitHeight: stepCol.implicitHeight + 20
                            radius: 10
                            color: glassCard
                            border.color: riskColor(modelData.risk_level || "ReadOnly")
                            border.width: 1

                            ColumnLayout {
                                id: stepCol
                                anchors.fill: parent
                                anchors.margins: 10
                                spacing: 6

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8
                                    Text {
                                        text: (modelData.step_id || "step") + " · " + (modelData.action_kind || "")
                                        color: textPrimary
                                        font.pixelSize: 13
                                        font.bold: true
                                    }
                                    Rectangle {
                                        objectName: "riskBadge_" + (modelData.risk_level || "ReadOnly")
                                        radius: 6
                                        color: "#0f172a"
                                        border.color: riskColor(modelData.risk_level || "ReadOnly")
                                        border.width: 1
                                        implicitHeight: 22
                                        implicitWidth: rTxt.implicitWidth + 12
                                        Text {
                                            id: rTxt
                                            anchors.centerIn: parent
                                            text: modelData.risk_level || "ReadOnly"
                                            color: riskColor(modelData.risk_level || "ReadOnly")
                                            font.pixelSize: 10
                                            font.bold: true
                                        }
                                    }
                                    Item { Layout.fillWidth: true }
                                    Text {
                                        text: "Onay: " + (modelData.approval_status || "") + " · Yürütme: " + (modelData.execution_status || "")
                                        color: statusColor(modelData.execution_status === "planned" ? (modelData.approval_status || "") : (modelData.execution_status || ""))
                                        font.pixelSize: 11
                                        font.bold: true
                                    }
                                }

                                Text {
                                    text: modelData.description || ""
                                    color: textPrimary
                                    font.pixelSize: 12
                                    wrapMode: Text.Wrap
                                    Layout.fillWidth: true
                                }

                                Text {
                                    text: "Hedef: " + (modelData.target || "-") + " · Ajan: " + (modelData.agent_id || "")
                                    color: textSecondary
                                    font.pixelSize: 11
                                    font.family: "monospace"
                                    elide: Text.ElideMiddle
                                    Layout.fillWidth: true
                                }

                                Text {
                                    visible: (modelData.output_preview || "").length > 0
                                    text: "Sonuç: " + (modelData.output_preview || "")
                                    color: accentTeal
                                    font.pixelSize: 11
                                    wrapMode: Text.Wrap
                                    Layout.fillWidth: true
                                }

                                RowLayout {
                                    visible: Boolean(modelData.requires_approval) && modelData.approval_status === "pending"
                                    Layout.fillWidth: true
                                    spacing: 8
                                    Button {
                                        objectName: "approveBtn_" + (modelData.step_id || "")
                                        text: "Adımı Onayla"
                                        onClicked: if (agentClient) agentClient.approveStep(modelData.step_id, true, "Kullanıcı onayı")
                                        background: Rectangle { radius: 6; color: "#15803d" }
                                        contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 11; font.bold: true; leftPadding: 10; rightPadding: 10 }
                                    }
                                    Button {
                                        objectName: "rejectBtn_" + (modelData.step_id || "")
                                        text: "Adımı Reddet"
                                        onClicked: if (agentClient) agentClient.approveStep(modelData.step_id, false, "Kullanıcı reddetti")
                                        background: Rectangle { radius: 6; color: "#991b1b" }
                                        contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 11; font.bold: true; leftPadding: 10; rightPadding: 10 }
                                    }
                                    Item { Layout.fillWidth: true }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
