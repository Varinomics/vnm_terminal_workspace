import QtQuick

Item {
    id: root

    objectName: "vnm_terminal_workspace_root"
    focus: true

    property string terminalTitle: ""
    property int terminalStyle: 1
    property bool timestampVisible: false
    property string timestampText: ""
    property real timestampX: 0
    property real timestampY: 0

    Rectangle {
        id: timestampTooltip

        visible: root.timestampVisible
        x: Math.min(
            root.width - width,
            Math.max(0, root.timestampX + 8))
        y: Math.min(
            root.height - height,
            Math.max(0, root.timestampY + 8))
        width: timestampLabel.implicitWidth + 12
        height: timestampLabel.implicitHeight + 8
        color: root.terminalStyle === 1 ? "#202124" : "#f2f2f2"
        radius: 3
        z: 2

        Text {
            id: timestampLabel

            anchors.centerIn: parent
            text: root.timestampText
            color: root.terminalStyle === 1 ? "#f4f4f4" : "#202124"
        }
    }
}
