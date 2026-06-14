/*
 * InfoTip — a small "(i)" help icon that reveals its `text` on hover.
 *
 * Use it to keep exposition out of the page body: pair it with a section
 * title or a control instead of a wrapping paragraph of helper text. Sibling
 * pages in this KCM's `ui/` module reference it by type name (`InfoTip { ... }`).
 */
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.Icon {
    id: tip

    /// The help text shown on hover. When empty, the icon hides itself.
    property string text: ""

    source: "help-contextual"
    implicitWidth: Kirigami.Units.iconSizes.small
    implicitHeight: Kirigami.Units.iconSizes.small
    Layout.preferredWidth: Kirigami.Units.iconSizes.small
    Layout.preferredHeight: Kirigami.Units.iconSizes.small
    visible: tip.text.length > 0
    // Surface the help text to assistive tech, not just sighted hover users.
    Accessible.role: Accessible.StaticText
    Accessible.name: tip.text

    HoverHandler { id: tipHover }
    QQC2.ToolTip.visible: tipHover.hovered && tip.text.length > 0
    QQC2.ToolTip.text: tip.text
}
