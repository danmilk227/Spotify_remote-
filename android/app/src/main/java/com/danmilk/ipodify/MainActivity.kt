package com.danmilk.ipodify

import android.Manifest
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.widget.Button
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat

class MainActivity : AppCompatActivity() {

    companion object {
        var instance: MainActivity? = null
        private const val PERMISSION_REQ_CODE = 101
    }

    private lateinit var tvStatus: TextView
    private lateinit var tvPermissions: TextView
    private lateinit var btnGrantPerms: Button
    private lateinit var btnNotifAccess: Button
    private lateinit var btnToggleService: Button

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        instance = this
        setContentView(R.layout.activity_main)

        tvStatus = findViewById(R.id.tvStatus)
        tvPermissions = findViewById(R.id.tvPermissions)
        btnGrantPerms = findViewById(R.id.btnGrantPerms)
        btnNotifAccess = findViewById(R.id.btnNotifAccess)
        btnToggleService = findViewById(R.id.btnToggleService)

        btnGrantPerms.setOnClickListener {
            checkAndRequestPermissions()
        }

        btnNotifAccess.setOnClickListener {
            startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS))
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

    fun updateUI() {
        runOnUiThread {
            tvStatus.text = "Bluetooth: ${IpodifyBleService.currentStatus}\nМедиа: ${MediaListenerService.lastTrackInfo}"

            val hasNotif = isNotificationServiceEnabled()
            val hasBt = hasBluetoothPermissions()

            tvPermissions.text = buildString {
                append("Bluetooth доступ: ").append(if (hasBt) "✅ Разрешен\n" else "❌ Требуется\n")
                append("Доступ к медиа/уведомлениям: ").append(if (hasNotif) "✅ Разрешен\n" else "❌ Требуется\n")
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
