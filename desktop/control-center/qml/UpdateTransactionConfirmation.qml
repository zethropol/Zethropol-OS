import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: root
    required property var updateBridge
    property string reviewedHash: ""
    property string reviewedSources: ""
    title: "Confirm Selected Sources"
    modal: true
    onOpened: {
        reviewedHash = updateBridge.planHash
        reviewedSources = updateBridge.reviewHash
        skipConsent.checked = false
        removalConsent.checked = false
    }
    onAccepted: {
        if (!updateBridge.previewReady || !updateBridge.applicableSources || reviewedSources.length === 0
            || reviewedSources !== updateBridge.reviewHash || reviewedHash !== updateBridge.planHash)
            return
        updateBridge.confirmRemovals(reviewedHash, removalConsent.checked)
        updateBridge.confirmSkippedSources(reviewedSources, skipConsent.checked)
        updateBridge.install()
    }
    onRejected: updateBridge.confirmRemovals(reviewedHash, false)

    contentItem: ColumnLayout {
        spacing: 10
        Label {
            text: "Selected sources run in separate stages. Official repositories use the full authenticated transaction and required recovery checks. External stages are never represented as part of the official commit. Review every blocker below."
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        CheckBox {
            id: removalConsent
            objectName: "replacementRemovalConsent"
            visible: root.updateBridge.removalApprovalRequired
            checked: false
            text: "I explicitly approve every removal and replacement listed in this plan."
            Layout.fillWidth: true
        }
        CheckBox {
            id: skipConsent
            objectName: "blockedSourceConsent"
            visible: root.updateBridge.blockedSources
            text: "I acknowledge blocked sources will NOT be updated. Continue only with supported stages."
            Layout.fillWidth: true
        }
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            TextArea {
                text: root.updateBridge.planPreview
                readOnly: true
                textFormat: TextEdit.PlainText
                wrapMode: TextEdit.Wrap
            }
        }
    }
    footer: DialogButtonBox {
        standardButtons: Dialog.Cancel
        onAccepted: root.accept()
        onRejected: root.reject()
        Button {
            objectName: "confirmOfficialTransaction"
            text: "Apply Reviewed Sources"
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            enabled: root.updateBridge.previewReady && root.updateBridge.applicableSources && root.reviewedSources.length > 0
                && root.reviewedSources === root.updateBridge.reviewHash
                && root.reviewedHash === root.updateBridge.planHash
                && (!root.updateBridge.removalApprovalRequired || removalConsent.checked)
                && (!root.updateBridge.blockedSources || skipConsent.checked)
        }
    }
}
