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
import com.winlator.star.widget.FrameRating
import com.winlator.star.widget.FrameRatingHorizontal
import com.winlator.star.widget.PerfHudView
import com.winlator.star.widget.fusionhud.FusionHudView
import com.winlator.star.widget.perfhud.PerformanceHudView

/**
 * Standalone demo: a fake "game" (an animated gradient surface) with a live FusionHUD overlay on top,
 * proving the HUD library renders and its metrics populate with no emulator, no native blob, no Compose.
 *
 * A single shared [FpsCounter] is ticked once per presented frame via [Choreographer] — exactly how the
 * real X-server epoll thread feeds it in Bannerlator — so the FPS number is genuine (tracks the display
 * refresh). Every overlay reads that same counter; the metric rows (CPU/GPU/RAM/temps/battery) come from
 * each overlay's own [com.winlator.star.widget.HudMetrics], reading this device's real procfs/sysfs.
 *
 * Buttons cycle the overlay STYLE (all shipped view classes), the Fusion SIZE, the accent colour, and
 * the lock state — the same listeners an integrator would wire.
 */
class DemoActivity : AppCompatActivity() {

    private val fpsCounter = FpsCounter()

    private lateinit var overlayContainer: FrameLayout
    private lateinit var background: GameBackgroundView
    private lateinit var styleButton: Button
    private lateinit var sizeButton: Button
    private lateinit var lockButton: Button

    private var currentOverlay: View? = null

    private val styles = listOf(
        "Fusion", "Performance", "PerfHud (classic)", "Classic vertical", "Classic horizontal"
    )
    private val sizeTokens = listOf("full", "tiles", "pill", "minimal", "mega")
    private val accents = listOf(0xFFA374FF.toInt(), 0xFF00E5FF.toInt(), 0xFFFF5252.toInt(), 0xFF69F0AE.toInt())

    private var styleIndex = 0
    private var sizeIndex = 0
    private var accentIndex = 0
    private var locked = false

    // Choreographer drives both the fake game's animation and the authoritative FPS tick.
    private val frameCallback = object : Choreographer.FrameCallback {
        override fun doFrame(frameTimeNanos: Long) {
            fpsCounter.tick()
            background.invalidate()
            Choreographer.getInstance().postFrameCallback(this)
        }
    }

    // Classic overlays are NOT self-refreshing (no coroutine); they expose update(). Tick them at ~1 Hz.
    private val metricTicker = object : Runnable {
        override fun run() {
            when (val v = currentOverlay) {
                is FrameRating -> v.update()
                is FrameRatingHorizontal -> v.update()
                is PerfHudView -> v.update()
                else -> { /* Fusion / Performance self-refresh via their own coroutine */ }
            }
            overlayContainer.postDelayed(this, 1000)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // Give classic FrameRating a renderer string to show even though there is no native libwinlator.
        GPUInformation.setRenderer("FusionHUD Demo GPU")
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

        root.addView(buildControlBar())

        setContentView(root)
        showStyle(styleIndex)
    }

    override fun onResume() {
        super.onResume()
        Choreographer.getInstance().postFrameCallback(frameCallback)
        overlayContainer.postDelayed(metricTicker, 1000)
    }

    override fun onPause() {
        super.onPause()
        Choreographer.getInstance().removeFrameCallback(frameCallback)
        overlayContainer.removeCallbacks(metricTicker)
    }

    private fun buildControlBar(): View {
        val bar = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            setPadding(dp(8), dp(8), dp(8), dp(8))
        }
        styleButton = button("Style") { styleIndex = (styleIndex + 1) % styles.size; showStyle(styleIndex) }
        sizeButton = button("Size") { sizeIndex = (sizeIndex + 1) % sizeTokens.size; applyConfigToCurrent() }
        lockButton = button("Lock: off") {
            locked = !locked
            lockButton.text = if (locked) "Lock: on" else "Lock: off"
            applyConfigToCurrent()
        }
        val accentButton = button("Accent") {
            accentIndex = (accentIndex + 1) % accents.size
            AppThemeState.setCurrentAccentArgb(accents[accentIndex])
            // Re-apply so the classic overlays (which don't watch the accent) refresh too.
            applyConfigToCurrent()
        }
        bar.addView(styleButton); bar.addView(sizeButton); bar.addView(lockButton); bar.addView(accentButton)

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

    private fun showStyle(index: Int) {
        currentOverlay?.let { overlayContainer.removeView(it) }
        val view: View = when (index) {
            0 -> FusionHudView(this).apply {
                setFpsCounter(fpsCounter)
                setEngineLabel("DXVK 2.4")
                setGpuModel("Adreno 750")
                setWineVersion("Proton 10.0-4")
                setGraphicsWrapper("FusionHUD Demo")
            }
            1 -> PerformanceHudView(this).apply {
                setFpsCounter(fpsCounter)
                setEngineLabel("DXVK 2.4")
                setGpuModel("Adreno 750")
            }
            2 -> PerfHudView(this).apply {
                setFpsCounter(fpsCounter)
                setEngineLabel("DXVK 2.4")
                setGpuModel("Adreno 750")
            }
            3 -> FrameRating(this, HashMap<String, Any>()).apply {
                setFpsCounter(fpsCounter)
                setRenderer("FusionHUD Demo GPU")
                setGpuName("Adreno 750")
            }
            else -> FrameRatingHorizontal(this).apply {
                setFpsCounter(fpsCounter)
                setRenderer("FusionHUD Demo GPU")
            }
        }
        currentOverlay = view
        overlayContainer.addView(view)
        styleButton.text = styles[index]
        applyConfigToCurrent()
    }

    /** Build the shared KV config for the current size/lock and push it to the active overlay. */
    private fun applyConfigToCurrent() {
        val cfg = Container.DEFAULT_FPS_COUNTER_CONFIG
            .replace(Regex("hudSize=[^,]*"), "hudSize=" + sizeTokens[sizeIndex])
            .replace(Regex("hudLocked=[^,]*"), "hudLocked=" + if (locked) "1" else "0")
        when (val v = currentOverlay) {
            is FusionHudView -> v.applyConfig(cfg)
            is PerformanceHudView -> v.applyConfig(cfg)
            is PerfHudView -> v.applyConfig(cfg)
            is FrameRating -> v.applyConfig(cfg)
            is FrameRatingHorizontal -> v.applyConfig(cfg)
        }
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
