// HelpSystem element: a centered row of button hints (button glyph + action label), ES-DE style.
import QtQuick
import "../Theme.js" as T

Item {
    property var el: ({})
    property var ctx: ({})
    property var host
    // Key/controller hints mean nothing on a touchscreen — hide the bar entirely on Mobile.
    // TV and desktop keep it (their input IS the keys/pad it describes).
    visible: !((typeof form !== "undefined") && form && form.mode === "mobile")
    property color fg: T.val(el, "color", "#FFFFFF")
    property color chip: Qt.rgba(fg.r, fg.g, fg.b, 0.16) // a tint of the text colour, so it reads on any bg
    property real fs: Number(T.val(el, "fontSize", 0.024)) * (host ? host.height : 720)
    // Optional `outline`, as on the Text element: a contrasting halo for a bar drawn over a background whose
    // brightness cannot be derived (an image). Absent on every hand-written theme -> Text.Normal, no change.
    readonly property string outlineColor: T.val(el, "outline", "")
    readonly property int textStyle: outlineColor !== "" ? Text.Outline : Text.Normal
    readonly property color textStyleColor: outlineColor !== "" ? outlineColor : "transparent"
    // Is a CONTROLLER driving right now? Reading `input.mode` here (rather than calling chipFor and hoping) is
    // what subscribes every chip below to InputMode::changed() -- a QML binding subscribes to a NOTIFY signal by
    // READING a property, and a plain function call subscribes to nothing, so a chip that only called chipFor
    // would be spelled once at load and never again. Via this one property, a brand change or a remap re-spells
    // the whole bar. typeof-guarded like the `form` read above, so a surface whose context has no `input`
    // (a fixture, a host that never registered it) renders exactly as it did before this existed.
    readonly property bool padMode: (typeof input !== "undefined") && input && input.mode === "pad"

    Row {
        id: hintRow
        anchors.centerIn: parent
        spacing: Math.max(10, fs * 0.9)
        // A phone is narrower than the hint row at TV scale — shrink the whole row uniformly to fit
        // (centered), instead of clipping hints off both edges.
        scale: Math.min(1, parent.width / Math.max(1, implicitWidth + 16))
        Repeater {
            model: el.entries ? el.entries : []
            delegate: Row {
                required property var modelData
                spacing: 6
                anchors.verticalCenter: parent.verticalCenter
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    radius: 5
                    color: chip
                    height: Math.max(1, fs * 1.25)
                    width: Math.max(height, btn.implicitWidth + 12)
                    Text {
                        id: btn; anchors.centerIn: parent
                        // While a pad is driving, the button the player is looking at replaces the key they
                        // would have typed. A hint InputMode does not own comes back unchanged, so a
                        // third-party theme's own chip stays the author's text.
                        text: padMode ? input.chipFor(modelData.button ? modelData.button : "")
                                      : (modelData.button ? modelData.button : "")
                        color: fg; font.pixelSize: fs * 0.85; font.bold: true
                        style: textStyle; styleColor: textStyleColor
                    }
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: modelData.label ? modelData.label : ""
                    color: fg; opacity: 0.85; font.pixelSize: fs
                    style: textStyle; styleColor: textStyleColor
                }
            }
        }
    }
}
