import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Item {
    id: page
    objectName: "appearancePage"

    implicitWidth: form.implicitWidth
    implicitHeight: form.implicitHeight + 24

    property alias cfg_backgroundOpacity: opacitySlider.value
    property int cfg_backgroundOpacityDefault: 60

    Kirigami.FormLayout {
        id: form
        anchors.top: parent.top
        anchors.topMargin: 24
        anchors.left: parent.left
        anchors.right: parent.right

        RowLayout {
            Kirigami.FormData.label: "Zemin opaklığı:"
            Layout.fillWidth: true

            Controls.Slider {
                id: opacitySlider
                objectName: "opacitySlider"
                Layout.fillWidth: true
                Layout.preferredWidth: 240
                from: 0
                to: 100
                stepSize: 1
                snapMode: Controls.Slider.SnapAlways
                value: 60
                Accessible.name: "Zemin opaklığı"
            }

            Controls.Label {
                objectName: "opacityValueLabel"
                Layout.minimumWidth: 48
                text: Math.round(opacitySlider.value) + " %"
                horizontalAlignment: Text.AlignRight
            }
        }

        Controls.Label {
            Layout.fillWidth: true
            text: "%0 tamamen saydam, %100 tamamen opak. Yazılar ve göstergeler etkilenmez."
            wrapMode: Text.WordWrap
        }
    }
}
