package com.danmilk.ipodify

import android.Manifest
import android.content.ClipData
import android.content.ClipboardManager
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.util.Log
import android.view.View
import android.widget.Button
import android.widget.EditText
import android.widget.ProgressBar
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.InputStream
import java.net.HttpURLConnection
import java.net.URL
import kotlin.concurrent.thread

class MainActivity : AppCompatActivity() {

    companion object {
        private const val TAG = "iPodifyMain"
        var instance: MainActivity? = null
        private const val PERMISSION_REQ_CODE = 101
    }

    private lateinit var tvStatus: TextView
    private lateinit var tvPermissions: TextView
    private lateinit var btnGrantPerms: Button
    private lateinit var btnNotifAccess: Button

    private lateinit var etClientId: EditText
    private lateinit var btnSaveClientId: Button
    private lateinit var btnCopyRedirect: Button
    private lateinit var tvSpotifyStatus: TextView
    private lateinit var btnSpotifyLogin: Button

    private lateinit var pbOta: ProgressBar
    private lateinit var tvOtaStatus: TextView
    private lateinit var btnOtaUpdate: Button

    private lateinit var btnToggleService: Button

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        instance = this
        setContentView(R.layout.activity_main)

        tvStatus = findViewById(R.id.tvStatus)
        tvPermissions = findViewById(R.id.tvPermissions)
        btnGrantPerms = findViewById(R.id.btnGrantPerms)
        btnNotifAccess = findViewById(R.id.btnNotifAccess)

        etClientId = findViewById(R.id.etClientId)
        btnSaveClientId = findViewById(R.id.btnSaveClientId)
        btnCopyRedirect = findViewById(R.id.btnCopyRedirect)
        tvSpotifyStatus = findViewById(R.id.tvSpotifyStatus)
        btnSpotifyLogin = findViewById(R.id.btnSpotifyLogin)

        pbOta = findViewById(R.id.pbOta)
        tvOtaStatus = findViewById(R.id.tvOtaStatus)
        btnOtaUpdate = findViewById(R.id.btnOtaUpdate)

        btnToggleService = findViewById(R.id.btnToggleService)

        // Load saved or default client ID
        etClientId.setText(SpotifyApiHelper.getClientId(this))

        btnSaveClientId.setOnClickListener {
            val id = etClientId.text.toString().trim()
            if (id.isNotEmpty()) {
                SpotifyApiHelper.saveClientId(this, id)
                Toast.makeText(this, "Client ID сохранен!", Toast.LENGTH_SHORT).show()
            }
        }

        btnCopyRedirect.setOnClickListener {
            val cm = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
            cm.setPrimaryClip(ClipData.newPlainText("Spotify Redirect URI", SpotifyApiHelper.REDIRECT_URI))
            Toast.makeText(this, "Redirect URI скопирован: ${SpotifyApiHelper.REDIRECT_URI}", Toast.LENGTH_SHORT).show()
        }

        btnSpotifyLogin.setOnClickListener {
            val authUrl = SpotifyApiHelper.getAuthorizeUrl(this)
            val browserIntent = Intent(Intent.ACTION_VIEW, android.net.Uri.parse(authUrl))
            startActivity(browserIntent)
        }

        btnGrantPerms.setOnClickListener {
            checkAndRequestPermissions()
        }

        btnNotifAccess.setOnClickListener {
            startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS))
        }

        btnOtaUpdate.setOnClickListener {
            startOtaWorkflow()
        }

        btnToggleService.setOnClickListener {
            val intent = Intent(this, IpodifyBleService::class.java)
            if (IpodifyBleService.instance != null) {
                stopService(intent)
                btnToggleService.text = "Запустить службу"
            } else {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                    startForegroundService(intent)
                } else {
                    startService(intent)
                }
                btnToggleService.text = "Остановить службу"
            }
            updateUI()
        }

        checkAndRequestPermissions()
        handleIntent(intent)
        updateUI()

        // Auto-start background service if permissions granted
        if (hasBluetoothPermissions()) {
            val intent = Intent(this, IpodifyBleService::class.java)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                startForegroundService(intent)
            } else {
                startService(intent)
            }
        }
    }

    override fun onResume() {
        super.onResume()
        updateUI()
    }

    override fun onDestroy() {
        super.onDestroy()
        instance = null
    }

    override fun onNewIntent(intent: Intent?) {
        super.onNewIntent(intent)
        handleIntent(intent)
    }

    private fun handleIntent(intent: Intent?) {
        if (intent?.getBooleanExtra("start_ota", false) == true) {
            Handler(Looper.getMainLooper()).postDelayed({ startOtaWorkflow() }, 1500)
        }

        val data = intent?.data ?: return
        if (data.scheme == "ipodify" && data.host == "callback") {
            val code = data.getQueryParameter("code")
            val error = data.getQueryParameter("error")

            if (!code.isNullOrEmpty()) {
                tvSpotifyStatus.text = "Обмен кода на токен..."
                SpotifyApiHelper.exchangeCodeForToken(this, code) { success, msg ->
                    runOnUiThread {
                        Toast.makeText(this@MainActivity, msg, Toast.LENGTH_LONG).show()
                        updateUI()
                    }
                }
            } else if (!error.isNullOrEmpty()) {
                Toast.makeText(this, "Ошибка авторизации: $error", Toast.LENGTH_LONG).show()
            } else {
                val fragment = data.fragment
                if (fragment != null && fragment.contains("access_token=")) {
                    val token = fragment.substringAfter("access_token=").substringBefore("&")
                    SpotifyApiHelper.saveAccessToken(this, token)
                    Toast.makeText(this, "Spotify успешно авторизован!", Toast.LENGTH_SHORT).show()
                    updateUI()
                }
            }
        }
    }

    private fun startOtaWorkflow() {
        val bleService = IpodifyBleService.instance
        if (bleService == null || !IpodifyBleService.isConnected) {
            Toast.makeText(this, "Сначала подключитесь к пульту iPodify по Bluetooth!", Toast.LENGTH_LONG).show()
            return
        }

        btnOtaUpdate.isEnabled = false
        pbOta.visibility = View.VISIBLE
        pbOta.progress = 0
        tvOtaStatus.text = "Загрузка прошивки с GitHub..."

        thread {
            try {
                // Пытаемся скачать свежий бинарник прошивки с GitHub
                val firmwareUrls = listOf(
                    "https://raw.githubusercontent.com/danmilk227/Spotify_remote-/main/firmware/firmware.bin",
                    "https://danmilk227.github.io/Spotify_remote-/firmware/firmware.bin"
                )

                var firmwareBytes: ByteArray? = null

                // 1. Проверяем локальный файл в Downloads
                try {
                    val downloadFw = File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS), "firmware.bin")
                    if (downloadFw.exists() && downloadFw.length() > 50000) {
                        firmwareBytes = downloadFw.readBytes()
                        Log.i(TAG, "Using local firmware.bin from Downloads (${firmwareBytes.size} bytes)")
                    }
                } catch (e: Exception) {
                    Log.w(TAG, "Local fw check: ${e.message}")
                }

                // 2. Если локального нет, скачиваем с GitHub
                if (firmwareBytes == null) {
                    for (urlStr in firmwareUrls) {
                        try {
                            val url = URL(urlStr)
                            val conn = url.openConnection() as HttpURLConnection
                            conn.connectTimeout = 5000
                            conn.readTimeout = 7000
                            if (conn.responseCode == 200) {
                                val baos = ByteArrayOutputStream()
                                val buffer = ByteArray(4096)
                                val input: InputStream = conn.inputStream
                                var read: Int
                                while (input.read(buffer).also { read = it } != -1) {
                                    baos.write(buffer, 0, read)
                                }
                                input.close()
                                firmwareBytes = baos.toByteArray()
                                if (firmwareBytes.size > 50000) break
                            }
                        } catch (e: Exception) {
                            // try next URL
                        }
                    }
                }

                if (firmwareBytes == null || firmwareBytes.size < 50000) {
                    runOnUiThread {
                        btnOtaUpdate.isEnabled = true
                        tvOtaStatus.text = "Ошибка: не удалось скачать бинарник с GitHub"
                        Toast.makeText(this@MainActivity, "Ошибка скачивания прошивки с GitHub", Toast.LENGTH_LONG).show()
                    }
                    return@thread
                }

                val totalSize = firmwareBytes.size
                runOnUiThread {
                    tvOtaStatus.text = "Бинарник загружен (${totalSize / 1024} KB). Передача на ESP32..."
                }

                bleService.startOtaUpdate(
                    firmwareBytes,
                    onProgress = { percent, status ->
                        runOnUiThread {
                            pbOta.progress = percent
                            tvOtaStatus.text = status
                        }
                    },
                    onComplete = { success, msg ->
                        runOnUiThread {
                            btnOtaUpdate.isEnabled = true
                            tvOtaStatus.text = msg
                            Toast.makeText(this@MainActivity, msg, Toast.LENGTH_LONG).show()
                        }
                    }
                )

            } catch (e: Exception) {
                runOnUiThread {
                    btnOtaUpdate.isEnabled = true
                    tvOtaStatus.text = "Сбой OTA: ${e.message}"
                }
            }
        }
    }

    fun updateUI() {
        runOnUiThread {
            val hasToken = SpotifyApiHelper.hasValidToken(this)
            tvStatus.text = "Bluetooth: ${IpodifyBleService.currentStatus}\nМедиа: ${MediaListenerService.lastTrackInfo}"

            if (hasToken) {
                tvSpotifyStatus.text = "Spotify токен: ✅ Авторизован (Web API активно)"
                tvSpotifyStatus.setTextColor(0xFF1DB954.toInt())
            } else {
                tvSpotifyStatus.text = "Spotify токен: ❌ Не авторизован (нажмите кнопку ниже)"
                tvSpotifyStatus.setTextColor(0xFFF59E0B.toInt())
            }

            val hasNotif = isNotificationServiceEnabled()
            val hasBt = hasBluetoothPermissions()

            tvPermissions.text = buildString {
                append("Bluetooth доступ: ").append(if (hasBt) "✅ Разрешен\n" else "❌ Требуется\n")
                append("Доступ к медиа: ").append(if (hasNotif) "✅ Разрешен\n" else "❌ Требуется\n")
            }

            btnNotifAccess.isEnabled = !hasNotif
            btnGrantPerms.isEnabled = !hasBt
            btnToggleService.text = if (IpodifyBleService.instance != null) "Остановить службу" else "Запустить службу"
        }
    }

    private fun hasBluetoothPermissions(): Boolean {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            val scan = ContextCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_SCAN) == PackageManager.PERMISSION_GRANTED
            val connect = ContextCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED
            return scan && connect
        } else {
            val loc = ContextCompat.checkSelfPermission(this, Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED
            return loc
        }
    }

    private fun isNotificationServiceEnabled(): Boolean {
        val cn = ComponentName(this, MediaListenerService::class.java)
        val flat = Settings.Secure.getString(contentResolver, "enabled_notification_listeners")
        return flat != null && flat.contains(cn.flattenToString())
    }

    private fun checkAndRequestPermissions() {
        val perms = mutableListOf<String>()

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            perms.add(Manifest.permission.BLUETOOTH_SCAN)
            perms.add(Manifest.permission.BLUETOOTH_CONNECT)
        } else {
            perms.add(Manifest.permission.ACCESS_FINE_LOCATION)
            perms.add(Manifest.permission.ACCESS_COARSE_LOCATION)
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            perms.add(Manifest.permission.POST_NOTIFICATIONS)
        }

        val missing = perms.filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }

        if (missing.isNotEmpty()) {
            ActivityCompat.requestPermissions(this, missing.toTypedArray(), PERMISSION_REQ_CODE)
        }
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        updateUI()
    }
}
