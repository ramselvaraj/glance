import QtQuick

Item {
    id: control

    required property Flickable flickable
    property real pixelFactor: 5.0
    property real notchPixels: 120.0
    property real decay: 0.95

    anchors.fill: parent

    WheelHandler {
        target: null
        blocking: true
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: (event) => {
            momentum.stop()

            let dx = event.pixelDelta.x * control.pixelFactor
            let dy = event.pixelDelta.y * control.pixelFactor
            if (dx === 0 && dy === 0) {
                dx = event.angleDelta.x / 120 * control.notchPixels
                dy = event.angleDelta.y / 120 * control.notchPixels
            }

            const maxX = Math.max(0, control.flickable.contentWidth - control.flickable.width)
            const maxY = Math.max(0, control.flickable.contentHeight - control.flickable.height)
            control.flickable.contentX = Math.max(0, Math.min(control.flickable.contentX - dx, maxX))
            control.flickable.contentY = Math.max(0, Math.min(control.flickable.contentY - dy, maxY))

            if (event.pixelDelta.x !== 0 || event.pixelDelta.y !== 0) {
                momentum.velocityX = momentum.velocityX * 0.4 - dx * 0.6
                momentum.velocityY = momentum.velocityY * 0.4 - dy * 0.6
                endDelay.restart()
                if (event.phase === Qt.ScrollEnd) {
                    endDelay.stop()
                    momentum.start()
                }
            }
        }
    }

    Timer {
        id: endDelay
        interval: 55
        onTriggered: momentum.start()
    }

    Timer {
        id: momentum
        interval: 16
        repeat: true
        property real velocityX: 0
        property real velocityY: 0

        function start() {
            if (Math.abs(velocityX) >= 0.5 || Math.abs(velocityY) >= 0.5)
                restart()
        }

        onTriggered: {
            const maxX = Math.max(0, control.flickable.contentWidth - control.flickable.width)
            const maxY = Math.max(0, control.flickable.contentHeight - control.flickable.height)
            const nextX = Math.max(0, Math.min(control.flickable.contentX + velocityX, maxX))
            const nextY = Math.max(0, Math.min(control.flickable.contentY + velocityY, maxY))
            const hitX = nextX === control.flickable.contentX && velocityX !== 0
            const hitY = nextY === control.flickable.contentY && velocityY !== 0
            control.flickable.contentX = nextX
            control.flickable.contentY = nextY
            velocityX = hitX ? 0 : velocityX * control.decay
            velocityY = hitY ? 0 : velocityY * control.decay
            if (Math.abs(velocityX) < 0.5 && Math.abs(velocityY) < 0.5)
                stop()
        }
    }
}
