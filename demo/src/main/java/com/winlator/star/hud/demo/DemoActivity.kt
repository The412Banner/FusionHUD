package com.winlator.star.hud.demo

import android.graphics.Canvas
import android.graphics.Color
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.Shader
import android.os.Bundle
import android.os.SystemClock
import android.view.Choreographer
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import androidx.appcompat.app.AppCompatActivity
import com.winlator.star.container.Container
import com.winlator.star.core.GPUInformation
import com.winlator.star.ui.theme.AppThemeState
import com.winlator.star.widget.FpsCounter
import com.winlator.star.widget.exportHudDiagnostics
import com.winlator.star.widget.fusionhud.FusionHdr
import com.winlator.star.widget.fusionhud.FusionHudView
import com.winlator.star.widget.fusionhud.FusionSize

/**
 * Standalone demo: a fake "game" (an animated gradient surface) with a live FusionHUD overlay on top,
 * proving the HUD library renders and its metrics populate with no emulator, no native blob, no Compose.
 *
 * A single shared [FpsCounter] is ticked once per presented frame via [Choreographer] — exactly how the
 * real X-server epoll thread feeds it in Bannerlator — so the FPS number is genuine (tracks the display
 * refresh). [FusionHudView] self-refreshes its metric rows on its own ~1 Hz coroutine (started in
 * onAttachedToWindow), reading this device's real procfs/sysfs via its internal HudMetrics.
 *
 * Buttons cycle the Fusion SIZE (Full/Tiles/Pill/Minimal/Mega), the accent colour, the lock state, and
 * export a HUD diagnostics report — the same surfaces an integrator would wire. A second row shows the
 * readouts a host sets itself (all off by default): frames reaching the screen when something adds or
 * drops frames after the game (setPresentedFps), the display server (setDisplayServer), the HDR state
 * (setHdrState) and a GPU name reported to games instead of the real one (setGpuSpoofName).
 */
class DemoActivity : AppCompatActivity() {

    private val fpsCounter = FpsCounter()

    private lateinit var overlayContainer: FrameLayout
    private lateinit var background: GameBackgroundView
    private lateinit var hud: FusionHudView
    private lateinit var sizeButton: Button
    private lateinit var lockButton: Button
    private lateinit var frameGenButton: Button
    private lateinit var displayButton: Button
    private lateinit var hdrButton: Button
    private lateinit var gpuNameButton: Button

    private val accents = listOf(0xFFA374FF.toInt(), 0xFF00E5FF.toInt(), 0xFFFF5252.toInt(), 0xFF69F0AE.toInt())

    private var sizeIndex = 0
    private var accentIndex = 0
    private var locked = false

    // Shown frames per game frame; 0 = nothing changes the frame count after the game.
    private val frameGenModes = listOf("off" to 0f, "×2" to 2f, "×3" to 3f, "frames lost" to 0.5f)
    private val displayServers = listOf("off" to "", "X11" to "X11", "Wayland" to "Wayland")
    private val hdrStates = listOf(
        "off" to FusionHdr.NONE, "on" to FusionHdr.ON, "no headroom" to FusionHdr.NO_HEADROOM,
        "switched off" to FusionHdr.OFF, "ready" to FusionHdr.READY, "tone-mapped" to FusionHdr.TONEMAPPED,
        "not on this screen" to FusionHdr.NOT_ON_THIS_SCREEN
    )
    private val spoofGpuName = "AMD Radeon RX 6800"
    private var frameGenIndex = 0
    private var displayIndex = 0
    private var hdrIndex = 0
    private var gpuSpoofed = false

    // Choreographer drives both the fake game's animation and the authoritative FPS tick.
    private val frameCallback = object : Choreographer.FrameCallback {
        override fun doFrame(frameTimeNanos: Long) {
            fpsCounter.tick()
            // A host with frame generation reports what reaches the panel; the demo fakes it from its own rate.
            hud.setPresentedFps(fpsCounter.currentFPS * frameGenModes[frameGenIndex].second)
            background.invalidate()
            Choreographer.getInstance().postFrameCallback(this)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // No native libwinlator here: hand the HUD renderer/Vulkan strings so its labels + diagnostics
        // show real-looking values standalone. (Otherwise getRenderer falls back to a GLES probe.)
        GPUInformation.setRenderer("FusionHUD Demo GPU (Adreno 750)")
        GPUInformation.setVulkanVersion("1.3.274")
        AppThemeState.setCurrentAccentArgb(accents[accentIndex])

        val root = FrameLayout(this)

        background = GameBackgroundView(this)
        root.addView(
            background,
            FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT
            )
        )

        overlayContainer = FrameLayout(this)
        root.addView(
            overlayContainer,
            FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply {
                gravity = Gravity.TOP or Gravity.START
                topMargin = dp(24)
                leftMargin = dp(24)
            }
        )

        hud = FusionHudView(this).apply {
            setFpsCounter(fpsCounter)
            setEngineLabel("DXVK 2.4")
            setGpuModel("Adreno 750")
            setWineVersion("Proton 10.0-4")
            setGraphicsWrapper("FusionHUD Demo")
            // Keep the size button + lock state in sync when the user taps/long-presses the HUD itself.
            setOnSizeCycledListener { token ->
                sizeIndex = FusionSize.values().indexOf(FusionSize.from(token)).coerceAtLeast(0)
                runOnUiThread { sizeButton.text = "Size: " + FusionSize.values()[sizeIndex].label }
            }
            setOnLockChangedListener { isLocked ->
                locked = isLocked
                runOnUiThread { lockButton.text = if (locked) "Lock: on" else "Lock: off" }
            }
        }
        overlayContainer.addView(hud)

        root.addView(buildControlBar())

        setContentView(root)
        applyConfig()
    }

    override fun onResume() {
        super.onResume()
        Choreographer.getInstance().postFrameCallback(frameCallback)
    }

    override fun onPause() {
        super.onPause()
        Choreographer.getInstance().removeFrameCallback(frameCallback)
    }

    private fun buildControlBar(): View {
        val bar = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_HORIZONTAL
            setPadding(dp(8), dp(8), dp(8), dp(8))
        }
        val row1 = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        val row2 = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        sizeButton = button("Size: " + FusionSize.values()[sizeIndex].label) {
            sizeIndex = (sizeIndex + 1) % FusionSize.values().size
            sizeButton.text = "Size: " + FusionSize.values()[sizeIndex].label
            applyConfig()
        }
        lockButton = button("Lock: off") {
            locked = !locked
            lockButton.text = if (locked) "Lock: on" else "Lock: off"
            applyConfig()
        }
        val accentButton = button("Accent") {
            accentIndex = (accentIndex + 1) % accents.size
            AppThemeState.setCurrentAccentArgb(accents[accentIndex])
            applyConfig() // nudge a rebuild so the new accent is picked up immediately
        }
        val diagButton = button("Diagnostics") {
            // Standalone defaults: writes to the app-specific external dir (no permission), no share sheet.
            exportHudDiagnostics(this)
        }
        row1.addView(sizeButton); row1.addView(lockButton); row1.addView(accentButton); row1.addView(diagButton)

        frameGenButton = button("Frame gen: " + frameGenModes[frameGenIndex].first) {
            frameGenIndex = (frameGenIndex + 1) % frameGenModes.size
            frameGenButton.text = "Frame gen: " + frameGenModes[frameGenIndex].first
        }
        displayButton = button("Display: " + displayServers[displayIndex].first) {
            displayIndex = (displayIndex + 1) % displayServers.size
            displayButton.text = "Display: " + displayServers[displayIndex].first
            hud.setDisplayServer(displayServers[displayIndex].second)
        }
        hdrButton = button("HDR: " + hdrStates[hdrIndex].first) {
            hdrIndex = (hdrIndex + 1) % hdrStates.size
            hdrButton.text = "HDR: " + hdrStates[hdrIndex].first
            hud.setHdrState(hdrStates[hdrIndex].second)
        }
        gpuNameButton = button("GPU name: real") {
            gpuSpoofed = !gpuSpoofed
            gpuNameButton.text = if (gpuSpoofed) "GPU name: spoofed" else "GPU name: real"
            hud.setGpuSpoofName(if (gpuSpoofed) spoofGpuName else null)
        }
        row2.addView(frameGenButton); row2.addView(displayButton); row2.addView(hdrButton); row2.addView(gpuNameButton)
        bar.addView(row1); bar.addView(row2)

        // A full-size holder so the (wrap-content) button bar pins to the bottom-centre of the root.
        return FrameLayout(this).apply {
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT
            )
            addView(
                bar,
                FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT
                ).apply { gravity = Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL }
            )
        }
    }

    /** Build the shared KV config for the current size/lock and push it to the HUD. */
    private fun applyConfig() {
        val cfg = Container.DEFAULT_FPS_COUNTER_CONFIG
            .replace(Regex("hudSize=[^,]*"), "hudSize=" + FusionSize.values()[sizeIndex].token)
            .replace(Regex("hudLocked=[^,]*"), "hudLocked=" + if (locked) "1" else "0")
        hud.applyConfig(cfg)
    }

    private fun button(text: String, onClick: () -> Unit): Button =
        Button(this).apply {
            this.text = text
            setOnClickListener { onClick() }
        }

    private fun dp(v: Int): Int = Math.round(v * resources.displayMetrics.density)

    /** A cheap fake "game": a gradient whose hue sweeps over time, redrawn every frame. */
    private class GameBackgroundView(context: android.content.Context) : View(context) {
        private val paint = Paint()
        private val start = SystemClock.elapsedRealtime()
        override fun onDraw(canvas: Canvas) {
            val t = (SystemClock.elapsedRealtime() - start) / 1000f
            val hue = (t * 30f) % 360f
            val c1 = Color.HSVToColor(floatArrayOf(hue, 0.7f, 0.35f))
            val c2 = Color.HSVToColor(floatArrayOf((hue + 90f) % 360f, 0.7f, 0.6f))
            paint.shader = LinearGradient(
                0f, 0f, width.toFloat(), height.toFloat(), c1, c2, Shader.TileMode.CLAMP
            )
            canvas.drawRect(0f, 0f, width.toFloat(), height.toFloat(), paint)
        }
    }
}
